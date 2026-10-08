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
	g32 a = f ? bind_entry(f) : 0;
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
static void call_missing(uint32_t *args, struct hres *r)
{
	(void)args; (void)r;
	die("guest called unimplemented function %s", table[cur_idx]->name);
}

static void call_generic(struct hfn *h, uint32_t *a, struct hres *r)
{
	uint64_t ints[6] = {0}, stk[32];
	double fps[8] = {0};
	int ni = 0, nf = 0, ns = 0;
	const char *s = h->sig;
	char ret = s[0];
	for (s += 2; *s; s++) {
		uint64_t v;
		int fp = 0;
		switch (*s) {
		case 'i': v = (uint64_t)(int64_t)(int32_t)*a++; break;
		case 'u': case 'p': v = *a++; break;
		case 'l': v = a[0] | (uint64_t)a[1] << 32; a += 2; break;
		case 'd': memcpy(&v, a, 8); a += 2; fp = 1; break;
		case 'f': v = 0; memcpy(&v, a, 4); a++; fp = 1; break;
		default: die("bad sig %s for %s", h->sig, h->name);
		}
		if (fp && nf < 8) memcpy(&fps[nf++], &v, 8);
		else if (!fp && ni < 6) ints[ni++] = v;
		else stk[ns++] = v;
	}
	double x0 = 0;
	uint64_t rax = host_invoke(h->fn, ints, fps, stk, ns, &x0);
	switch (ret) {
	case 'v': break;
	case 'i': case 'u': r->rax = (uint32_t)rax; break;
	case 'p':
		if (rax >> 32) die("%s returned a high pointer %#llx", h->name, (unsigned long long)rax);
		r->rax = rax;
		break;
	case 'l': r->rax = rax; break;
	case 'd': r->st0 = x0; r->flags |= F_ST0; break;
	case 'f': { float f; memcpy(&f, &x0, 4); r->st0 = f; r->flags |= F_ST0; break; }
	default: die("bad return sig %s for %s", h->sig, h->name);
	}
}

int trace_calls;

void hostcall_dispatch(uint32_t idx, uint32_t *args, struct hres *r)
{
	struct hfn *h = table[idx];
	if (trace_calls) fprintf(stderr, "[call] %s\n", h->name);
	switch (h->kind) {
	case 'G': call_generic(h, args, r); break;
	case 'R': ((rawfn)h->fn)(args, r); break;
	case 'M': cur_idx = idx; call_missing(args, r); break;
	default: die("bad host entry %s", h->name);
	}
}

void thunks_init(void)
{
	thunks_libc_init();
	thunks_pthread_init();
	thunks_cxx_init();
}
