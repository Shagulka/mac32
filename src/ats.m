// ATS / ATSUI (32-bit-only Apple text APIs) rebuilt on CoreText, for the
// game's UI font code, plus CGBitmapContext, the Carbon pasteboard and a few
// small calls from the same libraries.
//
// ATSUI facts used here (Apple's ATSUnicode*.h / SATSTypes.h):
//   attribute tags: bold 256, italic 257, underline 258, font 261 (ATSUFontID),
//   size 262 (Fixed), color 263 (RGBColor), rendering options 18;
//   RGB-alpha color 288 (4 floats); layout control kATSUCGContextTag 32767;
//   direct-access selector 100 = ATSLayoutRecord, packed to 2 bytes like the
//   Carbon headers: {u16 glyph @0; u32 flags @2; u32 originalOffset @6;
//   Fixed realPos @10} = 14 bytes (the game walks it with stride 14).
//   ATSFontMetrics: version + 14 floats, in fractions of an em.
#import <Foundation/Foundation.h>
#import <CoreText/CoreText.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <sys/mount.h>
#include <dlfcn.h>
#include "thunks.h"

#define FIX2D(f) ((double)(int32_t)(f) / 65536.0)

// ---- fonts -------------------------------------------------------------------
static CFStringRef fonts[8192];		// ATSFontRef/ATSUFontID n -> PostScript name
static uint32_t nfonts = 1;

static uint32_t font_id(CTFontRef f)
{
	CFStringRef ps = CTFontCopyPostScriptName(f);
	for (uint32_t i = 1; i < nfonts; i++)
		if (CFEqual(fonts[i], ps)) { CFRelease(ps); return i; }
	if (nfonts == 8192) die("too many fonts");
	fonts[nfonts] = ps;
	return nfonts++;
}
static CTFontRef font_at(uint32_t id, double size)
{
	if (!id || id >= nfonts) id = 0;
	return CTFontCreateWithName(id ? fonts[id] : CFSTR("Helvetica"), size, NULL);
}
static int names_match(CTFontRef f, CFStringRef want)
{
	CFStringRef keys[3] = { kCTFontPostScriptNameKey, kCTFontFullNameKey, kCTFontFamilyNameKey };
	for (int i = 0; i < 3; i++) {
		CFStringRef n = CTFontCopyName(f, keys[i]);
		if (!n) continue;
		int eq = CFStringCompare(n, want, kCFCompareCaseInsensitive) == kCFCompareEqualTo;
		CFRelease(n);
		if (eq) return 1;
	}
	return 0;
}
// CoreText substitutes a default font for unknown names; ATS returns 0 instead
static uint32_t find_font(CFStringRef name)
{
	if (!name) return 0;
	CTFontRef f = CTFontCreateWithName(name, 12, NULL);
	uint32_t id = names_match(f, name) ? font_id(f) : 0;
	CFRelease(f);
	LOG("ATSFontFind(%s) -> %u\n", [(__bridge NSString *)name UTF8String], id);
	return id;
}
static void t_ATSFontFindFromName(uint32_t *args, struct hres *r) { RET(find_font(hobj_unwrap(A(0)))); }
static void t_ATSFontFindFromPostScriptName(uint32_t *args, struct hres *r) { RET(find_font(hobj_unwrap(A(0)))); }

