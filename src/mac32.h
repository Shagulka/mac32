// mac32: run 32-bit i386 Mach-O code on Apple Silicon under Rosetta 2.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint32_t g32;	// a guest (32-bit) address

#define G2H(a) ((void *)(uintptr_t)(g32)(a))
#define H2G(p) ((g32)(uintptr_t)(p))

void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
extern int verbose;
#define LOG(...) do { if (verbose) fprintf(stderr, "[mac32] " __VA_ARGS__); } while (0)

// lowmem.c: everything the guest sees lives below 4 GB.
#define LOW_START 0x00001000u
#define LOW_END   0xf0000000u
void low_init(void);
void low_commit(g32 addr, uint32_t len, int prot);	// map fixed, zeroed
g32 low_pages(uint32_t len, int prot);			// page-aligned block
int low_free(g32 a);					// give a low_pages block back
uint32_t low_block_len(g32 a);				// its length, 0 if a is not one
g32 gmalloc(uint32_t n);
g32 gcalloc(uint32_t n, uint32_t sz);
g32 grealloc(g32 p, uint32_t n);
g32 gmemalign(uint32_t align, uint32_t n);
void gfree(g32 p);
uint32_t gmalloc_size(g32 p);
g32 gstrdup(const char *s);

// cpu.c: mode switching and host calls.
struct hres {			// result of a host call, filled by the thunk
	uint64_t rax;		// edx:eax
	double st0;		// pushed on the x87 stack if F_ST0
	uint32_t flags;
	uint32_t pad;
};
#define F_ST0  1		// return value is in st(0)
#define F_POP4 2		// callee pops 4 bytes (struct return pointer)

typedef void (*rawfn)(uint32_t *args, struct hres *r);

void cpu_init(void);
void cpu_thread_init(uint32_t stack_size);	// give this host thread a guest stack
uint64_t gcall(g32 fn, int nargs, ...);			// call cdecl guest code, 32-bit args
double gcall_fp(g32 fn, int nargs, ...);		// same, returns st(0)
uint64_t gcallv(g32 fn, int nargs, const uint32_t *args, double *st0);

// thunks.c: host functions the guest can import.
g32 host_lookup(const char *name);	// stub or data address, 0 if unknown
g32 missing_stub(const char *name);	// stub that reports an unimplemented call
void thunks_init(void);
const char *hostcall_name(uint32_t idx);

// objc.m: host objects (CF, Cocoa, CGL...) seen from the guest.
// Each host object gets one small guest block; the same object always maps
// to the same block. Guest-class instances keep their 32-bit ivars there.
g32 hobj_wrap(void *h);
void *hobj_unwrap(g32 g);
g32 guest_cstr(const char *s);	// copy into a recycled guest buffer

// macho.c
struct image;
void macho_add_search_dir(const char *dir);
struct image *macho_load(const char *path);
void macho_run_initializers(struct image *img);
g32 macho_sym(struct image *img, const char *name);	// exported symbol, 0 if none
g32 macho_find_any(const char *name);			// search every loaded image
g32 macho_entry(struct image *img);
g32 macho_main(struct image *img);
void set_guest_bundle(const char *path);
const char *macho_addr_name(g32 addr, g32 *off);	// for crash reports

// guest-side process state
extern g32 g_argv, g_envp, g_apple, g_progname;
extern int g_argc;
