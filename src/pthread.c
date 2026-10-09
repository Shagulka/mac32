// pthreads for the guest. i386 sizes: mutex 44, cond 28, mutexattr 12,
// condattr 8, attr 40 bytes; pthread_t and pthread_key_t are 4 bytes.
// Guest mutex/cond objects keep their signature word and hold a pointer to
// a real host object at offset 8, made on first use, so statically
// initialized objects (PTHREAD_MUTEX_INITIALIZER) work too.
#include "thunks.h"
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>

#define OUR_MUTEX_SIG 0x4d334d58	// "XM3M"
#define OUR_COND_SIG  0x4d334358
#define SIG_MUTEX_INIT     0x32AAABA7
#define SIG_MUTEX_ERRCHK   0x32AAABA1
#define SIG_MUTEX_RECURSE  0x32AAABA2

static pthread_mutex_t create_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t *host_mutex(g32 g, int type_if_new)
{
	uint32_t *w = G2H(g);
	uint64_t hp;
	memcpy(&hp, &w[2], 8);
	if (w[0] == OUR_MUTEX_SIG && hp) return (pthread_mutex_t *)(uintptr_t)hp;
	pthread_mutex_lock(&create_lock);
	memcpy(&hp, &w[2], 8);
	if (!(w[0] == OUR_MUTEX_SIG && hp)) {
		int type = type_if_new;
		if (type < 0) type = w[0] == SIG_MUTEX_RECURSE ? PTHREAD_MUTEX_RECURSIVE
			  : w[0] == SIG_MUTEX_ERRCHK ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
		pthread_mutexattr_t a;
		pthread_mutexattr_init(&a);
		pthread_mutexattr_settype(&a, type);
		pthread_mutex_t *m = malloc(sizeof *m);
		pthread_mutex_init(m, &a);
		pthread_mutexattr_destroy(&a);
		hp = (uint64_t)(uintptr_t)m;
		memcpy(&w[2], &hp, 8);
		atomic_thread_fence(memory_order_release);
		w[0] = OUR_MUTEX_SIG;
	}
	pthread_mutex_unlock(&create_lock);
	return (pthread_mutex_t *)(uintptr_t)hp;
}

static void t_mutex_init(uint32_t *args, struct hres *r)
{
	uint32_t *w = G2H(A(0));
	int type = PTHREAD_MUTEX_NORMAL;
	if (A(1)) type = (int)((uint32_t *)G2H(A(1)))[1];
	memset(w, 0, 44);
	host_mutex(A(0), type);
	RET(0);
}
static void t_mutex_destroy(uint32_t *args, struct hres *r)
{
	uint32_t *w = G2H(A(0));
	uint64_t hp;
	memcpy(&hp, &w[2], 8);
	if (w[0] == OUR_MUTEX_SIG && hp) {
		pthread_mutex_destroy((void *)(uintptr_t)hp);
		free((void *)(uintptr_t)hp);
	}
	memset(w, 0, 44);
	RET(0);
}
static void t_mutex_lock(uint32_t *args, struct hres *r) { RET(pthread_mutex_lock(host_mutex(A(0), -1))); }
static void t_mutex_trylock(uint32_t *args, struct hres *r) { RET(pthread_mutex_trylock(host_mutex(A(0), -1))); }
static void t_mutex_unlock(uint32_t *args, struct hres *r) { RET(pthread_mutex_unlock(host_mutex(A(0), -1))); }

