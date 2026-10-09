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
#include <setjmp.h>

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
	// A newer version than our tables know (SteamClient021): borrow the names
	// and signatures of the newest known one. Slots still come from probing
	// the game's own wrappers, which match the version it asked for; methods
	// only the newer version has stop the game with a clear message.
	size_t pl = strlen(version);
	while (pl && version[pl - 1] >= '0' && version[pl - 1] <= '9') pl--;
	const struct siface *best = NULL;
	int want = atoi(version + pl);
	for (const struct siface *i = steam_ifaces; i->cls; i++)
		if (!strncmp(i->version, version, pl) && strlen(i->version) > pl &&
		    i->version[pl] >= '0' && i->version[pl] <= '9' && atoi(i->version + pl) < want &&
		    (!best || atoi(i->version + pl) > atoi(best->version + pl)))
			best = i;	// older interfaces had other layouts: those stay unbridged
	if (best) fprintf(stderr, "[mac32] steam: %s is newer than our tables; using %s's methods\n", version, best->version);
	return best;
}

// ---- vtable layout, measured from the game's own libsteam_api ---------------
// Each SteamAPI_<Interface>_<Method> flat wrapper does `this->vtbl[k](...)`.
// Calling it once with a probe object whose vtable slots record their index
// tells us k for the SDK version this game was built with.
#define PROBE_SLOTS 256
static void steam_method(struct hfn *h, uint32_t *args, struct hres *r);
static g32 probe_obj, probe_vt, probe_args;
static __thread int probe_hit;
static __thread jmp_buf *probe_jb;
uint64_t cpu_tsd_get(void);
void cpu_tsd_set(uint64_t v);
// Record the slot and leave the wrapper at once: running the rest of it could
// go wrong (a struct-returning slot is expected to pop its hidden pointer).
static void probe_slot(struct hfn *h, uint32_t *args, struct hres *r)
{
	(void)args;
	probe_hit = (int)h->size;
	r->rax = 0;
	if (probe_jb) _longjmp(*probe_jb, 1);
}
static int probe(g32 wrapper)
{
	if (!probe_obj) {
		g32 vt = gcalloc(PROBE_SLOTS, 4);
		for (int k = 0; k < PROBE_SLOTS; k++) {
			struct hfn *f = calloc(1, sizeof *f);
			f->name = "steam:probe";
			f->kind = 'C';
			f->fn = (void *)probe_slot;
			f->size = (uint32_t)k;
			((g32 *)G2H(vt))[k] = hfn_stub(f);
		}
		probe_obj = gcalloc(1, 64);
		probe_vt = vt;
		// args: this (or a struct-return pointer) then scratch pointers
		g32 scratch = gcalloc(1, 1024);
		probe_args = gcalloc(18, 4);
		uint32_t *a = G2H(probe_args);
		a[0] = a[1] = probe_obj;
		for (int i = 2; i < 18; i++) a[i] = scratch;
	}
	memset(G2H(probe_obj), 0, 64);
	*(g32 *)G2H(probe_obj) = probe_vt;	// a struct-returning wrapper may have written over it
	probe_hit = -1;
	jmp_buf jb;
	uint64_t saved = cpu_tsd_get();
	if (!_setjmp(jb)) {
		probe_jb = &jb;
		gcallv(wrapper, 18, G2H(probe_args), NULL);
	}
	probe_jb = NULL;
	cpu_tsd_set(saved);
	return probe_hit;
}

struct layout { const struct siface *in; int n; const struct smethod **slot; g32 vtable; };
static struct layout layouts[256];
static int nlayouts;

static struct layout *layout_for(const struct siface *in)
{
	for (int i = 0; i < nlayouts; i++) if (layouts[i].in == in) return &layouts[i];
	if (nlayouts == 256) die("too many Steam interfaces");
	struct layout *L = &layouts[nlayouts++];
	L->in = in;
	const struct smethod **slot = calloc(PROBE_SLOTS, sizeof *slot);
	int n = 0, probed = 0, flat = 0;
	// the game's wrappers describe the version its libsteam_api was built for;
	// if that library has the accessor of some other version only (Timeline
	// v001 vs the engine's V004), its layout says nothing about this one
	int may_probe = 1;
	{
		const char *v = in->version + strlen(in->version);
		while (v > in->version && v[-1] >= '0' && v[-1] <= '9') v--;
		int want = atoi(v), other = 0, same = 0;
		char acc[128];
		for (int k = 1; k < 40 && *v; k++) {
			snprintf(acc, sizeof acc, "_SteamAPI_%s_v%03d", in->cls + 1, k);
			if (macho_find_any(acc)) { if (k == want) same = 1; else other = 1; }
		}
		if (other && !same) {
			may_probe = 0;
			LOG("steam: %s: the game's libsteam_api has another version; SDK order\n", in->version);
		}
	}
	for (int k = 0; k < in->n; k++) {
		const struct smethod *m = &in->m[k];
		int idx = m->slot;
		if (idx < 0 && m->flat && may_probe) {
			flat++;
			char name[256];
			snprintf(name, sizeof name, "_%s", m->flat);
			g32 w = macho_find_any(name);
			if (w && host_lookup(name) != w) {		// a guest wrapper, not one of our stubs
				idx = probe(w);
				if (idx >= 0) probed++;
			}
		}
		if (idx < 0 && probed == 0 && m->slot < 0) idx = k;	// no wrappers in this game: JSON order
		if (idx < 0 || idx >= PROBE_SLOTS) continue;
		if (getenv("MAC32_STEAMDUMP")) fprintf(stderr, "[steamdump] %s %d %s\n", in->version, idx, m->name);
		if (!slot[idx]) slot[idx] = m;
		if (idx + 1 > n) n = idx + 1;
	}
	// methods the game's library has no wrapper for (SetTimelineGameMode...):
	// JSON order is vtable order, so put one at its JSON position when that
	// slot is free and a neighbour sits at its own JSON position too
	for (int k = 0; probed && k < in->n && k < PROBE_SLOTS; k++) {
		const struct smethod *m = &in->m[k];
		int placed = 0;
		for (int j = 0; j < n && !placed; j++) placed = slot[j] == m;
		if (placed || slot[k] || m->slot >= 0) continue;
		if ((k > 0 && slot[k - 1] == &in->m[k - 1]) || (k + 1 < in->n && slot[k + 1] == &in->m[k + 1])) {
			slot[k] = m;
			if (k + 1 > n) n = k + 1;
			if (getenv("MAC32_STEAMDUMP")) fprintf(stderr, "[steamdump] %s %d %s (by order)\n", in->version, k, m->name);
		}
	}
	if (flat && !probed)
		fprintf(stderr, "[mac32] steam: %s: this libsteam_api has no flat wrappers; layout taken from "
			"the SDK description and not verified\n", in->version);
	LOG("steam: %s: %d slots, %d placed by probing\n", in->version, n, probed);
	L->n = n;
	L->slot = slot;
	return L;
}

