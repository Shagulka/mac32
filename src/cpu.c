// CPU glue: the 32-bit code selector, the low trampoline page, import stubs,
// guest stacks per thread, and calls in both directions.
#include "mac32.h"
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <execinfo.h>
#include <i386/user_ldt.h>
#include <architecture/i386/table.h>

extern char t_start[], t_end[], t_call32[], t_hostcall32[];
extern char t_cs32[], t_tsd_off[], t_dispatch[];

static g32 tramp;			// low copy of t_start..t_end
#define TADDR(sym) (tramp + (g32)((sym) - t_start))
static uint64_t (*call32)(g32 eip, g32 esp, double *st0);

static g32 stubs;			// 10-byte "push idx; jmp hostcall" stubs
#define STUB_SIZE 10
#define MAX_STUBS 16384

static pthread_key_t tsd_key;		// slot read by the trampoline via %gs
static __thread g32 guest_stack_top;

// each thread's guest stack goes back when the thread ends (GCD retires
// idle worker threads all the time, and each one that ran guest code has one)
static pthread_key_t stack_key;
static void free_stack(void *base)
{
	low_free((g32)(uintptr_t)base);
	guest_stack_top = 0;	// a guest key destructor that runs later gets a new one
}


void hostcall_dispatch(uint32_t idx, uint32_t *args, struct hres *r);

// read guest memory without faulting inside the signal handler
static int safe_read32(g32 a, g32 *out)
{
	vm_size_t n = 4;
	return vm_read_overwrite(mach_task_self(), a, 4, (vm_address_t)out, &n) == KERN_SUCCESS;
}

static void crash(int sig, siginfo_t *si, void *ucv)
{
	ucontext_t *uc = ucv;
	uint64_t ip = uc->uc_mcontext->__ss.__rip;
	g32 off = 0;
	const char *where = ip < 0x100000000ull ? macho_addr_name((g32)ip, &off) : NULL;
	fprintf(stderr, "\n[mac32] signal %d at %#llx", sig, (unsigned long long)ip);
	if (where) fprintf(stderr, " (%s+%#x)", where, off);
	fprintf(stderr, ", fault addr %p, cs %#llx\n", si->si_addr,
		(unsigned long long)uc->uc_mcontext->__ss.__cs);
	if ((uintptr_t)si->si_addr < LOW_END) low_describe((g32)(uintptr_t)si->si_addr);
	if (uc->uc_mcontext->__ss.__rsp < LOW_END) {
		fprintf(stderr, "  stack pointer: ");
		low_describe((g32)uc->uc_mcontext->__ss.__rsp);
	}
	fprintf(stderr, "  eax %08llx ebx %08llx ecx %08llx edx %08llx\n",
		uc->uc_mcontext->__ss.__rax & 0xffffffff, uc->uc_mcontext->__ss.__rbx & 0xffffffff,
		uc->uc_mcontext->__ss.__rcx & 0xffffffff, uc->uc_mcontext->__ss.__rdx & 0xffffffff);
	fprintf(stderr, "  esi %08llx edi %08llx ebp %08llx esp %08llx\n",
		uc->uc_mcontext->__ss.__rsi & 0xffffffff, uc->uc_mcontext->__ss.__rdi & 0xffffffff,
		uc->uc_mcontext->__ss.__rbp & 0xffffffff, uc->uc_mcontext->__ss.__rsp & 0xffffffff);
	if (ip >> 32) {		// host code: print the host stack too
		void *bt[64];
		int n = backtrace(bt, 64);
		backtrace_symbols_fd(bt, n, 2);
	}
	extern void print_recent_calls(void);
	print_recent_calls();
	// walk the guest ebp chain
	g32 ebp = (g32)uc->uc_mcontext->__ss.__rbp;
	for (int i = 0; i < 16 && ebp > 0x1000 && ebp < LOW_END; i++) {
		g32 ret, next;
		if (!safe_read32(ebp + 4, &ret) || !safe_read32(ebp, &next)) break;
		const char *n = macho_addr_name(ret, &off);
		fprintf(stderr, "  #%d %08x %s+%#x\n", i, ret, n ? n : "?", off);
		ebp = next;
	}
	_exit(128 + sig);
}

void guest_backtrace(g32 ret, g32 ebp)
{
	g32 off;
	const char *n = macho_addr_name(ret, &off);
	fprintf(stderr, "  called from %08x %s+%#x\n", ret, n ? n : "?", off);
	for (int i = 0; i < 16 && ebp > 0x1000 && ebp < LOW_END; i++) {
		g32 r, next;
		if (!safe_read32(ebp + 4, &r) || !safe_read32(ebp, &next)) break;
		n = macho_addr_name(r, &off);
		fprintf(stderr, "  #%d %08x %s+%#x\n", i, r, n ? n : "?", off);
		ebp = next;
	}
}