// ---- font enumeration (GoldSrc lists the system's fonts) -------------------------
static uint32_t font_id_ps(CFStringRef ps)
{
	for (uint32_t i = 1; i < nfonts; i++)
		if (CFEqual(fonts[i], ps)) return i;
	if (nfonts == 8192) die("too many fonts");
	fonts[nfonts] = CFRetain(ps);
	return nfonts++;
}
static int all_listed;
static void list_all_fonts(void)
{
	if (all_listed) return;
	all_listed = 1;
	CFArrayRef a = CTFontManagerCopyAvailablePostScriptNames();
	for (CFIndex i = 0; i < CFArrayGetCount(a); i++) font_id_ps(CFArrayGetValueAtIndex(a, i));
	CFRelease(a);
}
static void t_ATSUFontCount(uint32_t *args, struct hres *r)
{
	list_all_fonts();
	*(uint32_t *)G2H(A(0)) = nfonts - 1;
	RET(0);
}
static void t_ATSUGetFontIDs(uint32_t *args, struct hres *r)
{
	list_all_fonts();
	uint32_t n = nfonts - 1 < A(1) ? nfonts - 1 : A(1);
	for (uint32_t i = 0; i < n; i++) ((uint32_t *)G2H(A(0)))[i] = i + 1;
	if (A(2)) *(uint32_t *)G2H(A(2)) = n;
	RET(0);
}
// names a font answers to: family 1, full 4, PostScript 6 (Mac platform, Roman)
static const uint32_t name_codes[3] = { 1, 4, 6 };
static CFStringRef font_name(uint32_t id, uint32_t code)
{
	if (code == 6) return CFRetain(fonts[id]);
	CTFontRef f = CTFontCreateWithName(fonts[id], 12, NULL);
	CFStringRef n = CTFontCopyName(f, code == 1 ? kCTFontFamilyNameKey : kCTFontFullNameKey);
	CFRelease(f);
	return n;
}
static void t_ATSUCountFontNames(uint32_t *args, struct hres *r)
{
	*(uint32_t *)G2H(A(1)) = 3;
	RET(A(0) && A(0) < nfonts ? 0 : -8796);	// kATSUInvalidFontErr
}
static void t_ATSUGetIndFontName(uint32_t *args, struct hres *r)
{
	uint32_t id = A(0), idx = A(1), max = A(2);
	if (!id || id >= nfonts || idx >= 3) { RET(-8796); return; }
	CFStringRef n = font_name(id, name_codes[idx]);
	char buf[512] = "";
	if (n) { CFStringGetCString(n, buf, sizeof buf, kCFStringEncodingMacRoman); CFRelease(n); }
	uint32_t len = (uint32_t)strlen(buf);
	if (A(3)) memcpy(G2H(A(3)), buf, len < max ? len : max);
	if (A(4)) *(uint32_t *)G2H(A(4)) = len;
	if (A(5)) *(uint32_t *)G2H(A(5)) = name_codes[idx];
	if (A(6)) *(uint32_t *)G2H(A(6)) = 1;	// kFontMacintoshPlatform
	if (A(7)) *(uint32_t *)G2H(A(7)) = 0;	// kFontRomanScript
	if (A(8)) *(uint32_t *)G2H(A(8)) = 0;	// kFontEnglishLanguage
	RET(0);
}
static void t_ATSUFindFontFromName(uint32_t *args, struct hres *r)
{
	const uint8_t *b = G2H(A(0));
	uint32_t len = A(1), platform = A(3);
	CFStringRef n;
	if (platform == 0 || platform == 3 || (len >= 2 && !b[0]))	// Unicode / Microsoft: UTF-16BE
		n = CFStringCreateWithBytes(NULL, b, len, kCFStringEncodingUTF16BE, false);
	else
		n = CFStringCreateWithBytes(NULL, b, len, kCFStringEncodingMacRoman, false);
	uint32_t id = find_font(n);
	if (n) CFRelease(n);
	*(uint32_t *)G2H(A(6)) = id;
	RET(id ? 0 : -8796);
}
static void t_ATSFontActivateFromFileReference(uint32_t *args, struct hres *r)
{
	char path[PATH_MAX];
	if (FSRefMakePath(G2H(A(0)), (UInt8 *)path, sizeof path)) { RET(-43); return; }	// fnfErr
	CFURLRef u = CFURLCreateFromFileSystemRepresentation(NULL, (UInt8 *)path, (CFIndex)strlen(path), false);
	CFErrorRef err = NULL;
	bool ok = CTFontManagerRegisterFontsForURL(u, kCTFontManagerScopeProcess, &err) ||
		  (err && CFErrorGetCode(err) == kCTFontManagerErrorAlreadyRegistered);
	LOG("ATSFontActivateFromFileReference(%s): %s\n", path, ok ? "registered" : "failed");
	if (err) CFRelease(err);
	CFRelease(u);
	if (A(5)) *(uint32_t *)G2H(A(5)) = 1;	// container ref
	RET(ok ? 0 : -1);
}

