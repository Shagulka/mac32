// Fixes for bugs in the games' own 32-bit code, applied in memory when an
// image loads (the files on disk are never changed). Each patch replaces one
// function with a jump to a host function of ours.
#include "thunks.h"
#include <string.h>

#define U32(a) (*(uint32_t *)G2H(a))

// ---- Source engine, Nov 2024 builds: CFontManager::CreateOrFindWin32Font ----
// Valve's Mac vguimatsurface.dylib (Half-Life 2, Portal, ...) was built with
// font creation compiled out: this function appends a NULL font and returns
// NULL, and the next lookup crashes on it. The leftover COSXFont class does
// not match the font interface the rest of the library calls, so mac32 makes
// its own fonts (srcfont.m). This is the function as it should be.
//   this+0x14: CUtlVector<IVGuiFontImpl *> {memory, allocated, grow, size, elements}
static g32 utl_grow;
extern g32 srcfont_new(uint32_t name, uint32_t tall, uint32_t weight, uint32_t blur, uint32_t scanlines, uint32_t flags);
static void t_CreateOrFindWin32Font(uint32_t *args, struct hres *r)
{
	g32 self = A(0), vec = self + 0x14;
	uint32_t n = U32(vec + 12);
	for (uint32_t i = 0; i < n; i++) {
		g32 f = U32(U32(vec) + 4 * i);
		if (f && (gcall(U32(U32(f) + 0x10), 7, f, A(1), A(2), A(3), A(4), A(5), A(6)) & 0xff)) {	// IsEqualTo
			RET(f);
			return;
		}
	}
	g32 f = srcfont_new(A(1), A(2), A(3), A(4), A(5), A(6));
	if (!f) { RET(0); return; }
	if (n >= U32(vec + 4)) gcall(utl_grow, 2, vec, n + 1 - U32(vec + 4));
	U32(U32(vec) + 4 * n) = f;
	U32(vec + 12) = n + 1;
	U32(vec + 16) = U32(vec);			// m_pElements, the debugger's view
	RET(f);
}
static struct hfn h_font = R("CreateOrFindWin32Font (mac32 fix)", t_CreateOrFindWin32Font);

static int patch_jump(struct image *img, const char *fn, struct hfn *h)
{
	g32 at = macho_local_sym(img, fn);
	if (!at) return 0;
	g32 to = hfn_stub(h);
	uint8_t *p = G2H(at);
	int32_t rel = (int32_t)(to - (at + 5));
	p[0] = 0xe9;					// jmp rel32
	memcpy(p + 1, &rel, 4);
	return 1;
}

void apply_guest_patches(struct image *img)
{
	const char *base = strrchr(macho_path_of(img), '/');
	base = base ? base + 1 : macho_path_of(img);
	if (!strcmp(base, "vguimatsurface.dylib")) {
		utl_grow = macho_local_sym(img, "__ZN10CUtlMemoryIP13IVGuiFontImpliE4GrowEi");
		if (utl_grow && macho_local_sym(img, "__ZN8COSXFont6CreateEPKciiiii") &&	// the Nov 2024 build
		    patch_jump(img, "__ZN12CFontManager21CreateOrFindWin32FontEPKciiiii", &h_font))
			fprintf(stderr, "[mac32] patched %s: fonts come from mac32 (CoreText)\n", base);
	}
}
