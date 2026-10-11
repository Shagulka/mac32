// Source engine, Nov 25 2024 Mac builds (Half-Life 2 20th anniversary update,
// Portal, ...): vguimatsurface.dylib cannot make fonts.
//
// CFontManager::CreateOrFindWin32Font was built with font creation compiled
// out: it appends a NULL font to its list and returns NULL, and the next
// lookup calls a method on that NULL. The leftover COSXFont class does not
// match the IVGuiFontImpl layout the rest of the library calls, so this
// version makes mac32's own CoreText fonts (src/srcfont.m) instead.
//   this+0x14: CUtlVector<IVGuiFontImpl *> {memory, allocated, grow, size, elements}
#include "../src/patches.h"

#define U32(a) (*(uint32_t *)G2H(a))

extern g32 srcfont_new(uint32_t name, uint32_t tall, uint32_t weight, uint32_t blur, uint32_t scanlines, uint32_t flags);
static g32 utl_grow;

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
static struct hfn h_font = R("CreateOrFindWin32Font (mac32 patch)", t_CreateOrFindWin32Font);

static int apply(struct image *img)
{
	utl_grow = macho_local_sym(img, "__ZN10CUtlMemoryIP13IVGuiFontImpliE4GrowEi");
	return utl_grow && patch_jump(img, "__ZN12CFontManager21CreateOrFindWin32FontEPKciiiii", &h_font);
}

const struct guest_patch patch_source2024_vguimatsurface = {
	.library = "vguimatsurface.dylib",
	.sha256 = "6d5d9660de74e0e74d8cf7bbc42032bcc1f38302f6d7a086ee2d6350732be87d",
	.game = "Half-Life 2, Portal (Nov 25 2024 builds)",
	.what = "fonts come from mac32 (CoreText); this build cannot make any",
	.apply = apply,
};