static void t_ATSFontActivateFromMemory(uint32_t *args, struct hres *r)
{
	CFDataRef d = CFDataCreate(NULL, G2H(A(0)), A(1));
	CGDataProviderRef p = CGDataProviderCreateWithCFData(d);
	CGFontRef f = CGFontCreateWithDataProvider(p);
	CFErrorRef err = NULL;
	if (!f || (!CTFontManagerRegisterGraphicsFont(f, &err) && err && CFErrorGetCode(err) != kCTFontManagerErrorAlreadyRegistered)) {
		LOG("ATSFontActivateFromMemory: could not register a %u-byte font\n", A(1));
		RET(-1);
	} else {
		CFStringRef n = CGFontCopyPostScriptName(f);
		LOG("ATSFontActivateFromMemory: registered %s\n", [(__bridge NSString *)n UTF8String]);
		CFRelease(n);
		RET(0);
	}
	if (A(6)) *(uint32_t *)G2H(A(6)) = 1;	// container ref
	if (f) CFRelease(f);
	CGDataProviderRelease(p);
	CFRelease(d);
}
static void t_ATSFontGetName(uint32_t *args, struct hres *r)
{
	CTFontRef f = font_at(A(0), 12);
	*(g32 *)G2H(A(2)) = hobj_wrap((void *)CTFontCopyFullName(f));
	CFRelease(f);
	RET(0);
}
static void t_ATSFontGetHorizontalMetrics(uint32_t *args, struct hres *r)
{
	CTFontRef f = font_at(A(0), 1.0);
	CGRect bb = CTFontGetBoundingBox(f);
	float *m = G2H(A(2));
	((uint32_t *)m)[0] = 0;				// version
	m[1] = (float)CTFontGetAscent(f);
	m[2] = -(float)CTFontGetDescent(f);		// ATS descent is negative
	m[3] = (float)CTFontGetLeading(f);
	UniChar x = 'x';
	CGGlyph g;
	CTFontGetGlyphsForCharacters(f, &x, &g, 1);
	m[4] = (float)CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &g, NULL, 1);
	m[5] = (float)bb.size.width;
	m[6] = (float)bb.origin.x;
	m[7] = 0;
	m[8] = m[9] = 0;
	m[10] = (float)CTFontGetCapHeight(f);
	m[11] = (float)CTFontGetXHeight(f);
	m[12] = (float)CTFontGetSlantAngle(f);
	m[13] = (float)CTFontGetUnderlinePosition(f);
	m[14] = (float)CTFontGetUnderlineThickness(f);
	CFRelease(f);
	RET(0);
}

// ---- styles and layouts ------------------------------------------------------
struct style { uint32_t font; double size; int bold, italic, underline; float rgb[3]; };
struct layout { UniChar *text; uint32_t len; struct style *style; CGContextRef ctx; CTLineRef line; };
static struct style *styles[4096];
static struct layout *layouts[4096];
#define SBASE 0x57000000u
#define LBASE 0x4c000000u

static struct style *S(g32 h) { return h >= SBASE && h - SBASE < 4096 ? styles[h - SBASE] : NULL; }
static struct layout *L(g32 h) { return h >= LBASE && h - LBASE < 4096 ? layouts[h - LBASE] : NULL; }

