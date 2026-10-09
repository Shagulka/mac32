// Steam bridge: the game's own 32-bit libsteam_api talks to the real 64-bit
// Steam client. Steam keeps doing every check (login, ownership of the app).
//
// - dlopen("...steamclient.dylib") from the guest returns STEAM_HANDLE; its
//   symbols (CreateInterface, Steam_BGetCallback, ...) are host thunks into
//   the x86_64 steamclient.dylib inside the running Steam app.
// - Interface objects reach the guest as proxies: a guest block whose vptr is a
//   guest vtable of host stubs, one per method, in the i386 vtable order taken
//   from libsteam_api's flat wrappers (tools/gen_steam.py).
// - Callback payloads use #pragma pack(4) on macOS, the same layout in 32 and
//   64-bit, so they are copied as bytes; only CallbackMsg_t itself is converted.
#include "steam.h"
#include <dlfcn.h>
#include <string.h>
#include <pthread.h>

void call_sig(void *fn, const char *sig, const char *name, uint32_t *a, struct hres *r);
g32 hobj_wrap_block(void *h, uint32_t size, g32 first_word);
g32 hfn_stub(struct hfn *f);

static void *steamclient;
static pthread_mutex_t lk = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

static void *sc_sym(const char *name)
{
	if (!steamclient) {
		char p[1024];
		snprintf(p, sizeof p, "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib",
			 getenv("HOME"));
		steamclient = dlopen(p, RTLD_NOW | RTLD_LOCAL);
		if (!steamclient) die("cannot load the Steam client library: %s", dlerror());
		LOG("steam: loaded host %s\n", p);
	}
	void *f = dlsym(steamclient, name);
	if (!f) die("steamclient.dylib has no %s", name);
	return f;
}

// ---- interface proxies -------------------------------------------------------
static const struct siface *find_iface(const char *version)
{
	for (const struct siface *i = steam_ifaces; i->cls; i++)
		if (!strcmp(i->version, version)) return i;
	return NULL;
}

static g32 vtables[256];
static const struct siface *vt_iface[256];

static void steam_method(struct hfn *h, uint32_t *args, struct hres *r);

static g32 guest_vtable(const struct siface *in)
{
	for (int i = 0; i < 256; i++) {
		if (vt_iface[i] == in) return vtables[i];
		if (!vt_iface[i]) {
			g32 vt = gcalloc((uint32_t)in->n, 4);
			for (int k = 0; k < in->n; k++) {
				struct hfn *f = calloc(1, sizeof *f);
				char *nm = malloc(strlen(in->version) + strlen(in->m[k].name) + 16);
				sprintf(nm, "steam:%s::%s", in->version, in->m[k].name);
				f->name = nm;
				f->kind = 'C';
				f->fn = (void *)steam_method;
				f->sig = in->m[k].sig;
				f->size = (uint32_t)k;
				f->ctx = &in->m[k];
				((g32 *)G2H(vt))[k] = hfn_stub(f);
			}
			vt_iface[i] = in;
			vtables[i] = vt;
			return vt;
		}
	}
	die("too many Steam interfaces");
}

static g32 wrap_iface(void *host, const char *version)
{
	if (!host) return 0;
	const struct siface *in = find_iface(version);
	if (!in) {
		fprintf(stderr, "[mac32] steam: interface %s is not bridged; calls into it will stop the game\n", version);
		struct smethod *unknown = calloc(64, sizeof *unknown);
		struct siface *u = calloc(1, sizeof *u);
		for (int k = 0; k < 64; k++) {
			char *n = malloc(32);
			snprintf(n, 32, "method_%d", k);
			unknown[k] = (struct smethod){ n, "X:o", 0 };
		}
		*u = (struct siface){ "unknown", strdup(version), 64, unknown, 0 };
		in = u;
	}
	if (!in->trusted) LOG("steam: %s layout not checked against libsteam_api\n", version);
	pthread_mutex_lock(&lk);
	g32 g = hobj_wrap_block(host, 16, guest_vtable(in));
	pthread_mutex_unlock(&lk);
	LOG("steam: %s -> proxy %#x\n", version, g);
	return g;
}

// guest stack slot of 1-based parameter n (after this), given the signature
static uint32_t slot_of(const char *sig, int n)
{
	uint32_t s = 1;			// slot 0 is this
	const char *a = strchr(sig, ':') + 2;
	for (int i = 1; i < n; i++, a++) s += (*a == 'l' || *a == 'd') ? 2 : 1;
	return s;
}