static g32 guest_vtable(const struct siface *in)
{
	struct layout *L = layout_for(in);
	if (L->vtable) return L->vtable;
	g32 vt = gcalloc((uint32_t)(L->n ? L->n : 1), 4);
	static const struct smethod unbridged = { "private", NULL, "X:o", 0, -1 };
	for (int k = 0; k < L->n; k++) {
		const struct smethod *m = L->slot[k] ? L->slot[k] : &unbridged;
		struct hfn *f = calloc(1, sizeof *f);
		char *nm = malloc(strlen(in->version) + strlen(m->name) + 24);
		sprintf(nm, "steam:%s::%s#%d", in->version, m->name, k);
		f->name = nm;
		f->kind = 'C';
		f->fn = (void *)steam_method;
		f->sig = m->sig;
		f->size = (uint32_t)k;
		f->ctx = m;
		((g32 *)G2H(vt))[k] = hfn_stub(f);
	}
	L->vtable = vt;
	return vt;
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
			unknown[k] = (struct smethod){ n, NULL, "X:o", 0, k };
		}
		*u = (struct siface){ "unknown", strdup(version), 64, unknown };
		in = u;
	}
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

// ---- the old global accessors -----------------------------------------------
// Games built with Steamworks 1.47 or older import SteamApps(), SteamUser()...
// from libsteam_api; newer libraries (Half-Life's is from 2026) export only
// the versioned SteamAPI_SteamApps_v008() style. Steam only appends methods to
// these interfaces, so the newest version serves an older caller.
static g32 call_guest(const char *const *names, int nargs, const uint32_t *args)
{
	for (; *names; names++) {
		g32 f = macho_find_any(*names);
		if (f) return (g32)gcallv(f, nargs, args, NULL);
	}
	return 0;
}
#define ACCESSOR(fn, ...) static void fn(uint32_t *args, struct hres *r) { \
	(void)args; static const char *const n[] = { __VA_ARGS__, NULL }; RET(call_guest(n, 0, NULL)); }
ACCESSOR(t_SteamApps, "_SteamAPI_SteamApps_v008")
ACCESSOR(t_SteamUser, "_SteamAPI_SteamUser_v023", "_SteamAPI_SteamUser_v022", "_SteamAPI_SteamUser_v021", "_SteamAPI_SteamUser_v020")
ACCESSOR(t_SteamFriends, "_SteamAPI_SteamFriends_v017")
ACCESSOR(t_SteamHTTP, "_SteamAPI_SteamHTTP_v003")
ACCESSOR(t_SteamUtils, "_SteamAPI_SteamUtils_v010")
ACCESSOR(t_SteamUserStats, "_SteamAPI_SteamUserStats_v013", "_SteamAPI_SteamUserStats_v012", "_SteamAPI_SteamUserStats_v011")
ACCESSOR(t_SteamGameServer, "_SteamAPI_SteamGameServer_v015", "_SteamAPI_SteamGameServer_v014", "_SteamAPI_SteamGameServer_v013")
// old SteamGameServer_Init(ip, steamPort, gamePort, queryPort, mode, version)
// has the arguments of the new SteamInternal_GameServer_Init
static void t_SteamGameServer_Init(uint32_t *args, struct hres *r)
{
	static const char *const n[] = { "_SteamInternal_GameServer_Init", NULL };
	RET(call_guest(n, 6, args) & 0xff);
}

static struct hfn sc[] = {
	R("_SteamApps", t_SteamApps), R("_SteamUser", t_SteamUser), R("_SteamFriends", t_SteamFriends),
	R("_SteamHTTP", t_SteamHTTP), R("_SteamUtils", t_SteamUtils), R("_SteamUserStats", t_SteamUserStats),
	R("_SteamGameServer", t_SteamGameServer), R("_SteamGameServer_Init", t_SteamGameServer_Init),
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