static void t_ATSUCreateStyle(uint32_t *args, struct hres *r)
{
	for (uint32_t i = 1; i < 4096; i++)
		if (!styles[i]) {
			styles[i] = calloc(1, sizeof(struct style));
			styles[i]->size = 12;
			*(g32 *)G2H(A(0)) = SBASE + i;
			RET(0);
			return;
		}
	die("too many ATSU styles");
}
static void t_ATSUDisposeStyle(uint32_t *args, struct hres *r)
{
	if (S(A(0))) { free(S(A(0))); styles[A(0) - SBASE] = NULL; }
	RET(0);
}
static void t_ATSUSetAttributes(uint32_t *args, struct hres *r)
{
	struct style *s = S(A(0));
	uint32_t n = A(1), *tags = G2H(A(2)), *vals = G2H(A(4));
	for (uint32_t i = 0; s && i < n; i++) {
		void *v = G2H(vals[i]);
		switch (tags[i]) {
		case 256: s->bold = *(uint8_t *)v; break;
		case 257: s->italic = *(uint8_t *)v; break;
		case 258: s->underline = *(uint8_t *)v; break;
		case 261: s->font = *(uint32_t *)v; break;
		case 262: s->size = FIX2D(*(int32_t *)v); break;
		case 263: { uint16_t *c = v; for (int k = 0; k < 3; k++) s->rgb[k] = c[k] / 65535.0f; break; }
		case 288: { float *c = v; for (int k = 0; k < 3; k++) s->rgb[k] = c[k]; break; }	// kATSURGBAlphaColorTag
		default: break;
		}
	}
	RET(0);
}
static void t_ATSUCreateTextLayoutWithTextPtr(uint32_t *args, struct hres *r)
{
	// text, offset, length, total, nruns, runlengths[], styles[], *layout
	for (uint32_t i = 1; i < 4096; i++)
		if (!layouts[i]) {
			// kATSUFromTextBeginning / kATSUToTextEnd (0xFFFFFFFE/F) are sentinels
			uint32_t total = A(3), off = A(1) >= 0xfffffff0u ? 0 : A(1);
			uint32_t len = A(2) >= 0xfffffff0u ? total - off : A(2);
			struct layout *l = calloc(1, sizeof *l);
			l->len = len;
			l->text = malloc(sizeof(UniChar) * (l->len + 1));
			memcpy(l->text, (UniChar *)G2H(A(0)) + off, sizeof(UniChar) * l->len);
			l->style = A(4) ? S(((g32 *)G2H(A(6)))[0]) : NULL;
			layouts[i] = l;
			*(g32 *)G2H(A(7)) = LBASE + i;
			RET(0);
			return;
		}
	die("too many ATSU layouts");
}
static void t_ATSUDisposeTextLayout(uint32_t *args, struct hres *r)
{
	struct layout *l = L(A(0));
	if (l) {
		if (l->line) CFRelease(l->line);
		free(l->text);
		free(l);
		layouts[A(0) - LBASE] = NULL;
	}
	RET(0);
}
static void t_ATSUSetLayoutControls(uint32_t *args, struct hres *r)
{
	struct layout *l = L(A(0));
	uint32_t n = A(1), *tags = G2H(A(2)), *vals = G2H(A(4));
	for (uint32_t i = 0; l && i < n; i++)
		if (tags[i] == 32767) l->ctx = hobj_unwrap(*(g32 *)G2H(vals[i]));
	RET(0);
}

static CTFontRef style_font(struct style *s)
{
	CTFontRef f = font_at(s ? s->font : 0, s ? s->size : 12);
	CTFontSymbolicTraits t = (s && s->bold ? kCTFontBoldTrait : 0) | (s && s->italic ? kCTFontItalicTrait : 0);
	if (t) {
		CTFontRef g = CTFontCreateCopyWithSymbolicTraits(f, 0, NULL, t, t);
		if (g) { CFRelease(f); f = g; }
	}
	return f;
}
static CTLineRef layout_line(struct layout *l)
{
	if (l->line) return l->line;
	CTFontRef f = style_font(l->style);
	CGColorRef c = CGColorCreateGenericRGB(l->style ? l->style->rgb[0] : 0, l->style ? l->style->rgb[1] : 0,
					       l->style ? l->style->rgb[2] : 0, 1);
	NSDictionary *at = @{ (id)kCTFontAttributeName: (id)f, (id)kCTForegroundColorAttributeName: (id)c };
	NSString *s = [NSString stringWithCharacters:l->text length:l->len];
	NSAttributedString *as = [[NSAttributedString alloc] initWithString:s attributes:at];
	l->line = CTLineCreateWithAttributedString((CFAttributedStringRef)as);
	[as release];
	CFRelease(f);
	CGColorRelease(c);
	return l->line;
}
static void t_ATSUDrawText(uint32_t *args, struct hres *r)
{
	struct layout *l = L(A(0));
	if (!l || !l->ctx) { RET(-8790); return; }	// kATSUInvalidTextLayoutErr
	CGContextSetTextMatrix(l->ctx, CGAffineTransformIdentity);
	CGContextSetTextPosition(l->ctx, FIX2D(A(3)), FIX2D(A(4)));
	CTLineDraw(layout_line(l), l->ctx);
	RET(0);
}

