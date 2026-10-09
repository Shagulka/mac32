// A Source engine VGUI font for the Nov 2024 Mac builds (Half-Life 2, Portal,
// ...), drawn with CoreText. Their vguimatsurface calls fonts through a newer
// IVGuiFontImpl layout than the leftover COSXFont class implements, and no
// class there implements it, so the game could not make any font. The layout
// below was read from the callers in vguimatsurface (offset: use):
//   0x00 ~dtor  0x04 deleting dtor  0x08 Create  0x0c GetCharRGBA  0x10 IsEqualTo
//   0x14 IsValid  0x18 GetCharABCWidths  0x20 GetHeight  0x24 GetHeightRequested
//   0x28 GetAscent  0x2c GetMaxCharWidth  0x30 GetFlags  0x34 GetUnderlined
//   0x38 GetName  0x3c GetFamilyName  0x44 GetKernedCharWidth  0x48 usable?
// Slots 0x1c and 0x40 have no caller found; they log once and return 0.
#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#import <CoreGraphics/CoreGraphics.h>
#include <pthread.h>
#include "thunks.h"

#define U32(a) (*(uint32_t *)G2H(a))
enum { FONTFLAG_ITALIC = 0x1, FONTFLAG_UNDERLINE = 0x2, FONTFLAG_ANTIALIAS = 0x10 };

struct glyph { uint32_t ch; int a, b, c; float advance; };
struct sfont {
	char name[64];
	g32 gname;			// guest copy of the name
	int tall, weight, blur, scanlines, flags;
	CTFontRef ct;
	int height, ascent, maxwide;
	struct glyph *cache;		// open addressing on ch
	uint32_t cap, n;
};
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static struct { g32 obj; struct sfont *f; } objs[4096];
static int nobjs;
static g32 vtable;

static struct sfont *F(g32 obj)
{
	for (int i = 0; i < nobjs; i++) if (objs[i].obj == obj) return objs[i].f;
	return NULL;
}

// Windows-style "tall" is the cell height: scale the point size so ascent +
// descent fill it
static CTFontRef make_font(const char *name, int tall, int weight, int flags)
{
	CFStringRef n = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CTFontRef f = CTFontCreateWithName(n, tall, NULL);
	CFRelease(n);
	CGFloat h = CTFontGetAscent(f) + CTFontGetDescent(f);
	if (h > 0 && fabs(h - tall) > 0.5) {
		CTFontRef g = CTFontCreateCopyWithAttributes(f, tall * tall / h, NULL, NULL);
		CFRelease(f);
		f = g;
	}
	CTFontSymbolicTraits t = (weight >= 600 ? kCTFontBoldTrait : 0) | ((flags & FONTFLAG_ITALIC) ? kCTFontItalicTrait : 0);
	if (t) {
		CTFontRef g = CTFontCreateCopyWithSymbolicTraits(f, 0, NULL, t, t);
		if (g) { CFRelease(f); f = g; }
	}
	return f;
}

static CGGlyph glyph_for(CTFontRef f, uint32_t ch, CTFontRef *used)
{
	UniChar u[2];
	CFIndex n = 1;
	if (ch > 0xffff) { ch -= 0x10000; u[0] = (UniChar)(0xd800 + (ch >> 10)); u[1] = (UniChar)(0xdc00 + (ch & 0x3ff)); n = 2; }
	else u[0] = (UniChar)ch;
	CGGlyph g[2] = { 0, 0 };
	*used = (CTFontRef)CFRetain(f);
	if (CTFontGetGlyphsForCharacters(f, u, g, n)) return g[0];
	// not in this font: let CoreText pick a fallback, as Windows font linking would
	CFStringRef s = CFStringCreateWithCharacters(NULL, u, n);
	CTFontRef fb = CTFontCreateForString(f, s, CFRangeMake(0, n));
	CFRelease(s);
	if (fb && CTFontGetGlyphsForCharacters(fb, u, g, n)) { CFRelease(*used); *used = fb; return g[0]; }
	if (fb) CFRelease(fb);
	return 0;
}

