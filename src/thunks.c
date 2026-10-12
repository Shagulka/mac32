// Host functions the guest can import, and the dispatcher that calls them.
//
// Guest memory is ordinary process memory below 4 GB, so guest pointers work
// as host pointers. What needs converting: argument widths (the "sig"
// strings), structs whose layout differs between i386 and x86_64, pointers
// the host returns (they must be < 4 GB), and varargs.
//
// sig: "<ret>:<args>". Arg codes: i int32 (sign-extended), u uint32, p pointer,
// l 64-bit int (two slots), d double (two slots), f float.
// Return codes: v, i, u, p, l, d, f (d and f come back in st(0)).
#include "mac32.h"
#include "thunks.h"
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <os/lock.h>

uint64_t host_invoke(void *fn, const uint64_t ints[6], const double fps[8],
		     const uint64_t *stk, long nstk, double *xmm0_out);
g32 make_stub(uint32_t idx);

static struct hfn *table[MAX_HFN];
static uint32_t nhfn;
static os_unfair_lock tlock = OS_UNFAIR_LOCK_INIT;

// name -> entry hash
#define HBITS 14
static struct hfn *hash[1 << HBITS];
static uint32_t hname(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
	return h & ((1 << HBITS) - 1);
}
static struct hfn *hfind(const char *name)
{
	for (uint32_t h = hname(name);; h = (h + 1) & ((1 << HBITS) - 1)) {
		if (!hash[h]) return NULL;
		if (!strcmp(hash[h]->name, name)) return hash[h];
	}
}
static void hadd(struct hfn *f)
{
	uint32_t h = hname(f->name);
	while (hash[h]) h = (h + 1) & ((1 << HBITS) - 1);
	hash[h] = f;
}

void thunk_register(struct hfn *list, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (hfind(list[i].name)) die("duplicate host symbol %s", list[i].name);
		hadd(&list[i]);
	}
}

static g32 bind_entry(struct hfn *f)
{
	if (f->kind == 'D') {
		if (!f->addr) {
			f->addr = gcalloc(1, f->size ? f->size : 4);
			if (f->init) f->init(f->addr);
		}
		return f->addr;
	}
	if (!f->addr) {
		f->idx = nhfn;
		table[nhfn++] = f;
		f->addr = make_stub(f->idx);
	}
	return f->addr;
}

g32 host_lookup(const char *name)
{
	os_unfair_lock_lock(&tlock);
	struct hfn *f = hfind(name);
	if (!f) {
		// libSystem symbol variants ($UNIX2003, $NOCANCEL, ...) share a body
		const char *d = strchr(name, '$');
		if (d && strncmp(d, "$INODE64", 8)) {
			char base[256];
			snprintf(base, sizeof base, "%.*s", (int)(d - name), name);
			f = hfind(base);
		}
	}
	// failure stubs made for an earlier image do not count as provided
	g32 a = f && f->kind != 'M' ? bind_entry(f) : 0;
	os_unfair_lock_unlock(&tlock);
	return a;
}

static void call_missing(uint32_t *args, struct hres *r);

g32 missing_stub(const char *name)
{
	os_unfair_lock_lock(&tlock);
	struct hfn *f = hfind(name);
	if (!f) {
		f = calloc(1, sizeof *f);
		f->name = strdup(name);
		f->kind = 'M';
		hadd(f);
	}
	g32 a = bind_entry(f);
	os_unfair_lock_unlock(&tlock);
	return a;
}

const char *hostcall_name(uint32_t idx) { return idx < nhfn ? table[idx]->name : "?"; }

static __thread uint32_t cur_idx;
void guest_backtrace(g32 ret, g32 ebp);
static void call_missing(uint32_t *args, struct hres *r)
{
	(void)r;
	// host-call frame: [ebx][edi][esi][ebp][idx][ret][args...]
	fprintf(stderr, "[mac32] guest called unimplemented function %s\n", table[cur_idx]->name);
	guest_backtrace(args[-1], args[-3]);
	die("unimplemented: %s", table[cur_idx]->name);
}

void call_sig(void *fn, const char *sig, const char *name, uint32_t *a, struct hres *r);
static void call_generic(struct hfn *h, uint32_t *a, struct hres *r) { call_sig(h->fn, h->sig, h->name, a, r); }

// a dynamically made entry (e.g. one Steam interface method); returns its stub
g32 hfn_stub(struct hfn *f)
{
	os_unfair_lock_lock(&tlock);
	g32 a = bind_entry(f);
	os_unfair_lock_unlock(&tlock);
	return a;
}