// glyph records for the whole line, plus a closing record at the line's end
static void t_ATSUDirectGetLayoutDataArrayPtrFromTextLayout(uint32_t *args, struct hres *r)
{
	struct layout *l = L(A(0));
	if (!l || A(2) != 100) { *(g32 *)G2H(A(3)) = 0; *(uint32_t *)G2H(A(4)) = 0; RET(l ? -8800 : -8790); return; }
	CTLineRef line = layout_line(l);
	CFArrayRef runs = CTLineGetGlyphRuns(line);
	CFIndex total = CTLineGetGlyphCount(line);
	g32 arr = gcalloc((uint32_t)total + 1, 14);
	uint8_t *rec = G2H(arr);
	CFIndex k = 0;
	for (CFIndex i = 0; i < CFArrayGetCount(runs); i++) {
		CTRunRef run = CFArrayGetValueAtIndex(runs, i);
		CFIndex n = CTRunGetGlyphCount(run);
		CGGlyph *g = malloc(sizeof *g * (size_t)n);
		CGPoint *p = malloc(sizeof *p * (size_t)n);
		CFIndex *si = malloc(sizeof *si * (size_t)n);
		CTRunGetGlyphs(run, CFRangeMake(0, 0), g);
		CTRunGetPositions(run, CFRangeMake(0, 0), p);
		CTRunGetStringIndices(run, CFRangeMake(0, 0), si);
		for (CFIndex j = 0; j < n; j++, k++) {
			uint8_t *e = rec + k * 14;
			uint32_t off = (uint32_t)si[j] * 2;
			int32_t pos = (int32_t)(p[j].x * 65536.0);
			memcpy(e, &g[j], 2);
			memcpy(e + 6, &off, 4);
			memcpy(e + 10, &pos, 4);
		}
		free(g); free(p); free(si);
	}
	uint8_t *e = rec + total * 14;
	uint16_t del = 0xffff;						// kATSDeletedGlyphcode
	uint32_t end = l->len * 2;
	int32_t width = (int32_t)(CTLineGetTypographicBounds(line, NULL, NULL, NULL) * 65536.0);
	memcpy(e, &del, 2);
	memcpy(e + 6, &end, 4);
	memcpy(e + 10, &width, 4);
	*(g32 *)G2H(A(3)) = arr;
	*(uint32_t *)G2H(A(4)) = (uint32_t)total + 1;
	RET(0);
}
static void t_ATSUDirectReleaseLayoutDataArrayPtr(uint32_t *args, struct hres *r)
{
	g32 *p = G2H(A(2));
	if (p && *p) { gfree(*p); *p = 0; }
	RET(0);
}