static struct glyph *metrics(struct sfont *f, uint32_t ch)
{
	if (f->n * 2 >= f->cap) {
		struct glyph *old = f->cache;
		uint32_t oc = f->cap;
		f->cap = oc ? oc * 2 : 256;
		f->cache = calloc(f->cap, sizeof *f->cache);
		f->n = 0;
		for (uint32_t i = 0; i < oc; i++)
			if (old[i].ch) {
				uint32_t k = old[i].ch * 2654435761u & (f->cap - 1);
				while (f->cache[k].ch) k = (k + 1) & (f->cap - 1);
				f->cache[k] = old[i];
				f->n++;
			}
		free(old);
	}
	uint32_t k = ch * 2654435761u & (f->cap - 1);
	for (; f->cache[k].ch; k = (k + 1) & (f->cap - 1))
		if (f->cache[k].ch == ch) return &f->cache[k];
	struct glyph *g = &f->cache[k];
	g->ch = ch ? ch : 0xffffffffu;
	f->n++;
	CTFontRef used;
	CGGlyph gl = glyph_for(f->ct, ch, &used);
	CGSize adv;
	CTFontGetAdvancesForGlyphs(used, kCTFontOrientationHorizontal, &gl, &adv, 1);
	CGRect bb = CTFontGetBoundingRectsForGlyphs(used, kCTFontOrientationHorizontal, &gl, NULL, 1);
	CFRelease(used);
	g->advance = (float)adv.width;
	if (CGRectIsEmpty(bb)) {			// a space
		g->a = 0; g->b = 0; g->c = (int)lround(adv.width);
	} else {
		g->a = (int)floor(bb.origin.x);
		g->b = (int)ceil(CGRectGetMaxX(bb)) - g->a + 1;	// a pixel of room for antialiasing
		g->c = (int)lround(adv.width) - g->a - g->b;
	}
	return g;
}

// ---- the vtable slots: (this, args...) ----------------------------------------------
static void s_dtor(uint32_t *args, struct hres *r)
{
	(void)r;
	pthread_mutex_lock(&lk);
	for (int i = 0; i < nobjs; i++)
		if (objs[i].obj == A(0)) {
			struct sfont *f = objs[i].f;
			if (f->ct) CFRelease(f->ct);
			free(f->cache);
			free(f);
			objs[i] = objs[--nobjs];
			break;
		}
	pthread_mutex_unlock(&lk);
}
static void s_delete(uint32_t *args, struct hres *r) { s_dtor(args, r); gfree(A(0)); }
static void s_create(uint32_t *args, struct hres *r)
{
	const char *name = A(1) ? G2H(A(1)) : "Helvetica";
	struct sfont *f = calloc(1, sizeof *f);
	snprintf(f->name, sizeof f->name, "%s", name);
	f->tall = (int)A(2); f->weight = (int)A(3); f->blur = (int)A(4); f->scanlines = (int)A(5); f->flags = (int)A(6);
	f->ct = make_font(name, f->tall > 0 ? f->tall : 12, f->weight, f->flags);
	f->ascent = (int)ceil(CTFontGetAscent(f->ct));
	f->height = f->ascent + (int)ceil(CTFontGetDescent(f->ct));
	f->maxwide = (int)ceil(CTFontGetBoundingBox(f->ct).size.width);
	f->gname = gstrdup(f->name);
	pthread_mutex_lock(&lk);
	struct sfont *old = F(A(0));
	if (old) { CFRelease(old->ct); free(old->cache); free(old); }
	int i = 0;
	while (i < nobjs && objs[i].obj != A(0)) i++;
	if (i == nobjs) { if (nobjs == 4096) die("srcfont: too many fonts"); nobjs++; }
	objs[i].obj = A(0);
	objs[i].f = f;
	pthread_mutex_unlock(&lk);
	LOG("srcfont: %s tall %d weight %d flags %#x -> height %d ascent %d\n", name, f->tall, f->weight, f->flags, f->height, f->ascent);
	RET(1);
}
static void s_is_equal(uint32_t *args, struct hres *r)
{
	pthread_mutex_lock(&lk);
	struct sfont *f = F(A(0));
	int eq = f && !strcasecmp(f->name, A(1) ? G2H(A(1)) : "") && f->tall == (int)A(2) && f->weight == (int)A(3) &&
		 f->blur == (int)A(4) && f->scanlines == (int)A(5) && f->flags == (int)A(6);
	pthread_mutex_unlock(&lk);
	RET(eq);
}
static void s_true(uint32_t *args, struct hres *r) { (void)args; RET(1); }
// GetCharRGBA(ch, wide, tall, rgba): white, coverage in alpha, top row first
static void s_char_rgba(uint32_t *args, struct hres *r)
{
	(void)r;
	uint32_t ch = A(1);
	int w = (int)A(2), h = (int)A(3);
	if (w <= 0 || h <= 0) return;
	uint8_t *out = G2H(A(4));
	pthread_mutex_lock(&lk);
	struct sfont *f = F(A(0));
	if (!f) { pthread_mutex_unlock(&lk); memset(out, 0, (size_t)w * h * 4); return; }
	struct glyph *g = metrics(f, ch);
	int a = g->a, ascent = f->ascent;
	CTFontRef ct = (CTFontRef)CFRetain(f->ct);
	int aa = (f->flags & FONTFLAG_ANTIALIAS) != 0 || 1;	// Mac text is always smoothed
	pthread_mutex_unlock(&lk);
	uint8_t *cov = calloc((size_t)w * h, 1);
	CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
	CGContextRef c = CGBitmapContextCreate(cov, (size_t)w, (size_t)h, 8, (size_t)w, gray, kCGImageAlphaNone);
	CGColorSpaceRelease(gray);
	CGContextSetGrayFillColor(c, 1, 1);
	CGContextSetShouldAntialias(c, aa);
	CTFontRef used;
	CGGlyph gl = glyph_for(ct, ch, &used);
	CGPoint at = CGPointMake(-a, h - ascent);		// baseline, measured from the bottom
	CTFontDrawGlyphs(used, &gl, &at, 1, c);
	CFRelease(used);
	CFRelease(ct);
	CGContextRelease(c);
	for (int i = 0; i < w * h; i++) {
		out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = 255;
		out[i * 4 + 3] = cov[i];
	}
	free(cov);
}
// GetCharABCWidths(ch, int &a, int &b, int &c, int *)
static void s_abc(uint32_t *args, struct hres *r)
{
	(void)r;
	pthread_mutex_lock(&lk);
	struct sfont *f = F(A(0));
	struct glyph g = f ? *metrics(f, A(1)) : (struct glyph){ 0 };
	pthread_mutex_unlock(&lk);
	if (A(2)) U32(A(2)) = (uint32_t)g.a;
	if (A(3)) U32(A(3)) = (uint32_t)g.b;
	if (A(4)) U32(A(4)) = (uint32_t)g.c;
}
// GetKernedCharWidth(ch, before, after, float &wide, float &abcA, float &abcC)
static void s_kerned(uint32_t *args, struct hres *r)
{
	(void)r;
	pthread_mutex_lock(&lk);
	struct sfont *f = F(A(0));
	struct glyph g = f ? *metrics(f, A(1)) : (struct glyph){ 0 };
	pthread_mutex_unlock(&lk);
	float wide = g.advance, fa = (float)g.a, fc = (float)g.c;
	if (A(4)) memcpy(G2H(A(4)), &wide, 4);
	if (A(5)) memcpy(G2H(A(5)), &fa, 4);
	if (A(6)) memcpy(G2H(A(6)), &fc, 4);
}
#define GETTER(fn, expr) static void fn(uint32_t *args, struct hres *r) { \
	pthread_mutex_lock(&lk); struct sfont *f = F(A(0)); RET(f ? (uint32_t)(expr) : 0); pthread_mutex_unlock(&lk); }