void cpu_init(void)
{
	// flat 32-bit code segment in the LDT. Rosetta wants an explicit index.
	union ldt_entry d;
	memset(&d, 0, sizeof d);
	d.code.limit00 = 0xffff;
	d.code.limit16 = 0xf;
	d.code.type = DESC_CODE_READ;
	d.code.dpl = 3;
	d.code.present = 1;
	d.code.opsz = 1;
	d.code.granular = 1;
	int sel = -1;
	for (int i = 16; i < 8192 && sel < 0; i++)
		sel = i386_set_ldt(i, &d, 1);
	if (sel < 0) die("i386_set_ldt failed");

	if (pthread_key_create(&tsd_key, NULL)) die("pthread_key_create");
	if (pthread_key_create(&stack_key, free_stack)) die("pthread_key_create");

	size_t tlen = t_end - t_start;
	tramp = low_pages((uint32_t)tlen, PROT_READ | PROT_WRITE);
	memcpy(G2H(tramp), t_start, tlen);
	*(uint16_t *)G2H(TADDR(t_cs32)) = (uint16_t)((sel << 3) | 7);
	*(uint64_t *)G2H(TADDR(t_tsd_off)) = (uint64_t)tsd_key * 8;
	*(uint64_t *)G2H(TADDR(t_dispatch)) = (uint64_t)(uintptr_t)hostcall_dispatch;
	if (mprotect(G2H(tramp & ~0xfffu), (tlen + 0xfff) & ~0xfffu,
		     PROT_READ | PROT_WRITE | PROT_EXEC))
		die("mprotect trampoline");
	call32 = (void *)G2H(TADDR(t_call32));

	stubs = low_pages(MAX_STUBS * STUB_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
	LOG("cs32 %#x, trampoline %#x, stubs %#x\n", (sel << 3) | 7, tramp, stubs);

	struct sigaction sa = { .sa_sigaction = crash, .sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK };
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
}

g32 make_stub(uint32_t idx)
{
	if (idx >= MAX_STUBS) die("too many host stubs");
	g32 s = stubs + idx * STUB_SIZE;
	uint8_t *p = G2H(s);
	p[0] = 0x68;				// push imm32
	memcpy(p + 1, &idx, 4);
	p[5] = 0xe9;				// jmp rel32
	int32_t rel = (int32_t)(TADDR(t_hostcall32) - (s + 10));
	memcpy(p + 6, &rel, 4);
	return s;
}

// The per-thread slot where t_call32 keeps the host stack pointer. A host
// longjmp out of guest code must put back the value it had before the call.
uint64_t cpu_tsd_get(void) { return (uint64_t)(uintptr_t)pthread_getspecific(tsd_key); }
void cpu_tsd_set(uint64_t v) { pthread_setspecific(tsd_key, (void *)(uintptr_t)v); }

// the crash report needs a stack of its own: the usual reason to crash is
// having used this thread's stack up
static void alt_stack(void)
{
	static __thread int done;
	if (done) return;
	done = 1;
	stack_t ss = { .ss_sp = malloc(256 << 10), .ss_size = 256 << 10 };
	sigaltstack(&ss, NULL);
}

void cpu_thread_init(uint32_t stack_size)
{
	alt_stack();
	g32 base = low_pages(stack_size, PROT_READ | PROT_WRITE);
	guest_stack_top = base + stack_size - 64;
	pthread_setspecific(stack_key, (void *)(uintptr_t)base);
}

// Where the next guest call's arguments go: well under the current stack
// pointer if we are already running on a guest stack (inside a host call),
// else at the top of this thread's guest stack. The gap must cover what
// t_call32 pushes below rsp (saved registers + the iretq frame).
static inline __attribute__((always_inline)) g32 arg_area(int nargs)
{
	uintptr_t sp;
	__asm__ volatile("mov %%rsp, %0" : "=r"(sp));
	g32 top;
	if (sp < LOW_END) top = (g32)sp - 1024;
	else {
		if (!guest_stack_top) cpu_thread_init(8 << 20);
		top = guest_stack_top;
	}
	return (top - 4 * (uint32_t)nargs) & ~15u;
}

uint64_t gcallv(g32 fn, int nargs, const uint32_t *args, double *st0)
{
	g32 esp = arg_area(nargs);
	if (nargs) memcpy(G2H(esp), args, 4 * (size_t)nargs);
	return call32(fn, esp, st0);
}

uint64_t gcall(g32 fn, int nargs, ...)
{
	uint32_t a[16];
	va_list ap;
	va_start(ap, nargs);
	for (int i = 0; i < nargs; i++) a[i] = va_arg(ap, uint32_t);
	va_end(ap);
	return gcallv(fn, nargs, a, NULL);
}

double gcall_fp(g32 fn, int nargs, ...)
{
	uint32_t a[16];
	double d = 0;
	va_list ap;
	va_start(ap, nargs);
	for (int i = 0; i < nargs; i++) a[i] = va_arg(ap, uint32_t);
	va_end(ap);
	gcallv(fn, nargs, a, &d);
	return d;
}