// CoreGraphics types change size between i386 and x86_64 (CGFloat is float on
// i386, double here). Codes, in arguments and returns:
//   g CGFloat   P CGPoint/CGSize (2 floats)   R CGRect (4)   A CGAffineTransform (6)
//   N CFRange (two CFIndex: 4 bytes each on i386, 8 here; rax:rdx as a return)
// Pointer arguments whose pointee changes size, copied in and back out:
//   r CGRect * (HIRect *)   O object out-pointer (CFURLRef *): result wrapped
//   W pointer to a long-sized count (ItemCount *, Size *, CFIndex *)
// P travels in two SSE registers on the host; R and A go through memory
// (on the stack as arguments, through a hidden pointer as returns, which on
// i386 is the first guest argument and is popped by the callee).
static int fp_count(char c) { return c == 'P' ? 2 : c == 'R' ? 4 : c == 'A' ? 6 : 0; }

void call_sig(void *fn, const char *sig, const char *name, uint32_t *a, struct hres *r)
{
	struct { const char *name, *sig; } hh = { name, sig }, *h = &hh;
	uint64_t ints[6] = {0}, stk[48];
	g32 qslot[4];
	char *qval[4];
	int nq = 0;
	double fps[8] = {0};
	int ni = 0, nf = 0, ns = 0;
	const char *s = h->sig;
	char ret = s[0];
	double hret[6];					// R/A results land here
	struct { char kind; g32 g; union { double rect[4]; void *obj; uint64_t w; } h; } fix[8];
	int nfix = 0;
	g32 gret = 0;
	if (ret == 'R' || ret == 'A') {
		gret = *a++;				// the guest's result block
		ints[ni++] = (uint64_t)(uintptr_t)hret;	// the host's hidden result pointer
		r->flags |= F_POP4;
	}
	for (s += 2; *s; s++) {
		uint64_t v;
		int fp = 0;
		switch (*s) {
		case 'i': v = (uint64_t)(int64_t)(int32_t)*a++; break;
		case 'u': case 'p': v = *a++; break;
		case 'o': v = (uint64_t)(uintptr_t)hobj_unwrap(*a++); break;
		case 'B': v = *a++ & 0xff; break;			// bool in a 4-byte slot
		case 'Q':						// char ** out: host writes its own pointer
			if (nq == 4) die("%s: too many string out-params", name);
			qslot[nq] = *a++;
			qval[nq] = NULL;
			v = qslot[nq] ? (uint64_t)(uintptr_t)&qval[nq] : 0;
			nq++;
			break;
		case 'F':						// callback: not bridged, pass NULL
			if (*a) LOG("%s: guest callback %#x not passed to the host\n", name, *a);
			a++; v = 0; break;
		case 'l': v = a[0] | (uint64_t)a[1] << 32; a += 2; break;
		case 'd': memcpy(&v, a, 8); a += 2; fp = 1; break;
		case 'f': v = 0; memcpy(&v, a, 4); a++; fp = 1; break;
		case 'g': { float f; memcpy(&f, a, 4); a++; double d = f; memcpy(&v, &d, 8); fp = 1; break; }
		case 'N': {						// CFRange: two integer registers
			uint64_t lo = (uint64_t)(int64_t)(int32_t)a[0], ln = (uint64_t)(int64_t)(int32_t)a[1];
			a += 2;
			if (ni + 2 <= 6) { ints[ni++] = lo; ints[ni++] = ln; }
			else { stk[ns++] = lo; stk[ns++] = ln; }
			continue;
		}
		case 'r': case 'O': case 'W': {				// converted through a host temporary
			g32 g = *a++;
			if (!g) { v = 0; break; }
			if (nfix == 8) die("%s: too many converted pointers", name);
			fix[nfix].kind = *s;
			fix[nfix].g = g;
			if (*s == 'r') { const float *f = G2H(g); for (int k = 0; k < 4; k++) fix[nfix].h.rect[k] = f[k]; }
			else if (*s == 'O') fix[nfix].h.obj = NULL;
			else fix[nfix].h.w = *(uint32_t *)G2H(g);
			v = (uint64_t)(uintptr_t)&fix[nfix].h;
			nfix++;
			break;
		}
		case 'P': case 'R': case 'A': {
			int n = fp_count(*s);
			double d[6];
			for (int k = 0; k < n; k++) { float f; memcpy(&f, a++, 4); d[k] = f; }
			if (*s == 'P' && nf + 2 <= 8) { fps[nf++] = d[0]; fps[nf++] = d[1]; }
			else for (int k = 0; k < n; k++) memcpy(&stk[ns++], &d[k], 8);	// in memory, in order
			continue;
		}
		default: die("bad sig %s for %s", h->sig, h->name);
		}
		if (fp && nf < 8) memcpy(&fps[nf++], &v, 8);
		else if (!fp && ni < 6) ints[ni++] = v;
		else stk[ns++] = v;
	}
	double x[3] = { 0, 0, 0 };				// xmm0, xmm1, rdx
	uint64_t rax = host_invoke(fn, ints, fps, stk, ns, x);
	for (int i = 0; i < nq; i++)
		if (qslot[i]) *(g32 *)G2H(qslot[i]) = qval[i] ? gstrdup(qval[i]) : 0;	// guest copy; leaks a little
	for (int i = 0; i < nfix; i++) {
		if (fix[i].kind == 'r') { float *f = G2H(fix[i].g); for (int k = 0; k < 4; k++) f[k] = (float)fix[i].h.rect[k]; }
		else if (fix[i].kind == 'O') *(g32 *)G2H(fix[i].g) = hobj_wrap(fix[i].h.obj);
		else *(uint32_t *)G2H(fix[i].g) = (uint32_t)fix[i].h.w;
	}
	switch (ret) {
	case 'v': break;
	case 'i': case 'u': r->rax = (uint32_t)rax; break;
	case 'p':
		if (rax >> 32) die("%s returned a high pointer %#llx", h->name, (unsigned long long)rax);
		r->rax = rax;
		break;
	case 'l': r->rax = rax; break;
	case 'o': r->rax = hobj_wrap((void *)(uintptr_t)rax); break;
	case 'b': r->rax = rax & 0xff; break;	// GLboolean / Boolean: junk above the low byte
	case 'h': r->rax = rax; break;		// raw host pointer, the caller converts it
	case 'S': r->rax = rax ? guest_cstr((const char *)(uintptr_t)rax) : 0; break;
	case 'd': r->st0 = x[0]; r->flags |= F_ST0; break;
	case 'f': { float f; memcpy(&f, &x[0], 4); r->st0 = f; r->flags |= F_ST0; break; }
	case 'g': r->st0 = x[0]; r->flags |= F_ST0; break;	// x87 st(0) holds it at any width
	case 'P': { float f[2] = { (float)x[0], (float)x[1] }; memcpy(&r->rax, f, 8); break; }	// eax:edx
	case 'N': { uint64_t hi; memcpy(&hi, &x[2], 8); r->rax = (uint32_t)rax | (uint64_t)(uint32_t)hi << 32; break; }
	case 'R': case 'A': {
		float *g = G2H(gret);
		for (int k = 0; k < fp_count(ret); k++) g[k] = (float)hret[k];
		r->rax = gret;
		break;
	}
	default: die("bad return sig %s for %s", h->sig, h->name);
	}
}