GETTER(s_height, f->height)
GETTER(s_height_req, f->tall)
GETTER(s_ascent, f->ascent)
GETTER(s_maxwide, f->maxwide)
GETTER(s_flags, f->flags)
GETTER(s_underlined, (f->flags & FONTFLAG_UNDERLINE) != 0)
GETTER(s_name, f->gname)
static void s_unknown(uint32_t *args, struct hres *r)
{
	static int once;
	if (!once++) fprintf(stderr, "[mac32] srcfont: a font slot with no known use was called (font %#x)\n", A(0));
	RET(0);
}

static struct hfn slots[] = {
	R("srcfont::~dtor", s_dtor), R("srcfont::delete", s_delete), R("srcfont::Create", s_create),
	R("srcfont::GetCharRGBA", s_char_rgba), R("srcfont::IsEqualTo", s_is_equal), R("srcfont::IsValid", s_true),
	R("srcfont::GetCharABCWidths", s_abc), R("srcfont::slot7", s_unknown), R("srcfont::GetHeight", s_height),
	R("srcfont::GetHeightRequested", s_height_req), R("srcfont::GetAscent", s_ascent),
	R("srcfont::GetMaxCharWidth", s_maxwide), R("srcfont::GetFlags", s_flags), R("srcfont::GetUnderlined", s_underlined),
	R("srcfont::GetName", s_name), R("srcfont::GetFamilyName", s_name), R("srcfont::slot16", s_unknown),
	R("srcfont::GetKernedCharWidth", s_kerned), R("srcfont::usable", s_true),
	R("srcfont::slot19", s_unknown), R("srcfont::slot20", s_unknown), R("srcfont::slot21", s_unknown),
	R("srcfont::slot22", s_unknown), R("srcfont::slot23", s_unknown),
};

// a new font object, already created; 0 if the font cannot be made
g32 srcfont_new(uint32_t name, uint32_t tall, uint32_t weight, uint32_t blur, uint32_t scanlines, uint32_t flags)
{
	if (!vtable) {
		int n = (int)(sizeof slots / sizeof slots[0]);
		g32 vt = gcalloc((uint32_t)n + 2, 4);		// offset-to-top, typeinfo, then the slots
		for (int i = 0; i < n; i++) U32(vt + 8 + 4 * (g32)i) = hfn_stub(&slots[i]);
		vtable = vt + 8;
	}
	g32 obj = gcalloc(1, 64);
	U32(obj) = vtable;
	uint32_t a[7] = { obj, name, tall, weight, blur, scanlines, flags };
	struct hres r = { 0 };
	s_create(a, &r);
	return obj;
}
