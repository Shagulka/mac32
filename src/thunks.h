#pragma once
#include "mac32.h"

#define MAX_HFN 16384

struct hfn {
	const char *name;
	char kind;		// G generic, R raw, D data, M missing, C closure
	void *fn;
	const char *sig;
	uint32_t size;		// D: bytes of guest data
	void (*init)(g32 addr);	// D: fill the data
	const void *ctx;	// C: closure data for fn(h, args, r)
	g32 addr;		// stub or data address once bound
	uint32_t idx;
};

#define G(n, f, s) { .name = n, .kind = 'G', .fn = (void *)(f), .sig = s }
#define R(n, f)    { .name = n, .kind = 'R', .fn = (void *)(f) }
#define D(n, sz, i) { .name = n, .kind = 'D', .size = sz, .init = i }

void thunk_register(struct hfn *list, size_t n);
g32 hfn_stub(struct hfn *f);				// a stub for a dynamically made entry
#define REGISTER(list) thunk_register(list, sizeof list / sizeof list[0])

void thunks_libc_init(void);
void thunks_pthread_init(void);
void thunks_cxx_init(void);
void thunks_libc2_init(void);
void thunks_objc_init(void);
void thunks_apple_init(void);
void thunks_gl_init(void);
void thunks_libc3_init(void);
void thunks_ats_init(void);
void thunks_steam_init(void);
void thunks_zlib_init(void);
void thunks_audio_init(void);
void thunks_sndmgr_init(void);
void thunks_openal_init(void);
void thunks_apple2_init(void);
void thunks_legacy_init(void);

// raw thunk helpers
#define A(i) (args[i])
#define RET(v) (r->rax = (uint32_t)(v))
#define RET64(v) (r->rax = (uint64_t)(v))

// guest printf engine (format.c): format with i386 varargs at ap
char *gvformat(const char *fmt, const uint32_t *ap, int *len);
char *gvformat_objc(const char *fmt, const uint32_t *ap, int *len, const char *(*obj)(g32));

// guest FILE objects (libc.c)
FILE *gfile_host(g32 gf);
g32 gfile_new(FILE *h);