int trace_calls;

// MAC32_TRACE=sub1,sub2: with -t, only trace calls whose name contains one of them
int trace_match(const char *name)
{
	static char *filters[16];
	static int nf = -1;
	if (nf < 0) {
		nf = 0;
		const char *e = getenv("MAC32_TRACE");
		if (e) {
			char *c = strdup(e), *t;
			while ((t = strsep(&c, ",")) && nf < 16) if (*t) filters[nf++] = t;
		}
	}
	if (!nf) return 1;
	for (int i = 0; i < nf; i++) if (strstr(name, filters[i])) return 1;
	return 0;
}

// the last host calls of each thread, printed by the crash handler
#define RING 32
static __thread uint32_t ring[RING], ring_n;
void print_recent_calls(void)
{
	fprintf(stderr, "  last host calls on this thread (oldest first):\n   ");
	for (uint32_t i = ring_n > RING ? ring_n - RING : 0; i < ring_n; i++)
		fprintf(stderr, " %s", table[ring[i % RING]]->name);
	fprintf(stderr, "\n");
}

void hostcall_dispatch(uint32_t idx, uint32_t *args, struct hres *r)
{
	struct hfn *h = table[idx];
	ring[ring_n++ % RING] = idx;
	if (trace_calls && trace_match(h->name)) fprintf(stderr, "[call] %s\n", h->name);
	switch (h->kind) {
	case 'G': call_generic(h, args, r); break;
	case 'R': ((rawfn)h->fn)(args, r); break;
	case 'M': cur_idx = idx; call_missing(args, r); break;
	case 'C': ((void (*)(struct hfn *, uint32_t *, struct hres *))h->fn)(h, args, r); break;
	default: die("bad host entry %s", h->name);
	}
}

void thunks_init(void)
{
	thunks_libc_init();
	thunks_pthread_init();
	thunks_cxx_init();
	thunks_libc2_init();
	thunks_objc_init();
	thunks_apple_init();
	thunks_gl_init();
	thunks_libc3_init();
	thunks_ats_init();
	thunks_steam_init();
	thunks_zlib_init();
	thunks_audio_init();
	thunks_sndmgr_init();
	thunks_openal_init();
	thunks_apple2_init();
	thunks_legacy_init();
	thunks_cg_init();
	thunks_cf_init();
	thunks_carbon_init();
	thunks_qd_init();
}