static void steam_method(struct hfn *h, uint32_t *args, struct hres *r)
{
	const struct smethod *m = h->ctx;
	if (strchr(m->sig, 'X'))
		die("Steam method %s is not bridged yet (signature %s)", h->name, m->sig);
	void *self = hobj_unwrap(args[0]);
	void *fn = (*(void ***)self)[h->size];
	if (m->sig[0] == 'I') {
		char s2[64];
		snprintf(s2, sizeof s2, "h%s", m->sig + 1);	// raw host pointer back
		call_sig(fn, s2, h->name, args, r);
		const char *ver = m->ver_arg ? G2H(args[slot_of(m->sig, m->ver_arg)]) : "?";
		r->rax = wrap_iface((void *)(uintptr_t)r->rax, ver);
		return;
	}
	call_sig(fn, m->sig, h->name, args, r);
}

// ---- steamclient exports -------------------------------------------------------
static void t_CreateInterface(uint32_t *args, struct hres *r)
{
	void *(*f)(const char *, int *) = sc_sym("CreateInterface");
	const char *v = G2H(A(0));
	void *o = f(v, A(1) ? G2H(A(1)) : NULL);
	LOG("steam: CreateInterface(%s) -> %p\n", v, o);
	RET(wrap_iface(o, v));
}
// CallbackMsg_t: {HSteamUser user; int id; uint8 *param; int size} (pointer 8 bytes on host)
struct host_msg { int32_t user, id; uint8_t *param; int32_t size; };
static __thread g32 cb_buf;
static __thread uint32_t cb_cap;
static void t_Steam_BGetCallback(uint32_t *args, struct hres *r)
{
	int (*f)(int32_t, struct host_msg *) = sc_sym("Steam_BGetCallback");
	struct host_msg m;
	int ok = f((int32_t)A(0), &m) & 0xff;
	if (ok) {
		if ((uint32_t)m.size > cb_cap) {
			gfree(cb_buf);
			cb_cap = (uint32_t)m.size + 256;
			cb_buf = gmalloc(cb_cap);
		}
		memcpy(G2H(cb_buf), m.param, (size_t)m.size);
		uint32_t *g = G2H(A(1));
		g[0] = (uint32_t)m.user; g[1] = (uint32_t)m.id; g[2] = cb_buf; g[3] = (uint32_t)m.size;
	}
	RET(ok);
}
static void t_Steam_FreeLastCallback(uint32_t *args, struct hres *r)
{
	int (*f)(int32_t) = sc_sym("Steam_FreeLastCallback");
	RET(f((int32_t)A(0)) & 0xff);
}
static void t_Steam_GetAPICallResult(uint32_t *args, struct hres *r)
{
	// pipe, SteamAPICall_t (2 words), void *buf, int size, int expected, bool *failed
	int (*f)(int32_t, uint64_t, void *, int, int, uint8_t *) = sc_sym("Steam_GetAPICallResult");
	uint64_t call = A(1) | (uint64_t)A(2) << 32;
	RET(f((int32_t)A(0), call, G2H(A(3)), (int)A(4), (int)A(5), A(6) ? G2H(A(6)) : NULL) & 0xff);
}
static void t_Steam_ReleaseThreadLocalMemory(uint32_t *args, struct hres *r)
{
	void (*f)(int) = sc_sym("Steam_ReleaseThreadLocalMemory");
	f(A(0) & 0xff);
	(void)r;
}
static void t_breakpad(uint32_t *args, struct hres *r) { (void)args; RET(0); }

static struct hfn sc[] = {
	R("_mac32steam_CreateInterface", t_CreateInterface),
	R("_mac32steam_Steam_BGetCallback", t_Steam_BGetCallback),
	R("_mac32steam_Steam_FreeLastCallback", t_Steam_FreeLastCallback),
	R("_mac32steam_Steam_GetAPICallResult", t_Steam_GetAPICallResult),
	R("_mac32steam_Steam_ReleaseThreadLocalMemory", t_Steam_ReleaseThreadLocalMemory),
	R("_mac32steam_Breakpad_SteamMiniDumpInit", t_breakpad),
	R("_mac32steam_Breakpad_SteamSetAppID", t_breakpad),
	R("_mac32steam_Breakpad_SteamSetSteamID", t_breakpad),
	R("_mac32steam_Breakpad_SteamWriteMiniDumpSetComment", t_breakpad),
	R("_mac32steam_Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId", t_breakpad),
};

// dlsym(STEAM_HANDLE, name) lands here
g32 steamclient_sym(const char *name)
{
	char n[256];
	snprintf(n, sizeof n, "_mac32steam_%s", name);
	g32 a = host_lookup(n);
	if (!a) fprintf(stderr, "[mac32] steam: libsteam_api asked steamclient for %s, which is not bridged\n", name);
	return a;
}

void thunks_steam_init(void) { REGISTER(sc); }