static void glyph_metrics(struct style *s, uint32_t n, g32 glyphs, uint32_t stride,
			  void (*put)(uint32_t i, CTFontRef f, CGGlyph g, CGRect bb, double adv, void *ctx), void *ctx)
{
	CTFontRef f = style_font(s);
	for (uint32_t i = 0; i < n; i++) {
		CGGlyph g = *(uint16_t *)G2H(glyphs + i * (stride ? stride : 2));
		CGRect bb;
		CTFontGetBoundingRectsForGlyphs(f, kCTFontOrientationHorizontal, &g, &bb, 1);
		double adv = CTFontGetAdvancesForGlyphs(f, kCTFontOrientationHorizontal, &g, NULL, 1);
		put(i, f, g, bb, adv, ctx);
	}
	CFRelease(f);
}
// ATSGlyphIdealMetrics: advance, sideBearing, otherSideBearing (Float32Points)
static void put_ideal(uint32_t i, CTFontRef f, CGGlyph g, CGRect bb, double adv, void *ctx)
{
	(void)f; (void)g;
	float *m = (float *)ctx + i * 6;
	m[0] = (float)adv; m[1] = 0;
	m[2] = (float)bb.origin.x; m[3] = 0;
	m[4] = (float)(adv - (bb.origin.x + bb.size.width)); m[5] = 0;
}
static void t_ATSUGlyphGetIdealMetrics(uint32_t *args, struct hres *r)
{
	glyph_metrics(S(A(0)), A(1), A(2), A(3), put_ideal, G2H(A(4)));
	RET(0);
}
// ATSGlyphScreenMetrics: deviceAdvance, topLeft, height, width, sideBearing, otherSideBearing
static void put_screen(uint32_t i, CTFontRef f, CGGlyph g, CGRect bb, double adv, void *ctx)
{
	(void)f; (void)g;
	uint8_t *m = (uint8_t *)ctx + i * 40;
	float *fl = (float *)m;
	double l = floor(bb.origin.x), t = ceil(bb.origin.y + bb.size.height);
	double rgt = ceil(bb.origin.x + bb.size.width), b = floor(bb.origin.y);
	fl[0] = (float)round(adv); fl[1] = 0;
	fl[2] = (float)l; fl[3] = (float)t;
	*(uint32_t *)(m + 16) = (uint32_t)(t - b);
	*(uint32_t *)(m + 20) = (uint32_t)(rgt - l);
	fl[6] = (float)l; fl[7] = 0;
	fl[8] = (float)(round(adv) - rgt); fl[9] = 0;
}
static void t_ATSUGlyphGetScreenMetrics(uint32_t *args, struct hres *r)
{
	// style, n, glyphs, stride, forcingAntiAlias, antiAliasSwitch, out
	glyph_metrics(S(A(0)), A(1), A(2), A(3), put_screen, G2H(A(6)));
	RET(0);
}
static void t_ok(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static int32_t w_Long2Fix(int32_t x) { return x << 16; }

// ---- CGBitmapContext: pixels always live in guest memory ------------------------
static void t_CGBitmapContextCreate(uint32_t *args, struct hres *r)
{
	g32 data = A(0);
	if (!data) data = gcalloc(A(2), A(4) ? A(4) : A(1) * 4);
	CGContextRef c = CGBitmapContextCreate(G2H(data), A(1), A(2), A(3), A(4), hobj_unwrap(A(5)), A(6));
	RET(hobj_wrap(c));
}
static void t_CGBitmapContextGetData(uint32_t *args, struct hres *r)
{
	void *p = CGBitmapContextGetData(hobj_unwrap(A(0)));
	RET((uintptr_t)p < LOW_END ? (g32)(uintptr_t)p : 0);
}
static void w_CGContextClearRect(CGContextRef c, float x, float y, float w, float h) { CGContextClearRect(c, CGRectMake(x, y, w, h)); }
static void w_CGContextSetLineWidth(CGContextRef c, float w) { CGContextSetLineWidth(c, w); }
static void w_CGContextSetRGBStrokeColor(CGContextRef c, float r, float g, float b, float a) { CGContextSetRGBStrokeColor(c, r, g, b, a); }
static void w_CGContextSetRGBFillColor(CGContextRef c, float r, float g, float b, float a) { CGContextSetRGBFillColor(c, r, g, b, a); }
static void w_CGContextSetAllowsAntialiasing(CGContextRef c, int b) { CGContextSetAllowsAntialiasing(c, b & 0xff); }
static void w_CGContextSetShouldAntialias(CGContextRef c, int b) { CGContextSetShouldAntialias(c, b & 0xff); }

// ---- Carbon pasteboard: item IDs are small numbers, ItemCount is 32-bit -----------
static void t_PasteboardCreate(uint32_t *args, struct hres *r)
{
	PasteboardRef p = NULL;
	OSStatus s = PasteboardCreate(hobj_unwrap(A(0)), &p);
	*(g32 *)G2H(A(1)) = hobj_wrap(p);
	RET(s);
}
static void t_PasteboardGetItemCount(uint32_t *args, struct hres *r)
{
	ItemCount n = 0;
	OSStatus s = PasteboardGetItemCount(hobj_unwrap(A(0)), &n);
	*(uint32_t *)G2H(A(1)) = (uint32_t)n;
	RET(s);
}
static void t_PasteboardGetItemIdentifier(uint32_t *args, struct hres *r)
{
	PasteboardItemID id = NULL;
	OSStatus s = PasteboardGetItemIdentifier(hobj_unwrap(A(0)), (CFIndex)(int32_t)A(1), &id);
	*(uint32_t *)G2H(A(2)) = (uint32_t)(uintptr_t)id;
	RET(s);
}
static void t_PasteboardCopyItemFlavorData(uint32_t *args, struct hres *r)
{
	CFDataRef d = NULL;
	OSStatus s = PasteboardCopyItemFlavorData(hobj_unwrap(A(0)), (PasteboardItemID)(uintptr_t)A(1), hobj_unwrap(A(2)), &d);
	*(g32 *)G2H(A(3)) = hobj_wrap((void *)d);
	RET(s);
}
static int w_PasteboardPutItemFlavor(PasteboardRef p, uint32_t id, CFStringRef f, CFDataRef d, uint32_t fl)
{ return PasteboardPutItemFlavor(p, (PasteboardItemID)(uintptr_t)id, f, d, fl); }
static int w_PasteboardClear(PasteboardRef p) { return PasteboardClear(p); }
static uint32_t w_PasteboardSynchronize(PasteboardRef p) { return PasteboardSynchronize(p); }
static int w_SetThemeCursor(uint32_t c)
{
	static int (*f)(uint32_t);	// not declared for 64-bit; look it up
	if (!f) f = dlsym(RTLD_DEFAULT, "SetThemeCursor");
	return f ? f(c) : 0;
}

// QuickDraw Rect {top, left, bottom, right} of the ink, y down, around (x, y)
static void t_ATSUMeasureTextImage(uint32_t *args, struct hres *r)
{
	struct layout *l = L(A(0));
	if (!l) { RET(-8790); return; }
	CGFloat asc = 0, desc = 0;
	double w = CTLineGetTypographicBounds(layout_line(l), &asc, &desc, NULL);
	double x = FIX2D(A(3)), y = FIX2D(A(4));
	int16_t *rc = G2H(A(5));
	rc[0] = (int16_t)floor(y - asc);
	rc[1] = (int16_t)floor(x);
	rc[2] = (int16_t)ceil(y + desc);
	rc[3] = (int16_t)ceil(x + w);
	RET(0);
}
static void t_CGContextSelectFont(uint32_t *args, struct hres *r)
{
	(void)r;
	float size;
	memcpy(&size, &args[2], 4);
	CGContextSelectFont(hobj_unwrap(A(0)), G2H(A(1)), size, (CGTextEncoding)A(3));
}

// a window's context belongs to AppKit (see window_context_note in legacy.m)
extern id window_of_context(CGContextRef c);
static void w_CGContextFlush(CGContextRef c) { if (!window_of_context(c)) CGContextFlush(c); }
static void w_CGContextRelease(CGContextRef c) { if (c && !window_of_context(c)) CGContextRelease(c); }

static struct hfn ats[] = {
	R("_ATSFontFindFromName", t_ATSFontFindFromName),
	R("_ATSFontFindFromPostScriptName", t_ATSFontFindFromPostScriptName),
	R("_ATSFontActivateFromMemory", t_ATSFontActivateFromMemory),
	R("_ATSFontActivateFromFileReference", t_ATSFontActivateFromFileReference),
	R("_ATSUFontCount", t_ATSUFontCount), R("_ATSUGetFontIDs", t_ATSUGetFontIDs),
	R("_ATSUCountFontNames", t_ATSUCountFontNames), R("_ATSUGetIndFontName", t_ATSUGetIndFontName),
	R("_ATSUFindFontFromName", t_ATSUFindFontFromName), R("_ATSUMeasureTextImage", t_ATSUMeasureTextImage),
	R("_CGContextSelectFont", t_CGContextSelectFont),
	R("_ATSFontGetName", t_ATSFontGetName),
	R("_ATSFontGetHorizontalMetrics", t_ATSFontGetHorizontalMetrics),
	R("_ATSUCreateStyle", t_ATSUCreateStyle), R("_ATSUDisposeStyle", t_ATSUDisposeStyle),
	R("_ATSUSetAttributes", t_ATSUSetAttributes),
	R("_ATSUCreateTextLayoutWithTextPtr", t_ATSUCreateTextLayoutWithTextPtr),
	R("_ATSUDisposeTextLayout", t_ATSUDisposeTextLayout),
	R("_ATSUSetLayoutControls", t_ATSUSetLayoutControls),
	R("_ATSUDrawText", t_ATSUDrawText),
	R("_ATSUDirectGetLayoutDataArrayPtrFromTextLayout", t_ATSUDirectGetLayoutDataArrayPtrFromTextLayout),
	R("_ATSUDirectReleaseLayoutDataArrayPtr", t_ATSUDirectReleaseLayoutDataArrayPtr),
	R("_ATSUGlyphGetIdealMetrics", t_ATSUGlyphGetIdealMetrics),
	R("_ATSUGlyphGetScreenMetrics", t_ATSUGlyphGetScreenMetrics),
	R("_ATSUSetTransientFontMatching", t_ok),
	G("_Long2Fix", w_Long2Fix, "i:i"),
	R("_CGBitmapContextCreate", t_CGBitmapContextCreate),
	R("_CGBitmapContextGetData", t_CGBitmapContextGetData),
	G("_CGColorSpaceCreateDeviceRGB", CGColorSpaceCreateDeviceRGB, "o:"),
	G("_CGColorSpaceRelease", CGColorSpaceRelease, "v:o"),
	G("_CGContextClearRect", w_CGContextClearRect, "v:offff"),
	G("_CGContextFlush", w_CGContextFlush, "v:o"),
	G("_CGContextRelease", w_CGContextRelease, "v:o"),
	G("_CGContextSetAllowsAntialiasing", w_CGContextSetAllowsAntialiasing, "v:oi"),
	G("_CGContextSetShouldAntialias", w_CGContextSetShouldAntialias, "v:oi"),
	G("_CGContextSetLineWidth", w_CGContextSetLineWidth, "v:of"),
	G("_CGContextSetRGBStrokeColor", w_CGContextSetRGBStrokeColor, "v:offff"),
	G("_CGContextSetRGBFillColor", w_CGContextSetRGBFillColor, "v:offff"),
	G("_CGContextSetTextDrawingMode", CGContextSetTextDrawingMode, "v:oi"),
	R("_PasteboardCreate", t_PasteboardCreate),
	R("_PasteboardGetItemCount", t_PasteboardGetItemCount),
	R("_PasteboardGetItemIdentifier", t_PasteboardGetItemIdentifier),
	R("_PasteboardCopyItemFlavorData", t_PasteboardCopyItemFlavorData),
	G("_PasteboardPutItemFlavor", w_PasteboardPutItemFlavor, "i:ouoou"),
	G("_PasteboardClear", w_PasteboardClear, "i:o"),
	G("_PasteboardSynchronize", w_PasteboardSynchronize, "u:o"),
	G("_statfs64", statfs, "i:pp"),
	G("_SetThemeCursor", w_SetThemeCursor, "i:u"),
};

void thunks_ats_init(void) { REGISTER(ats); }