// mutexattr: [sig][type][pshared]
static void t_mutexattr_init(uint32_t *args, struct hres *r) { uint32_t *w = G2H(A(0)); w[0] = 0x4d544154; w[1] = PTHREAD_MUTEX_NORMAL; w[2] = 0; RET(0); }
static void t_mutexattr_destroy(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_mutexattr_settype(uint32_t *args, struct hres *r) { ((uint32_t *)G2H(A(0)))[1] = A(1); RET(0); }
static void t_mutexattr_gettype(uint32_t *args, struct hres *r) { *(uint32_t *)G2H(A(1)) = ((uint32_t *)G2H(A(0)))[1]; RET(0); }
static void t_mutexattr_setpshared(uint32_t *args, struct hres *r) { ((uint32_t *)G2H(A(0)))[2] = A(1); RET(0); }

static pthread_cond_t *host_cond(g32 g)
{
	uint32_t *w = G2H(g);
	uint64_t hp;
	memcpy(&hp, &w[2], 8);
	if (w[0] == OUR_COND_SIG && hp) return (pthread_cond_t *)(uintptr_t)hp;
	pthread_mutex_lock(&create_lock);
	memcpy(&hp, &w[2], 8);
	if (!(w[0] == OUR_COND_SIG && hp)) {
		pthread_cond_t *c = malloc(sizeof *c);
		pthread_cond_init(c, NULL);
		hp = (uint64_t)(uintptr_t)c;
		memcpy(&w[2], &hp, 8);
		atomic_thread_fence(memory_order_release);
		w[0] = OUR_COND_SIG;
	}
	pthread_mutex_unlock(&create_lock);
	return (pthread_cond_t *)(uintptr_t)hp;
}
static void t_cond_init(uint32_t *args, struct hres *r) { memset(G2H(A(0)), 0, 28); host_cond(A(0)); RET(0); }
static void t_cond_destroy(uint32_t *args, struct hres *r)
{
	uint32_t *w = G2H(A(0));
	uint64_t hp;
	memcpy(&hp, &w[2], 8);
	if (w[0] == OUR_COND_SIG && hp) {
		pthread_cond_destroy((void *)(uintptr_t)hp);
		free((void *)(uintptr_t)hp);
	}
	memset(w, 0, 28);
	RET(0);
}
static void t_cond_signal(uint32_t *args, struct hres *r) { RET(pthread_cond_signal(host_cond(A(0)))); }
static void t_cond_broadcast(uint32_t *args, struct hres *r) { RET(pthread_cond_broadcast(host_cond(A(0)))); }
static void t_cond_wait(uint32_t *args, struct hres *r) { RET(pthread_cond_wait(host_cond(A(0)), host_mutex(A(1), -1))); }
static void t_cond_timedwait(uint32_t *args, struct hres *r)
{
	int32_t *g = G2H(A(2));
	struct timespec ts = { g[0], g[1] };
	RET(pthread_cond_timedwait(host_cond(A(0)), host_mutex(A(1), -1), &ts));
}
static void t_condattr_init(uint32_t *args, struct hres *r) { memset(G2H(A(0)), 0, 8); RET(0); }
static void t_condattr_destroy(uint32_t *args, struct hres *r) { (void)args; RET(0); }

// ---- threads ---------------------------------------------------------------
// Guest pthread_t is a pointer to this 16-byte guest record.
struct gthread { uint64_t host; uint32_t retval; uint32_t pad; };
static __thread g32 self_rec;

static g32 thread_record(pthread_t t)
{
	g32 g = gcalloc(1, sizeof(struct gthread));
	((struct gthread *)G2H(g))->host = (uint64_t)(uintptr_t)t;
	return g;
}
static pthread_t rec_host(g32 g) { return (pthread_t)(uintptr_t)((struct gthread *)G2H(g))->host; }

struct start { g32 fn, arg, rec; uint32_t stack; dispatch_semaphore_t go; };

// threads made by pthread_create_suspended_np wait here until thread_resume
static pthread_mutex_t susp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { mach_port_t port; dispatch_semaphore_t go; } suspended[64];

static void *thread_main(void *p)
{
	struct start s = *(struct start *)p;
	free(p);
	if (s.go) dispatch_semaphore_wait(s.go, DISPATCH_TIME_FOREVER);
	cpu_thread_init(s.stack);
	self_rec = s.rec;
	uint32_t rv = (uint32_t)gcall(s.fn, 1, s.arg);
	return (void *)(uintptr_t)rv;
}

// attr: [sig][stacksize][detach][...]
#define ATTR_SIG 0x41545452
static void t_attr_init(uint32_t *args, struct hres *r) { uint32_t *w = G2H(A(0)); memset(w, 0, 40); w[0] = ATTR_SIG; w[1] = 512 << 10; w[2] = PTHREAD_CREATE_JOINABLE; RET(0); }
static void t_attr_destroy(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_attr_setstacksize(uint32_t *args, struct hres *r) { ((uint32_t *)G2H(A(0)))[1] = A(1); RET(0); }
static void t_attr_getstacksize(uint32_t *args, struct hres *r) { *(uint32_t *)G2H(A(1)) = ((uint32_t *)G2H(A(0)))[1]; RET(0); }
static void t_attr_setdetachstate(uint32_t *args, struct hres *r) { ((uint32_t *)G2H(A(0)))[2] = A(1); RET(0); }
static void t_attr_noop(uint32_t *args, struct hres *r) { (void)args; RET(0); }

static void create_thread(uint32_t *args, struct hres *r, int suspended_start);
static void t_create(uint32_t *args, struct hres *r) { create_thread(args, r, 0); }
static void t_create_suspended(uint32_t *args, struct hres *r) { create_thread(args, r, 1); }
static void t_thread_resume(uint32_t *args, struct hres *r)
{
	pthread_mutex_lock(&susp_lock);
	for (int i = 0; i < 64; i++)
		if (suspended[i].go && suspended[i].port == A(0)) {
			dispatch_semaphore_signal(suspended[i].go);
			suspended[i].go = NULL;
			pthread_mutex_unlock(&susp_lock);
			RET(0);
			return;
		}
	pthread_mutex_unlock(&susp_lock);
	RET(thread_resume(A(0)));
}
static void t_thread_policy_set(uint32_t *args, struct hres *r) { (void)args; RET(0); }	// scheduling hints: ignored

static void create_thread(uint32_t *args, struct hres *r, int suspended_start)
{
	struct start *s = calloc(1, sizeof *s);
	if (suspended_start) s->go = dispatch_semaphore_create(0);
	uint32_t stack = 512 << 10;
	int detach = PTHREAD_CREATE_JOINABLE;
	if (A(1)) {
		uint32_t *a = G2H(A(1));
		if (a[0] == ATTR_SIG) { stack = a[1]; detach = (int)a[2]; }
	}
	if (stack < (256 << 10)) stack = 256 << 10;
	s->fn = A(2);
	s->arg = A(3);
	s->stack = stack;
	s->rec = gcalloc(1, sizeof(struct gthread));
	pthread_attr_t ha;
	pthread_attr_init(&ha);
	pthread_attr_setdetachstate(&ha, detach);
	pthread_t t;
	g32 rec = s->rec;
	int rc = pthread_create(&t, &ha, thread_main, s);
	pthread_attr_destroy(&ha);
	if (rc) { RET(rc); return; }	// s is freed by nobody: rare
	((struct gthread *)G2H(rec))->host = (uint64_t)(uintptr_t)t;
	*(g32 *)G2H(A(0)) = rec;
	if (suspended_start) {
		pthread_mutex_lock(&susp_lock);
		for (int i = 0; i < 64; i++)
			if (!suspended[i].go) { suspended[i].port = pthread_mach_thread_np(t); suspended[i].go = s->go; break; }
		pthread_mutex_unlock(&susp_lock);
	}
	RET(0);
}
static void t_join(uint32_t *args, struct hres *r)
{
	void *rv;
	int rc = pthread_join(rec_host(A(0)), &rv);
	if (!rc && A(1)) *(g32 *)G2H(A(1)) = (g32)(uintptr_t)rv;
	RET(rc);
}
static void t_detach(uint32_t *args, struct hres *r) { RET(pthread_detach(rec_host(A(0)))); }
static void t_self(uint32_t *args, struct hres *r)
{
	(void)args;
	if (!self_rec) self_rec = thread_record(pthread_self());
	RET(self_rec);
}
static void t_equal(uint32_t *args, struct hres *r) { RET(A(0) == A(1) || rec_host(A(0)) == rec_host(A(1))); }
static void t_kill(uint32_t *args, struct hres *r) { RET(pthread_kill(rec_host(A(0)), (int)A(1))); }
static void t_getschedparam(uint32_t *args, struct hres *r)
{
	int pol;
	struct sched_param sp;
	int rc = pthread_getschedparam(rec_host(A(0)), &pol, &sp);
	if (!rc) { *(int32_t *)G2H(A(1)) = pol; memcpy(G2H(A(2)), &sp, 8); }
	RET(rc);
}
static void t_setschedparam(uint32_t *args, struct hres *r)
{
	struct sched_param sp;
	memcpy(&sp, G2H(A(2)), 8);
	RET(pthread_setschedparam(rec_host(A(0)), (int)A(1), &sp));
}
static void t_mach_thread_np(uint32_t *args, struct hres *r) { RET(pthread_mach_thread_np(rec_host(A(0)))); }
static void t_setname_np(uint32_t *args, struct hres *r) { RET(pthread_setname_np(G2H(A(0)))); }

// keys: guest values are 32 bits, stored zero-extended
static void t_key_create(uint32_t *args, struct hres *r)
{
	pthread_key_t k;
	int rc = pthread_key_create(&k, NULL);	// TODO: guest destructors
	if (!rc) *(uint32_t *)G2H(A(0)) = (uint32_t)k;
	RET(rc);
}
static void t_key_delete(uint32_t *args, struct hres *r) { RET(pthread_key_delete(A(0))); }
static void t_getspecific(uint32_t *args, struct hres *r) { RET((uint32_t)(uintptr_t)pthread_getspecific(A(0))); }
static void t_setspecific(uint32_t *args, struct hres *r) { RET(pthread_setspecific(A(0), (void *)(uintptr_t)A(1))); }

// pthread_once: guest pthread_once_t is {sig, 4 bytes}
static pthread_mutex_t once_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static void t_once(uint32_t *args, struct hres *r)
{
	uint32_t *o = G2H(A(0));
	if (o[1] == 0x4f4e4345) { RET(0); return; }
	pthread_mutex_lock(&once_lock);
	if (o[1] != 0x4f4e4345) {
		gcall(A(1), 0);
		o[1] = 0x4f4e4345;
	}
	pthread_mutex_unlock(&once_lock);
	RET(0);
}

static void t_yield(uint32_t *args, struct hres *r) { (void)args; (void)r; sched_yield(); }

static struct hfn pth[] = {
	R("_pthread_mutex_init", t_mutex_init), R("_pthread_mutex_destroy", t_mutex_destroy),
	R("_pthread_mutex_lock", t_mutex_lock), R("_pthread_mutex_trylock", t_mutex_trylock),
	R("_pthread_mutex_unlock", t_mutex_unlock),
	R("_pthread_mutexattr_init", t_mutexattr_init), R("_pthread_mutexattr_destroy", t_mutexattr_destroy),
	R("_pthread_mutexattr_settype", t_mutexattr_settype), R("_pthread_mutexattr_gettype", t_mutexattr_gettype),
	R("_pthread_mutexattr_setpshared", t_mutexattr_setpshared),
	R("_pthread_cond_init", t_cond_init), R("_pthread_cond_destroy", t_cond_destroy),
	R("_pthread_cond_signal", t_cond_signal), R("_pthread_cond_broadcast", t_cond_broadcast),
	R("_pthread_cond_wait", t_cond_wait), R("_pthread_cond_timedwait", t_cond_timedwait),
	R("_pthread_condattr_init", t_condattr_init), R("_pthread_condattr_destroy", t_condattr_destroy),
	R("_pthread_attr_init", t_attr_init), R("_pthread_attr_destroy", t_attr_destroy),
	R("_pthread_attr_setstacksize", t_attr_setstacksize), R("_pthread_attr_getstacksize", t_attr_getstacksize),
	R("_pthread_attr_setdetachstate", t_attr_setdetachstate),
	R("_pthread_attr_setschedparam", t_attr_noop), R("_pthread_attr_setschedpolicy", t_attr_noop),
	R("_pthread_attr_setscope", t_attr_noop), R("_pthread_attr_setinheritsched", t_attr_noop),
	R("_pthread_create", t_create), R("_pthread_create_suspended_np", t_create_suspended),
	R("_thread_resume", t_thread_resume), R("_thread_policy_set", t_thread_policy_set), R("_pthread_join", t_join), R("_pthread_detach", t_detach),
	R("_pthread_self", t_self), R("_pthread_equal", t_equal), R("_pthread_kill", t_kill),
	R("_pthread_getschedparam", t_getschedparam), R("_pthread_setschedparam", t_setschedparam),
	R("_pthread_mach_thread_np", t_mach_thread_np), R("_pthread_setname_np", t_setname_np),
	R("_pthread_key_create", t_key_create), R("_pthread_key_delete", t_key_delete),
	R("_pthread_getspecific", t_getspecific), R("_pthread_setspecific", t_setspecific),
	R("_pthread_once", t_once),
	R("_pthread_yield_np", t_yield), R("_sched_yield", t_yield),
};

void thunks_pthread_init(void) { REGISTER(pth); }
