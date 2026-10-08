// The C++ runtime pieces that need no 32-bit unwinding: new/delete, atexit,
// static-init guards. Exceptions and RTTI need a real 32-bit runtime later.
#include "thunks.h"
#include <string.h>
#include <pthread.h>

static g32 w_new(uint32_t n)
{
	g32 p = gmalloc(n);
	if (!p) die("guest operator new(%u) failed", n);
	return p;
}

struct atexit_ent { g32 fn, arg; int cxa; };
static struct atexit_ent *ax;
static int nax, cap;
static pthread_mutex_t ax_lock = PTHREAD_MUTEX_INITIALIZER;
static void add_atexit(g32 fn, g32 arg, int cxa)
{
	pthread_mutex_lock(&ax_lock);
	if (nax == cap) { cap = cap ? cap * 2 : 256; ax = realloc(ax, sizeof *ax * (size_t)cap); }
	ax[nax++] = (struct atexit_ent){ fn, arg, cxa };
	pthread_mutex_unlock(&ax_lock);
}
void run_guest_atexit(void)
{
	while (nax > 0) {
		struct atexit_ent e = ax[--nax];
		if (e.cxa) gcall(e.fn, 1, e.arg);
		else gcall(e.fn, 0);
	}
}
static void t_cxa_atexit(uint32_t *args, struct hres *r) { add_atexit(A(0), A(1), 1); RET(0); }
static void t_atexit(uint32_t *args, struct hres *r) { add_atexit(A(0), 0, 0); RET(0); }

// guard: 64-bit object, first byte set once initialized
static pthread_mutex_t guard_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static void t_guard_acquire(uint32_t *args, struct hres *r)
{
	uint8_t *g = G2H(A(0));
	if (*g) { RET(0); return; }
	pthread_mutex_lock(&guard_lock);
	if (*g) { pthread_mutex_unlock(&guard_lock); RET(0); return; }
	RET(1);			// lock held until release/abort
}
static void t_guard_release(uint32_t *args, struct hres *r)
{
	(void)r;
	*(uint8_t *)G2H(A(0)) = 1;
	pthread_mutex_unlock(&guard_lock);
}
static void t_guard_abort(uint32_t *args, struct hres *r) { (void)args; (void)r; pthread_mutex_unlock(&guard_lock); }
static void t_pure_virtual(uint32_t *args, struct hres *r) { (void)args; (void)r; die("guest called a pure virtual function"); }
static void t_terminate(uint32_t *args, struct hres *r) { (void)args; (void)r; die("guest called std::terminate"); }

// dyld_func_lookup(name, void **addr): used by old crt1.o/dylib1.o glue
static void t_dyld_func_lookup(uint32_t *args, struct hres *r)
{
	const char *n = G2H(A(0));
	g32 a = host_lookup(n);
	if (!a) LOG("dyld_func_lookup(%s): not provided\n", n);
	*(g32 *)G2H(A(1)) = a;
	RET(a != 0);
}
static void t_dyld_noop(uint32_t *args, struct hres *r) { (void)args; RET(0); }

static struct hfn cxx[] = {
	G("__Znwm", w_new, "p:u"), G("__Znam", w_new, "p:u"),
	G("__Znwj", w_new, "p:u"), G("__Znaj", w_new, "p:u"),	// operator new(unsigned int)
	G("__ZdlPv", gfree, "v:p"), G("__ZdaPv", gfree, "v:p"),
	G("__ZnwmRKSt9nothrow_t", gmalloc, "p:up"), G("__ZnamRKSt9nothrow_t", gmalloc, "p:up"),
	G("__ZdlPvRKSt9nothrow_t", gfree, "v:pp"), G("__ZdaPvRKSt9nothrow_t", gfree, "v:pp"),
	R("___cxa_atexit", t_cxa_atexit), R("_atexit", t_atexit),
	R("___cxa_guard_acquire", t_guard_acquire), R("___cxa_guard_release", t_guard_release),
	R("___cxa_guard_abort", t_guard_abort),
	R("___cxa_pure_virtual", t_pure_virtual), R("__ZSt9terminatev", t_terminate),
	R("dyld_func_lookup", t_dyld_func_lookup),
	R("__dyld_make_delayed_module_initializer_calls", t_dyld_noop),
	R("__dyld_mod_term_funcs", t_dyld_noop),
};

void thunks_cxx_init(void) { REGISTER(cxx); }
