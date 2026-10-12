// Objective-C bridge between 32-bit guest code (legacy "fragile" i386 ObjC
// runtime) and the 64-bit host runtime.
//
// Host objects reach the guest as small guest blocks (handles). Classes the
// guest defines (AppWindow : NSWindow, ...) become real host subclasses made
// at runtime; every guest method is installed as _objc_msgForward and lands in
// our forwardInvocation:, which calls the 32-bit IMP. Guest messages to host
// objects go through NSInvocation using the host method's type signature,
// converting between i386 and x86_64 layouts (CGFloat was float on i386,
// NSInteger was int, BOOL was char, NSRect returns through a hidden pointer).
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <objc/objc-exception.h>
#include <pthread.h>
#include <sys/mman.h>
#include <ctype.h>
#include "thunks.h"

@interface NSInvocation (Private)
- (void)invokeUsingIMP:(IMP)imp;
@end

static pthread_mutex_t lk = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

// ---- u64 -> u64 hash map -------------------------------------------------
struct map { uint64_t *k, *v; uint32_t cap, n; };
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return x; }
static void map_put(struct map *m, uint64_t k, uint64_t v);
static void map_grow(struct map *m)
{
	struct map o = *m;
	m->cap = o.cap ? o.cap * 2 : 1024;
	m->k = calloc(m->cap, 8);
	m->v = calloc(m->cap, 8);
	m->n = 0;
	for (uint32_t i = 0; i < o.cap; i++) if (o.k[i]) map_put(m, o.k[i], o.v[i]);
	free(o.k); free(o.v);
}
static void map_put(struct map *m, uint64_t k, uint64_t v)
{
	if (m->n * 2 >= m->cap) map_grow(m);
	uint32_t i = (uint32_t)mix(k) & (m->cap - 1);
	while (m->k[i] && m->k[i] != k) i = (i + 1) & (m->cap - 1);
	if (!m->k[i]) m->n++;
	m->k[i] = k;
	m->v[i] = v;
}
static uint64_t map_get(struct map *m, uint64_t k)
{
	if (!m->cap) return 0;
	for (uint32_t i = (uint32_t)mix(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
		if (!m->k[i]) return 0;
		if (m->k[i] == k) return m->v[i];
	}
}

static struct map h2g, g2h;		// host object <-> guest handle

// ---- guest classes ----------------------------------------------------------
struct gmeth { const char *sel; const char *types; g32 imp; };
struct gclass {
	const char *name;
	g32 cls, meta;
	struct gclass *super;		// guest-defined superclass, if any
	const char *host_super;		// first host ancestor's name
	uint32_t isize;
	struct gmeth *im, *cm;
	int nim, ncm;
	Class host;			// dynamic host subclass
};
static struct gclass *gclasses[256];
static int ngclasses;
static struct map gcls_by_addr;		// guest cls/meta struct -> gclass*
static struct map gcls_by_host;		// host Class -> gclass*

static struct gclass *gclass_named(const char *n)
{
	for (int i = 0; i < ngclasses; i++) if (!strcmp(gclasses[i]->name, n)) return gclasses[i];
	return NULL;
}

// selectors: one canonical guest string per name
static NSMutableDictionary *selmap;	// NSString -> NSNumber(g32)
static g32 canon_sel(const char *s, g32 existing)
{
	NSString *k = [NSString stringWithUTF8String:s];
	NSNumber *v = selmap[k];
	if (v) return v.unsignedIntValue;
	g32 g = existing ? existing : gstrdup(s);
	selmap[k] = @(g);
	return g;
}

// legacy i386 structs
#define U32(a) (*(uint32_t *)G2H(a))
enum { C_ISA = 0, C_SUPER = 4, C_NAME = 8, C_INFO = 16, C_ISIZE = 20, C_METHODS = 28 };

static void read_methods(g32 list, struct gmeth **out, int *n)
{
	if (!list || list == 0xffffffff) return;
	int cnt = (int)U32(list + 4);
	*out = realloc(*out, sizeof **out * (size_t)(*n + cnt));
	for (int i = 0; i < cnt; i++) {
		g32 m = list + 8 + (g32)i * 12;
		U32(m) = canon_sel(G2H(U32(m)), U32(m));
		(*out)[*n + i] = (struct gmeth){ G2H(U32(m)), G2H(U32(m + 4)), U32(m + 8) };
	}
	*n += cnt;
}

static g32 objc_wrap(id h);
static id objc_unwrap(g32 g);
static void register_class(g32 cls)
{
	const char *name = G2H(U32(cls + C_NAME));
	if (gclass_named(name)) {
		LOG("objc: class %s defined twice; using the first\n", name);
		return;
	}
	struct gclass *gc = calloc(1, sizeof *gc);
	gc->name = name;
	gc->cls = cls;
	gc->meta = U32(cls + C_ISA);
	gc->isize = U32(cls + C_ISIZE);
	gc->host_super = G2H(U32(cls + C_SUPER));	// still a name at this point
	read_methods(U32(cls + C_METHODS), &gc->im, &gc->nim);
	read_methods(U32(gc->meta + C_METHODS), &gc->cm, &gc->ncm);
	gclasses[ngclasses++] = gc;
	map_put(&gcls_by_addr, cls, (uint64_t)(uintptr_t)gc);
	map_put(&gcls_by_addr, gc->meta, (uint64_t)(uintptr_t)gc);
	LOG("objc: class %s : %s, %d+%d methods, %u bytes\n", name, gc->host_super, gc->nim, gc->ncm, gc->isize);
}

// After all classes of an image are registered: link superclasses.
static void link_class(struct gclass *gc)
{
	if (gc->super || !gc->host_super) return;
	struct gclass *s = gclass_named(gc->host_super);
	if (s) {
		gc->super = s;
		U32(gc->cls + C_SUPER) = s->cls;
		U32(gc->meta + C_SUPER) = s->meta;
		gc->host_super = NULL;
	} else {
		Class hc = objc_getClass(gc->host_super);
		if (!hc) {	// e.g. QTKit classes, gone since macOS 10.15: the class stays unusable
			fprintf(stderr, "[mac32] objc: %s's superclass %s does not exist on this macOS; using NSObject\n",
				gc->name, gc->host_super);
			gc->host_super = "NSObject";
			hc = [NSObject class];
		}
		U32(gc->cls + C_SUPER) = objc_wrap(hc);
		U32(gc->meta + C_SUPER) = objc_wrap(object_getClass(hc));
	}
	// metaclass isa (root meta) is not used by our dispatch
}

struct catdef { const char *cls; g32 im, cm; };
static Class ensure_host_class(struct gclass *gc);
static Class host_base(struct gclass *gc);
static void install(Class cls, Class lookup, struct gmeth *m, int n, int meta);

// Categories a guest adds to host classes (SDL's +[NSCursor invisibleCursor]).
// Guest messages find them here; the host itself does not see them.
struct hcat { Class cls; struct gmeth *im, *cm; int nim, ncm; };
static struct hcat hcats[128];
static int nhcats;
static void add_host_category(const char *cn, g32 im, g32 cm)
{
	Class c = objc_getClass(cn);
	if (!c) { LOG("objc: category on missing class %s ignored\n", cn); return; }
	struct hcat *h = NULL;
	for (int i = 0; i < nhcats && !h; i++) if (hcats[i].cls == c) h = &hcats[i];
	if (!h) {
		if (nhcats == 128) die("objc: too many categories on host classes");
		h = &hcats[nhcats++];
		h->cls = c;
	}
	read_methods(im, &h->im, &h->nim);
	read_methods(cm, &h->cm, &h->ncm);
	LOG("objc: category on host class %s: %d+%d methods\n", cn, h->nim, h->ncm);
}
static g32 host_category_imp(id target, const char *sel)
{
	if (!nhcats || !target) return 0;
	int cm = object_isClass(target);
	for (Class k = cm ? (Class)target : object_getClass(target); k; k = class_getSuperclass(k))
		for (int i = 0; i < nhcats; i++) {
			if (hcats[i].cls != k) continue;
			struct gmeth *m = cm ? hcats[i].cm : hcats[i].im;
			int n = cm ? hcats[i].ncm : hcats[i].nim;
			for (int j = n - 1; j >= 0; j--) if (!strcmp(m[j].sel, sel)) return m[j].imp;
		}
	return 0;
}

int macho_section(struct image *img, const char *seg, const char *sect, g32 *addr, uint32_t *size);

void objc_image_loaded(struct image *img)
{
	g32 a;
	uint32_t sz;
	pthread_mutex_lock(&lk);
	if (!selmap) selmap = [NSMutableDictionary new];
	if (macho_section(img, "__OBJC", "__message_refs", &a, &sz))
		for (uint32_t i = 0; i < sz; i += 4) U32(a + i) = canon_sel(G2H(U32(a + i)), U32(a + i));
	if (macho_section(img, "__OBJC", "__module_info", &a, &sz)) {
		for (uint32_t m = 0; m < sz; m += 16) {
			g32 st = U32(a + m + 12);
			if (!st) continue;
			uint16_t ncls = *(uint16_t *)G2H(st + 8), ncat = *(uint16_t *)G2H(st + 10);
			for (int i = 0; i < ncls; i++) register_class(U32(st + 12 + (g32)i * 4));
			for (int i = 0; i < ncat; i++) {
				g32 c = U32(st + 12 + (g32)(ncls + i) * 4);
				const char *cn = G2H(U32(c + 4));
				struct gclass *gc = gclass_named(cn);
				LOG("objc: category %s on %s\n", (char *)G2H(U32(c)), cn);
				if (gc) {
					int ni = gc->nim, nc = gc->ncm;
					read_methods(U32(c + 8), &gc->im, &gc->nim);
					read_methods(U32(c + 12), &gc->cm, &gc->ncm);
					if (gc->host) {	// class already made: give it the new methods too
						Class base = host_base(gc);
						install(gc->host, base, gc->im + ni, gc->nim - ni, 0);
						install(object_getClass(gc->host), base, gc->cm + nc, gc->ncm - nc, 1);
					}
				} else {
					add_host_category(cn, U32(c + 8), U32(c + 12));
				}
			}
		}
	}
	for (int i = 0; i < ngclasses; i++) link_class(gclasses[i]);
	// make the host classes now: nib files and NSClassFromString look them up by name
	for (int i = 0; i < ngclasses; i++) ensure_host_class(gclasses[i]);
	if (macho_section(img, "__OBJC", "__cls_refs", &a, &sz))
		for (uint32_t i = 0; i < sz; i += 4) {
			const char *n = G2H(U32(a + i));
			struct gclass *gc = gclass_named(n);
			if (gc) U32(a + i) = gc->cls;
			else {
				Class hc = objc_getClass(n);
				if (!hc) LOG("objc: class %s not found\n", n);
				U32(a + i) = hc ? objc_wrap(hc) : 0;
				if (getenv("MAC32_CLSREFS")) LOG("cls_ref %#x (%s) -> %#x\n", a + i, n, U32(a + i));
			}
		}
	// MAC32_CLSREFS_RO=1: make __cls_refs read-only, to catch whoever overwrites it
	if (getenv("MAC32_CLSREFS_RO") && macho_section(img, "__OBJC", "__cls_refs", &a, &sz))
		mprotect(G2H(a & ~0xfffu), ((a + sz + 0xfff) & ~0xfffu) - (a & ~0xfffu), PROT_READ);
	pthread_mutex_unlock(&lk);
}

// ---- type encodings ---------------------------------------------------------
static const char *skipq(const char *t) { while (*t && strchr("rnNoORV", *t)) t++; return t; }

enum st_kind { ST_NONE, ST_RECT, ST_PAIR_F, ST_PAIR_U };
static enum st_kind struct_kind(const char *t)
{
	if (*t != '{') return ST_NONE;
	if (!strncmp(t, "{CGRect=", 8) || !strncmp(t, "{_NSRect=", 9)) return ST_RECT;
	if (!strncmp(t, "{CGPoint=", 9) || !strncmp(t, "{CGSize=", 8) ||
	    !strncmp(t, "{_NSPoint=", 10) || !strncmp(t, "{_NSSize=", 9)) return ST_PAIR_F;
	if (!strncmp(t, "{_NSRange=", 10)) return ST_PAIR_U;
	return ST_NONE;
}

// guest -> host value for one argument; returns guest words consumed
static int arg_g2h(const char *t, const uint32_t *g, void *out, g32 *outparam)
{
	t = skipq(t);
	*outparam = 0;
	switch (*t) {
	case 'c': *(int8_t *)out = (int8_t)g[0]; return 1;
	case 'C': *(uint8_t *)out = (uint8_t)g[0]; return 1;
	case 'B': *(bool *)out = (g[0] & 0xff) != 0; return 1;
	case 's': *(int16_t *)out = (int16_t)g[0]; return 1;
	case 'S': *(uint16_t *)out = (uint16_t)g[0]; return 1;
	case 'i': case 'l': *(int32_t *)out = (int32_t)g[0]; return 1;
	case 'I': case 'L': *(uint32_t *)out = g[0]; return 1;
	case 'q': *(int64_t *)out = (int32_t)g[0]; return 1;	// NSInteger was 32-bit
	case 'Q': *(uint64_t *)out = g[0]; return 1;
	case 'f': memcpy(out, g, 4); return 1;
	case 'd': memcpy(out, g, 8); return 2;
	case '@': case '#': *(id *)out = objc_unwrap(g[0]); return 1;
	case ':': *(SEL *)out = g[0] ? sel_registerName(G2H(g[0])) : NULL; return 1;
	case '*': *(char **)out = g[0] ? G2H(g[0]) : NULL; return 1;
	case '^':
		if (t[1] == '@' && g[0]) { *outparam = g[0]; *(void **)out = NULL; return 1; }	// id * out-param, fixed up later
		// opaque refs (CGImageRef, CGLContextObj, ...) arrive as handles;
		// anything else is a plain pointer into guest memory
		if (g[0]) {
			pthread_mutex_lock(&lk);
			uint64_t h = map_get(&g2h, g[0]);
			pthread_mutex_unlock(&lk);
			*(void **)out = h ? (void *)(uintptr_t)h : G2H(g[0]);
		} else {
			*(void **)out = NULL;
		}
		return 1;
	case '{': {
		float f[4];
		switch (struct_kind(t)) {
		case ST_RECT: memcpy(f, g, 16); for (int i = 0; i < 4; i++) ((double *)out)[i] = f[i]; return 4;
		case ST_PAIR_F: memcpy(f, g, 8); ((double *)out)[0] = f[0]; ((double *)out)[1] = f[1]; return 2;
		case ST_PAIR_U: ((uint64_t *)out)[0] = g[0]; ((uint64_t *)out)[1] = g[1]; return 2;
		default: die("objc: cannot pass struct %s from 32-bit code", t);
		}
	}
	default: die("objc: cannot pass type %s from 32-bit code", t);
	}
}

// host return value -> guest result (eax:edx, st(0) or the sret block)
static void ret_h2g(const char *t, const void *v, struct hres *r, g32 sret)
{
	t = skipq(t);
	switch (*t) {
	case 'v': return;
	case 'c': r->rax = (uint32_t)(int32_t)*(int8_t *)v; return;
	case 'C': r->rax = *(uint8_t *)v; return;
	case 'B': r->rax = *(bool *)v; return;
	case 's': r->rax = (uint32_t)(int32_t)*(int16_t *)v; return;
	case 'S': r->rax = *(uint16_t *)v; return;
	case 'i': case 'l': case 'I': case 'L': r->rax = *(uint32_t *)v; return;
	case 'q': case 'Q': r->rax = *(uint64_t *)v; return;
	case 'f': r->st0 = *(float *)v; r->flags |= F_ST0; return;
	case 'd': r->st0 = *(double *)v; r->flags |= F_ST0; return;
	case '@': case '#': r->rax = objc_wrap(*(id *)v); return;
	case ':': { SEL s = *(SEL *)v; r->rax = s ? canon_sel(sel_getName(s), 0) : 0; return; }
	case '*': { char *s = *(char **)v; r->rax = s ? guest_cstr(s) : 0; return; }
	case '^': r->rax = hobj_wrap(*(void **)v); return;
	case '{': {
		const double *d = v;
		float f[4];
		switch (struct_kind(t)) {
		case ST_RECT:
			for (int i = 0; i < 4; i++) f[i] = (float)d[i];
			if (!sret) die("objc: NSRect return without a struct pointer");
			memcpy(G2H(sret), f, 16);
			r->rax = sret;
			return;
		case ST_PAIR_F: f[0] = (float)d[0]; f[1] = (float)d[1]; memcpy(&r->rax, f, 8); return;
		case ST_PAIR_U: { const uint64_t *u = v; r->rax = (uint32_t)u[0] | (uint64_t)(uint32_t)u[1] << 32; return; }
		default: die("objc: cannot return struct %s to 32-bit code", t);
		}
	}
	default: die("objc: cannot return type %s to 32-bit code", t);
	}
}

// ---- handles ------------------------------------------------------------------
static g32 cfconst_isa;
static int is_block(g32 g);
static id host_block_for(g32 gb);
static void init_cfconst(g32 a) { cfconst_isa = a; }

g32 hobj_wrap(void *h)
{
	if (!h) return 0;
	pthread_mutex_lock(&lk);
	g32 g = (g32)map_get(&h2g, (uint64_t)(uintptr_t)h);
	if (!g) {
		g = gcalloc(1, 8);
		U32(g + 4) = 0x4a424f48;	// "HOBJ"
		map_put(&h2g, (uint64_t)(uintptr_t)h, g);
		map_put(&g2h, g, (uint64_t)(uintptr_t)h);
	}
	pthread_mutex_unlock(&lk);
	return g;
}

// a handle whose first word is chosen by the caller (e.g. a C++ vtable pointer)
g32 hobj_wrap_block(void *h, uint32_t size, g32 first_word)
{
	if (!h) return 0;
	pthread_mutex_lock(&lk);
	g32 g = (g32)map_get(&h2g, (uint64_t)(uintptr_t)h);
	if (!g || U32(g) != first_word) {
		g = gcalloc(1, size < 8 ? 8 : size);
		U32(g) = first_word;
		map_put(&h2g, (uint64_t)(uintptr_t)h, g);
		map_put(&g2h, g, (uint64_t)(uintptr_t)h);
	}
	pthread_mutex_unlock(&lk);
	return g;
}

static Class ensure_host_class(struct gclass *gc);

static struct gclass *gclass_of_host(Class c)
{
	for (; c; c = class_getSuperclass(c)) {
		struct gclass *gc = (void *)(uintptr_t)map_get(&gcls_by_host, (uint64_t)(uintptr_t)c);
		if (gc) return gc;
	}
	return NULL;
}

static g32 objc_wrap(id h)
{
	if (!h) return 0;
	pthread_mutex_lock(&lk);
	uint64_t key = (uint64_t)(uintptr_t)h;
	g32 g = (g32)map_get(&h2g, key);
	struct gclass *gc = NULL;
	if (object_isClass(h)) {
		gc = (void *)(uintptr_t)map_get(&gcls_by_host, key);
		if (gc) { pthread_mutex_unlock(&lk); return gc->cls; }
	} else {
		gc = gclass_of_host(object_getClass(h));
	}
	// an old handle for a freed object at the same address may have the wrong class
	if (g && gc && U32(g) != gc->cls) g = 0;
	if (!g && object_isClass(h)) {
		// a system class: shaped like an i386 class struct, since old
		// compiled code reads isa, super_class and name straight from it
		// ([super x] in a category class method loads the class's isa)
		g = gcalloc(1, 48);
		map_put(&h2g, key, g);
		map_put(&g2h, g, key);
		Class c = (Class)h, sup = class_getSuperclass(c);
		U32(g + C_ISA) = objc_wrap(object_getClass(c));	// cycles end at handles already mapped
		U32(g + C_SUPER) = sup ? objc_wrap(sup) : 0;
		U32(g + C_NAME) = gstrdup(class_getName(c));
		U32(g + 12) = (uint32_t)class_getVersion(c);
		U32(g + C_INFO) = class_isMetaClass(c) ? 2 : 1;		// CLS_META / CLS_CLASS
		U32(g + C_ISIZE) = (uint32_t)class_getInstanceSize(c);
	}
	if (!g) {
		uint32_t size = gc && gc->isize > 8 ? gc->isize : 8;
		g = gcalloc(1, size);
		if (gc) U32(g) = gc->cls;
		else U32(g + 4) = 0x4a424f48;
		map_put(&h2g, key, g);
		map_put(&g2h, g, key);
	}
	pthread_mutex_unlock(&lk);
	return g;
}

static id objc_unwrap(g32 g)
{
	if (!g) return nil;
	pthread_mutex_lock(&lk);
	id h = (__bridge id)(void *)(uintptr_t)map_get(&g2h, g);
	if (!h) {
		struct gclass *gc = (void *)(uintptr_t)map_get(&gcls_by_addr, g);
		if (gc) {
			Class c = ensure_host_class(gc);
			h = g == gc->meta ? object_getClass(c) : c;
		} else if (is_block(g)) {
			pthread_mutex_unlock(&lk);
			h = host_block_for(g);	// not cached: stack blocks reuse addresses
			pthread_mutex_lock(&lk);
		} else if (cfconst_isa && U32(g) == cfconst_isa) {
			// CFSTR("...") constant in a guest image: {isa, flags, char *, length}
			CFStringRef s = CFStringCreateWithBytes(NULL, G2H(U32(g + 8)), (CFIndex)U32(g + 12),
								kCFStringEncodingUTF8, false);
			if (!s) s = CFStringCreateWithBytes(NULL, G2H(U32(g + 8)), (CFIndex)U32(g + 12),
							    kCFStringEncodingMacRoman, false);
			h = (__bridge id)s;		// kept forever, like the constant it stands for
			map_put(&g2h, g, (uint64_t)(uintptr_t)s);
		} else {
			pthread_mutex_unlock(&lk);
			die("objc: %#x is not a known object", g);
		}
	}
	pthread_mutex_unlock(&lk);
	return h;
}

void *hobj_unwrap(g32 g) { return (__bridge void *)objc_unwrap(g); }

// guest copies of returned C strings, recycled round-robin
#define NSTR 1024
static struct { g32 g; uint32_t cap; } strs[NSTR];
static uint32_t nexts;
g32 guest_cstr(const char *s)
{
	uint32_t n = (uint32_t)strlen(s) + 1;
	pthread_mutex_lock(&lk);
	uint32_t i = nexts++ % NSTR;
	if (strs[i].cap < n) {
		gfree(strs[i].g);
		strs[i].cap = n < 64 ? 64 : n;
		strs[i].g = gmalloc(strs[i].cap);
	}
	memcpy(G2H(strs[i].g), s, n);
	g32 g = strs[i].g;
	pthread_mutex_unlock(&lk);
	return g;
}

// ---- dispatch: guest -> host ----------------------------------------------------
static g32 find_imp(struct gclass *gc, const char *sel, int classmethod, struct gclass **found)
{
	for (; gc; gc = gc->super) {
		struct gmeth *m = classmethod ? gc->cm : gc->im;
		int n = classmethod ? gc->ncm : gc->nim;
		for (int i = n - 1; i >= 0; i--)	// later lists (categories) win
			if (m[i].sel == sel || !strcmp(m[i].sel, sel)) { if (found) *found = gc; return m[i].imp; }
	}
	return 0;
}

// the first host ancestor of a guest class
static Class host_base(struct gclass *gc)
{
	while (gc->super) gc = gc->super;
	return objc_getClass(gc->host_super);
}

static g32 nsapp_var;
static void sync_nsapp(void)
{
	if (nsapp_var) U32(nsapp_var) = objc_wrap(NSApp);
}

enum { K_NORMAL, K_STRET, K_FPRET, K_SUPER, K_SUPER_STRET };

static void call_host(id target, Class super_cls, SEL hsel, const char *selname,
		      const uint32_t *ga, struct hres *r, g32 sret)
{
	NSMethodSignature *sig;
	if (super_cls) {
		Method m = object_isClass(target) ? class_getClassMethod(super_cls, hsel)
						  : class_getInstanceMethod(super_cls, hsel);
		sig = m ? [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(m)] : nil;
	} else {
		sig = [target methodSignatureForSelector:hsel];
	}
	if (!sig) {
		fprintf(stderr, "[mac32] objc: %s does not respond to %s\n",
			object_getClassName(target), selname);
		return;
	}
	// Our own reference: the message may be -release on the guest's autorelease
	// pool, which would otherwise free this invocation in the middle of -invoke.
	NSInvocation *inv = [[NSInvocation invocationWithMethodSignature:sig] retain];
	inv.target = target;
	inv.selector = hsel;
	uint8_t buf[64];
	g32 outp[16];
	id outv[16];
	NSUInteger n = sig.numberOfArguments;
	for (NSUInteger i = 2; i < n && i < 16; i++) {
		const char *t = [sig getArgumentTypeAtIndex:i];
		memset(buf, 0, sizeof buf);
		ga += arg_g2h(t, ga, buf, &outp[i]);
		if (outp[i]) {			// id * out-parameter
			outv[i] = nil;
			id *p = &outv[i];
			[inv setArgument:&p atIndex:(NSInteger)i];
		} else {
			[inv setArgument:buf atIndex:(NSInteger)i];
		}
	}
	@try {
		if (super_cls) [inv invokeUsingIMP:object_isClass(target) ? method_getImplementation(class_getClassMethod(super_cls, hsel))
									   : class_getMethodImplementation(super_cls, hsel)];
		else [inv invoke];
	} @catch (NSException *e) {
		// guest code cannot catch host exceptions: report where it was thrown
		fprintf(stderr, "[mac32] Objective-C exception in -[%s %s]: %s\n", object_getClassName(target), selname,
			e.reason.UTF8String);
		for (NSString *l in e.callStackSymbols) fprintf(stderr, "  %s\n", l.UTF8String);
		extern void print_recent_calls(void);
		print_recent_calls();
		@throw;
	}
	for (NSUInteger i = 2; i < n && i < 16; i++)
		if (outp[i]) U32(outp[i]) = objc_wrap(outv[i]);
	const char *rt = sig.methodReturnType;
	if (*skipq(rt) != 'v') {
		uint8_t rv[64] = {0};
		[inv getReturnValue:rv];
		ret_h2g(rt, rv, r, sret);
	}
	[inv release];
	sync_nsapp();
}

static void fast_enum(id target, const uint32_t *ga, struct hres *r);
const char *describe_obj(g32 g);

// Declared properties with no accessor methods and no ivars (Valve's i386
// SDL2 was built that way, so it would fail on a real 32-bit Mac too): give
// them storage on the host object, as @synthesize would have.
// class ext (cls+44): size, weakIvarLayout, property list {entsize, count, {name, attrs}...}
static int synth_property(struct gclass *gc, id target, const char *sel, const uint32_t *ga, struct hres *r)
{
	size_t sl = strlen(sel);
	int setter = sl > 4 && !strncmp(sel, "set", 3) && sel[sl - 1] == ':' && !strchr(sel, ':')[1];
	for (; gc; gc = gc->super) {
		g32 ext = U32(gc->cls + 44);
		if (!ext || U32(ext) < 12 || !U32(ext + 8)) continue;
		g32 pl = U32(ext + 8);
		uint32_t es = U32(pl), n = U32(pl + 4);
		for (uint32_t i = 0; i < n; i++) {
			const char *name = G2H(U32(pl + 8 + i * es)), *at = G2H(U32(pl + 12 + i * es));
			size_t nl = strlen(name);
			int match = setter ? sl == nl + 4 && toupper((unsigned char)name[0]) == sel[3] &&
						 !strncmp(sel + 4, name + 1, nl - 1)
					   : !strcmp(sel, name);
			if (!match) continue;
			if (strstr(at, ",G") || strstr(at, ",S")) return 0;	// custom accessor names: not ours
			char t = at[1];
			const void *key = name;		// guest strings never move
			if (t == '@') {
				if (setter) {
					objc_AssociationPolicy pol = strstr(at, ",C") ? OBJC_ASSOCIATION_COPY_NONATOMIC
								   : strstr(at, ",&") ? OBJC_ASSOCIATION_RETAIN_NONATOMIC
								   : OBJC_ASSOCIATION_ASSIGN;
					objc_setAssociatedObject(target, key, ga[0] ? objc_unwrap(ga[0]) : nil, pol);
				} else {
					r->rax = objc_wrap(objc_getAssociatedObject(target, key));
				}
				return 1;
			}
			int wide = t == 'q' || t == 'Q' || t == 'd';
			if (!strchr("cCsSiIlLfB^*qQd", t)) die("objc: cannot synthesize property %s (%s)", name, at);
			if (setter) {
				uint64_t v = ga[0] | (wide ? (uint64_t)ga[1] << 32 : 0);
				objc_setAssociatedObject(target, key, @(v), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
			} else {
				uint64_t v = [objc_getAssociatedObject(target, key) unsignedLongLongValue];
				if (t == 'f') { float f; uint32_t w = (uint32_t)v; memcpy(&f, &w, 4); r->st0 = f; r->flags |= F_ST0; }
				else if (t == 'd') { double dd; memcpy(&dd, &v, 8); r->st0 = dd; r->flags |= F_ST0; }
				else r->rax = wide ? v : (uint32_t)v;
			}
			return 1;
		}
	}
	return 0;
}

// NSTrackingRectTag is an NSInteger holding a 64-bit value: the guest gets a
// small number that stands for it
static NSTrackingRectTag tags[1024];
static int tracking_rect(id target, const char *sel, const uint32_t *ga, struct hres *r)
{
	if (!strcmp(sel, "addTrackingRect:owner:userData:assumeInside:")) {
		float f[4];
		memcpy(f, ga, 16);
		NSTrackingRectTag t = [target addTrackingRect:NSMakeRect(f[0], f[1], f[2], f[3]) owner:objc_unwrap(ga[4])
						     userData:(void *)(uintptr_t)ga[5] assumeInside:(ga[6] & 0xff) != 0];
		pthread_mutex_lock(&lk);
		uint32_t i = 1;
		while (i < 1024 && tags[i]) i++;
		if (i == 1024) die("objc: too many tracking rects");
		tags[i] = t;
		pthread_mutex_unlock(&lk);
		r->rax = i;
		return 1;
	}
	if (!strcmp(sel, "getRectsBeingDrawn:count:")) {	// (const NSRect **, NSInteger *)
		const NSRect *rs = NULL;
		NSInteger n = 0;
		[target getRectsBeingDrawn:&rs count:&n];
		static __thread g32 buf;
		static __thread uint32_t cap;
		if ((uint32_t)n > cap) { gfree(buf); cap = (uint32_t)n + 8; buf = gmalloc(cap * 16); }
		float *f = G2H(buf);
		for (NSInteger i = 0; i < n; i++) {
			f[i * 4] = (float)rs[i].origin.x; f[i * 4 + 1] = (float)rs[i].origin.y;
			f[i * 4 + 2] = (float)rs[i].size.width; f[i * 4 + 3] = (float)rs[i].size.height;
		}
		if (ga[0]) U32(ga[0]) = buf;
		if (ga[1]) U32(ga[1]) = (uint32_t)n;
		return 1;
	}
	if (!strcmp(sel, "removeTrackingRect:")) {
		uint32_t i = ga[0];
		if (i && i < 1024 && tags[i]) {
			[target removeTrackingRect:tags[i]];
			tags[i] = 0;
		}
		return 1;
	}
	return 0;
}

// graphicsContextWithWindow: contexts: remember the window, so a later draw
// into its CGContext can be shown (legacy.m, window_context_note)
extern void window_context_note(CGContextRef c, NSWindow *w);
static char ctx_window_key;
static int window_context(id target, const char *sel, const uint32_t *ga, struct hres *r)
{
	if (!strcmp(sel, "graphicsContextWithWindow:")) {
		NSWindow *w = objc_unwrap(ga[0]);
		NSGraphicsContext *c = [target graphicsContextWithWindow:w];
		if (!c && w) {
			// macOS returns nil for this now: stand in with a 1x1 bitmap
			// context kept with the window; images drawn into it are shown
			static char token_key;
			id token = objc_getAssociatedObject(w, &token_key);
			if (!token) {
				CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
				CGContextRef b = CGBitmapContextCreate(NULL, 1, 1, 8, 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedFirst);
				CGColorSpaceRelease(cs);
				token = (id)b;
				objc_setAssociatedObject(w, &token_key, token, OBJC_ASSOCIATION_RETAIN);
				CGContextRelease(b);
			}
			c = [NSGraphicsContext graphicsContextWithCGContext:(CGContextRef)token flipped:NO];
		}
		if (c) objc_setAssociatedObject(c, &ctx_window_key, w, OBJC_ASSOCIATION_ASSIGN);
		r->rax = objc_wrap(c);
		return 1;
	}
	if (!strcmp(sel, "graphicsPort") || !strcmp(sel, "CGContext")) {
		NSWindow *w = objc_getAssociatedObject(target, &ctx_window_key);
		CGContextRef c = [(NSGraphicsContext *)target CGContext];
		if (w) window_context_note(c, w);
		r->rax = hobj_wrap(c);
		return 1;
	}
	return 0;
}

// NSBitmapImageRep pixel memory must sit below 4 GB for 32-bit code to touch
// it. When the program lets AppKit allocate the planes, allocate them in guest
// memory instead and hand them over; they are freed with the image rep.
@interface M32GuestBlocks : NSObject { @public g32 b[5]; }
@end
@implementation M32GuestBlocks
- (void)dealloc { for (int i = 0; i < 5; i++) gfree(b[i]); [super dealloc]; }
@end
static char guest_planes_key;

static g32 low_or_zero(const void *p, const char *what)
{
	if (!p || (uintptr_t)p < 0x100000000ull) return H2G(p);
	LOG("objc: %s points above 4 GB; the program gets NULL\n", what);
	return 0;
}

static int bitmap_rep(id target, const char *sel, const uint32_t *ga, struct hres *r)
{
	int fmt = !strcmp(sel, "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:"
			       "isPlanar:colorSpaceName:bitmapFormat:bytesPerRow:bitsPerPixel:");
	if (fmt || !strcmp(sel, "initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:samplesPerPixel:hasAlpha:"
				"isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:")) {
		int32_t w = (int32_t)ga[1], h = (int32_t)ga[2], bps = (int32_t)ga[3], spp = (int32_t)ga[4];
		BOOL alpha = (ga[5] & 0xff) != 0, planar = (ga[6] & 0xff) != 0;
		NSString *cs = objc_unwrap(ga[7]);
		uint32_t format = fmt ? ga[8] : 0;
		int32_t bpr = (int32_t)ga[8 + fmt], bpp = (int32_t)ga[9 + fmt];
		int np = planar ? spp : 1;
		if (np < 1 || np > 5) np = 1;
		if (!bpr) bpr = (int32_t)(((int64_t)w * (planar ? bps : (bpp ? bpp : bps * spp)) + 7) / 8);
		unsigned char *hp[5] = { 0 };
		M32GuestBlocks *mine = nil;
		const uint32_t *gp = ga[0] ? G2H(ga[0]) : NULL;
		for (int i = 0; i < np; i++) {
			if (gp && gp[i]) { hp[i] = G2H(gp[i]); continue; }
			if (!mine) mine = [M32GuestBlocks new];
			size_t n = (size_t)bpr * (size_t)(h > 0 ? h : 0);
			mine->b[i] = gmalloc((uint32_t)(n ? n : 1));
			memset(G2H(mine->b[i]), 0, n);
			hp[i] = G2H(mine->b[i]);
		}
		NSBitmapImageRep *rep = fmt
			? [target initWithBitmapDataPlanes:hp pixelsWide:w pixelsHigh:h bitsPerSample:bps samplesPerPixel:spp
					 hasAlpha:alpha isPlanar:planar colorSpaceName:cs bitmapFormat:format
				       bytesPerRow:bpr bitsPerPixel:bpp]
			: [target initWithBitmapDataPlanes:hp pixelsWide:w pixelsHigh:h bitsPerSample:bps samplesPerPixel:spp
					 hasAlpha:alpha isPlanar:planar colorSpaceName:cs bytesPerRow:bpr bitsPerPixel:bpp];
		if (rep && mine) objc_setAssociatedObject(rep, &guest_planes_key, mine, OBJC_ASSOCIATION_RETAIN);
		[mine release];
		r->rax = objc_wrap(rep);
		return 1;
	}
	if (!strcmp(sel, "getBitmapDataPlanes:")) {	// (unsigned char **): five pointers
		unsigned char *hp[5] = { 0 };
		[(NSBitmapImageRep *)target getBitmapDataPlanes:hp];
		if (ga[0]) for (int i = 0; i < 5; i++) ((uint32_t *)G2H(ga[0]))[i] = low_or_zero(hp[i], "getBitmapDataPlanes:");
		return 1;
	}
	if (!strcmp(sel, "bitmapData")) {
		r->rax = low_or_zero([(NSBitmapImageRep *)target bitmapData], "bitmapData");
		return 1;
	}
	return 0;
}

// nil-terminated object lists (arrayWithObjects:, dictionaryWithObjectsAndKeys:
// ...): NSInvocation cannot carry varargs, so build them from the guest list
static int variadic_objects(id target, const char *sel, const uint32_t *ga, struct hres *r)
{
	static const char *const sels[] = {
		"arrayWithObjects:", "initWithObjects:", "setWithObjects:",
		"dictionaryWithObjectsAndKeys:", "initWithObjectsAndKeys:", NULL };
	int k = 0;
	while (sels[k] && strcmp(sels[k], sel)) k++;
	if (!sels[k]) return 0;
	NSMutableArray *v = [NSMutableArray array];
	for (int i = 0; ga[i] && i < 4096; i++) [v addObject:objc_unwrap(ga[i])];
	id out;
	if (k >= 3) {
		NSMutableDictionary *d = [NSMutableDictionary dictionary];
		for (NSUInteger i = 0; i + 1 < v.count; i += 2) d[v[i + 1]] = v[i];	// object, key
		out = k == 3 ? [[target class] dictionaryWithDictionary:d] : [target initWithDictionary:d];
	} else if (k == 2) {
		out = [target setWithArray:v];
	} else {
		out = k == 0 ? [target arrayWithArray:v] : [target initWithArray:v];
	}
	r->rax = objc_wrap(out);
	return 1;
}

static void msgsend(uint32_t *a, struct hres *r, int kind)
{
	g32 sret = 0;
	if (kind == K_STRET || kind == K_SUPER_STRET) { sret = *a++; r->flags |= F_POP4; }
	g32 self, gsel = a[1], supcls = 0;
	if (kind == K_SUPER || kind == K_SUPER_STRET) {
		self = U32(a[0]);
		supcls = U32(a[0] + 4);
		if (!supcls) {
			g32 off;
			const char *n = macho_addr_name(a[-1 - (kind == K_SUPER_STRET)], &off);
			die("objc: [super %s] with no superclass (objc_super at %#x = {%#x, 0}), from %s+%#x",
			    (char *)G2H(a[1]), a[0], self, n ? n : "?", off);
		}
	} else {
		self = a[0];
	}
	if (!self) return;
	const char *sel = G2H(gsel);
	extern int trace_calls;
	extern int trace_match(const char *);
	if (trace_calls && trace_match(sel)) fprintf(stderr, "[objc] %s%#x %s\n", supcls ? "super " : "", self, sel);
	const uint32_t *ga = a + 2;

	// a guest method?
	struct gclass *gc;
	int classmethod;
	if (supcls) {
		gc = (void *)(uintptr_t)map_get(&gcls_by_addr, supcls);
		classmethod = gc && supcls == gc->meta;
	} else {
		gc = (void *)(uintptr_t)map_get(&gcls_by_addr, self);
		classmethod = gc != NULL;
		if (!gc) gc = (void *)(uintptr_t)map_get(&gcls_by_addr, U32(self));
	}
	g32 imp = gc ? find_imp(gc, sel, classmethod, NULL) : 0;
	if (!imp && gc && !classmethod && !supcls && synth_property(gc, objc_unwrap(self), sel, ga, r)) return;
	if (!imp && !gc && !supcls) imp = host_category_imp(objc_unwrap(self), sel);
	if (imp) {
		uint32_t v[24];
		int k = 0;
		if (sret) v[k++] = sret;
		v[k++] = self;
		v[k++] = gsel;
		memcpy(&v[k], ga, 16 * 4);	// we do not know the count; extra words are harmless
		double st0;
		r->rax = gcallv(imp, k + 16, v, kind == K_FPRET ? &st0 : NULL);
		if (kind == K_FPRET) { r->st0 = st0; r->flags |= F_ST0; }
		return;
	}

	// a host method. For super calls, super_cls is the host class (never a
	// metaclass) whose implementation to use; call_host picks class or
	// instance methods from the target.
	id target = objc_unwrap(self);
	Class super_cls = nil;
	if (supcls) {
		if (gc) super_cls = host_base(gc);
		else {
			super_cls = objc_unwrap(supcls);
			if (class_isMetaClass(super_cls)) super_cls = objc_getClass(class_getName(super_cls));
		}
	}
	if (!strcmp(sel, "countByEnumeratingWithState:objects:count:")) { fast_enum(target, ga, r); return; }
	if (variadic_objects(target, sel, ga, r)) return;
	if (tracking_rect(target, sel, ga, r)) return;
	if ([target isKindOfClass:[NSBitmapImageRep class]] && bitmap_rep(target, sel, ga, r)) return;
	if ((target == (id)[NSGraphicsContext class] || [target isKindOfClass:[NSGraphicsContext class]]) &&
	    window_context(target, sel, ga, r)) return;
	if (!strcmp(sel, "stringWithFormat:")) {	// variadic: NSInvocation cannot carry it
		NSString *fmt = objc_unwrap(ga[0]);
		int n;
		char *c = gvformat_objc(fmt.UTF8String, ga + 1, &n, describe_obj);
		r->rax = objc_wrap([NSString stringWithUTF8String:c]);	// autoreleased into the guest's pool
		free(c);
		return;
	}
	// no autorelease pool here: returned objects belong to the guest's own pools
	call_host(target, super_cls, sel_registerName(sel), sel, ga, r, sret);
}

static void t_msgSend(uint32_t *a, struct hres *r) { msgsend(a, r, K_NORMAL); }
static void t_msgSend_stret(uint32_t *a, struct hres *r) { msgsend(a, r, K_STRET); }
static void t_msgSend_fpret(uint32_t *a, struct hres *r) { msgsend(a, r, K_FPRET); }
static void t_msgSendSuper(uint32_t *a, struct hres *r) { msgsend(a, r, K_SUPER); }
static void t_msgSendSuper_stret(uint32_t *a, struct hres *r) { msgsend(a, r, K_SUPER_STRET); }

// fast enumeration: i386 NSFastEnumerationState is {state, itemsPtr, mutationsPtr, extra[5]} in 4-byte words
static void fast_enum(id target, const uint32_t *ga, struct hres *r)
{
	g32 gstate = ga[0], gbuf = ga[1];
	uint32_t gcount = ga[2];
	static __thread NSFastEnumerationState *hs;
	static __thread g32 mut;
	if (!hs) hs = calloc(1, sizeof *hs);
	if (!mut) mut = gcalloc(1, 4);
	if (U32(gstate) == 0) memset(hs, 0, sizeof *hs);
	__unsafe_unretained id items[64];
	NSUInteger want = gcount < 64 ? gcount : 64;
	NSUInteger n = [target countByEnumeratingWithState:hs objects:items count:want];
	for (NSUInteger i = 0; i < n; i++) U32(gbuf + (g32)i * 4) = objc_wrap(hs->itemsPtr[i]);
	U32(gstate) = 1;
	U32(gstate + 4) = gbuf;
	U32(mut) = hs->mutationsPtr ? (uint32_t)*hs->mutationsPtr : 0;
	U32(gstate + 8) = mut;
	r->rax = (uint32_t)n;
}

// ---- dispatch: host -> guest (methods of guest classes) ---------------------------
static int arg_h2g(const char *t, NSInvocation *inv, NSUInteger i, uint32_t *g)
{
	t = skipq(t);
	uint8_t b[64] = {0};
	[inv getArgument:b atIndex:(NSInteger)i];
	switch (*t) {
	case 'c': g[0] = (uint32_t)(int32_t)*(int8_t *)b; return 1;
	case 'C': g[0] = *(uint8_t *)b; return 1;
	case 'B': g[0] = *(bool *)b; return 1;
	case 's': g[0] = (uint32_t)(int32_t)*(int16_t *)b; return 1;
	case 'S': g[0] = *(uint16_t *)b; return 1;
	case 'i': case 'l': case 'I': case 'L': g[0] = *(uint32_t *)b; return 1;
	case 'q': case 'Q': g[0] = (uint32_t)*(uint64_t *)b; return 1;
	case 'f': memcpy(g, b, 4); return 1;
	case 'd': memcpy(g, b, 8); return 2;
	case '@': case '#': g[0] = objc_wrap(*(__unsafe_unretained id *)b); return 1;
	case ':': { SEL s = *(SEL *)b; g[0] = s ? canon_sel(sel_getName(s), 0) : 0; return 1; }
	case '*': { char *s = *(char **)b; g[0] = s ? guest_cstr(s) : 0; return 1; }
	case '^': { void *p = *(void **)b; g[0] = (uintptr_t)p < LOW_END ? (g32)(uintptr_t)p : hobj_wrap(p); return 1; }
	case '{': {
		const double *d = (const double *)b;
		float f[4];
		switch (struct_kind(t)) {
		case ST_RECT: for (int k = 0; k < 4; k++) f[k] = (float)d[k]; memcpy(g, f, 16); return 4;
		case ST_PAIR_F: f[0] = (float)d[0]; f[1] = (float)d[1]; memcpy(g, f, 8); return 2;
		case ST_PAIR_U: { const uint64_t *u = (const uint64_t *)b; g[0] = (uint32_t)u[0]; g[1] = (uint32_t)u[1]; return 2; }
		default: die("objc: cannot pass struct %s to 32-bit code", t);
		}
	}
	default: die("objc: cannot pass type %s to 32-bit code", t);
	}
}

static void forward_to_guest(id self, SEL _cmd, NSInvocation *inv)
{
	(void)_cmd;
	const char *sel = sel_getName(inv.selector);
	int classmethod = object_isClass(self);
	extern int trace_calls;
	extern int trace_match(const char *);
	if (trace_calls && trace_match(sel)) fprintf(stderr, "[objc<-host] %s %s\n", object_getClassName(self), sel);
	struct gclass *gc = gclass_of_host(classmethod ? (Class)self : object_getClass(self));
	g32 imp = gc ? find_imp(gc, sel, classmethod, NULL) : 0;
	if (!imp) {
		Class base = host_base(gc);
		struct objc_super sup = { self, classmethod ? object_getClass(base) : base };
		((void (*)(struct objc_super *, SEL, NSInvocation *))objc_msgSendSuper)(&sup, @selector(forwardInvocation:), inv);
		return;
	}
	NSMethodSignature *sig = inv.methodSignature;
	const char *rt = skipq(sig.methodReturnType);
	uint32_t v[40];
	int k = 0;
	g32 sret = 0;
	if (struct_kind(rt) == ST_RECT) { sret = gcalloc(1, 16); v[k++] = sret; }
	v[k++] = classmethod ? gc->cls : objc_wrap(self);
	v[k++] = canon_sel(sel, 0);
	for (NSUInteger i = 2; i < sig.numberOfArguments && k < 36; i++)
		k += arg_h2g([sig getArgumentTypeAtIndex:i], inv, i, &v[k]);
	double st0 = 0;
	int fp = *rt == 'f' || *rt == 'd';
	uint64_t rax = gcallv(imp, k, v, fp ? &st0 : NULL);
	if (*rt == 'v') return;
	uint8_t out[64] = {0};
	switch (*rt) {
	case 'c': case 'C': *(uint8_t *)out = (uint8_t)rax; break;
	case 'B': *(bool *)out = (rax & 0xff) != 0; break;
	case 's': case 'S': *(uint16_t *)out = (uint16_t)rax; break;
	case 'i': case 'I': case 'l': case 'L': *(uint32_t *)out = (uint32_t)rax; break;
	case 'q': *(int64_t *)out = (int32_t)rax; break;
	case 'Q': *(uint64_t *)out = (uint32_t)rax; break;
	case 'f': *(float *)out = (float)st0; break;
	case 'd': *(double *)out = st0; break;
	case '@': case '#': *(__unsafe_unretained id *)out = objc_unwrap((g32)rax); break;
	case ':': *(SEL *)out = rax ? sel_registerName(G2H((g32)rax)) : NULL; break;
	case '*': case '^': *(void **)out = rax ? G2H((g32)rax) : NULL; break;
	case '{': {
		float f[4];
		double *d = (double *)out;
		switch (struct_kind(rt)) {
		case ST_RECT: memcpy(f, G2H(sret), 16); for (int i = 0; i < 4; i++) d[i] = f[i]; gfree(sret); break;
		case ST_PAIR_F: memcpy(f, &rax, 8); d[0] = f[0]; d[1] = f[1]; break;
		case ST_PAIR_U: ((uint64_t *)out)[0] = (uint32_t)rax; ((uint64_t *)out)[1] = (uint32_t)(rax >> 32); break;
		default: die("objc: cannot return struct %s from 32-bit code", rt);
		}
		break;
	}
	default: die("objc: cannot return type %s from 32-bit code", rt);
	}
	[inv setReturnValue:out];
}

// i386 type string -> x86_64 one, for guest methods the host class does not know
static char *host_types(const char *g)
{
	NSMutableString *s = [NSMutableString string];
	for (const char *p = g; *p; p++) {
		if (*p >= '0' && *p <= '9') continue;			// frame offsets
		if (*p == '"') {					// @"ClassName"
			const char *e = strchr(p + 1, '"');
			if (!e) break;
			p = e;
			continue;
		}
		if (*p == '^' && (p[1] == '{' || p[1] == '(')) {	// pointer to a struct: its layout does not matter
			[s appendString:@"^v"];
			int depth = 0;
			p++;
			do { if (*p == '{' || *p == '(') depth++; else if (*p == '}' || *p == ')') depth--; } while (depth && *++p);
			continue;
		}
		if (*p == '(') {					// a union: NSMethodSignature cannot parse them
			[s appendString:@"q"];				// (vlc_value_t and friends are 8 bytes)
			int depth = 0;
			do { if (*p == '{' || *p == '(') depth++; else if (*p == '}' || *p == ')') depth--; } while (depth && *++p);
			continue;
		}
		if (!strncmp(p, "{_NSRect=", 9)) { [s appendString:@"{CGRect={CGPoint=dd}{CGSize=dd}}"]; p = strstr(p, "}}") + 1; continue; }
		if (!strncmp(p, "{_NSPoint=ff}", 13)) { [s appendString:@"{CGPoint=dd}"]; p += 12; continue; }
		if (!strncmp(p, "{_NSSize=ff}", 12)) { [s appendString:@"{CGSize=dd}"]; p += 11; continue; }
		if (!strncmp(p, "{_NSRange=II}", 13)) { [s appendString:@"{_NSRange=QQ}"]; p += 12; continue; }	// NSUInteger fields
		if (*p == '{' || *p == '(') {				// struct name stays as it is
			const char *e = p + 1;
			while (*e && *e != '=' && *e != '}' && *e != ')') e++;
			[s appendString:[[[NSString alloc] initWithBytes:p length:(NSUInteger)(e - p) encoding:NSUTF8StringEncoding] autorelease]];
			p = e - 1;
			continue;
		}
		char c = *p == 'l' ? 'i' : *p == 'L' ? 'I' : *p;	// long was 32-bit
		[s appendFormat:@"%c", c];
	}
	return strdup(s.UTF8String);
}

static IMP fwd_for(const char *types)
{
	NSMethodSignature *sig = [NSMethodSignature signatureWithObjCTypes:types];
	const char *rt = skipq(sig.methodReturnType);
	if (*rt == '{' && sig.methodReturnLength > 16) return (IMP)_objc_msgForward_stret;
	return _objc_msgForward;
}

// The runtime calls .cxx_destruct/.cxx_construct as dtor(obj), with no
// selector: a forwarding stub there reads a stale register as the selector
// (the random "unrecognized selector" crashes in SDL's text-input view).
// These IMPs ignore _cmd and call the guest's own method.
static void cxx_special(id self, const char *name)
{
	struct gclass *gc = gclass_of_host(object_getClass(self));
	g32 imp = gc ? find_imp(gc, name, 0, NULL) : 0;
	if (imp) gcall(imp, 2, objc_wrap(self), canon_sel(name, 0));
}
static void cxx_destruct_imp(id self, SEL _cmd) { (void)_cmd; cxx_special(self, ".cxx_destruct"); }
static id cxx_construct_imp(id self, SEL _cmd) { (void)_cmd; cxx_special(self, ".cxx_construct"); return self; }

static void install(Class cls, Class lookup, struct gmeth *m, int n, int meta)
{
	for (int i = 0; i < n; i++) {
		if (!meta && !strcmp(m[i].sel, ".cxx_destruct")) {
			class_replaceMethod(cls, sel_registerName(".cxx_destruct"), (IMP)cxx_destruct_imp, "v@:");
			continue;
		}
		if (!meta && !strcmp(m[i].sel, ".cxx_construct")) {
			class_replaceMethod(cls, sel_registerName(".cxx_construct"), (IMP)cxx_construct_imp, "@@:");
			continue;
		}
		SEL s = sel_registerName(m[i].sel);
		Method hm = meta ? class_getClassMethod(lookup, s) : class_getInstanceMethod(lookup, s);
		const char *t = hm ? method_getTypeEncoding(hm) : host_types(m[i].types);
		class_replaceMethod(cls, s, fwd_for(t), t);
	}
}

static Class ensure_host_class(struct gclass *gc)
{
	if (gc->host) return gc->host;
	pthread_mutex_lock(&lk);
	if (!gc->host) {
		Class parent = gc->super ? ensure_host_class(gc->super) : objc_getClass(gc->host_super);
		char name[256];
		snprintf(name, sizeof name, "%s", gc->name);
		if (objc_getClass(name)) snprintf(name, sizeof name, "mac32_%s", gc->name);
		Class c = objc_allocateClassPair(parent, name, 0);
		install(c, parent, gc->im, gc->nim, 0);
		install(object_getClass(c), parent, gc->cm, gc->ncm, 1);
		class_addMethod(c, @selector(forwardInvocation:), (IMP)forward_to_guest, "v@:@");
		class_addMethod(object_getClass(c), @selector(forwardInvocation:), (IMP)forward_to_guest, "v@:@");
		objc_registerClassPair(c);
		map_put(&gcls_by_host, (uint64_t)(uintptr_t)c, (uint64_t)(uintptr_t)gc);
		gc->host = c;
		LOG("objc: made host class %s : %s\n", name, class_getName(parent));
	}
	pthread_mutex_unlock(&lk);
	return gc->host;
}

// ---- the guest app's bundle stands in for the main bundle ---------------------------
static NSBundle *guest_bundle;
static id main_bundle_imp(id self, SEL _cmd) { (void)self; (void)_cmd; return guest_bundle; }
void set_guest_bundle(const char *path)
{
	guest_bundle = [[NSBundle bundleWithPath:[NSString stringWithUTF8String:path]] retain];
	if (!guest_bundle) die("%s is not a bundle", path);
	method_setImplementation(class_getClassMethod([NSBundle class], @selector(mainBundle)), (IMP)main_bundle_imp);
	LOG("main bundle is now %s\n", path);
}
void *guest_main_cfbundle(void)
{
	if (!guest_bundle) return (void *)CFBundleGetMainBundle();
	static CFBundleRef b;
	if (!b) b = CFBundleCreate(NULL, (__bridge CFURLRef)guest_bundle.bundleURL);
	return b;
}

// ---- system data constants the guest imports by name ----------------------------
// NSFontAttributeName, kCFAllocatorSystemDefault, ...: a host global holding an
// object. The guest gets a 4-byte cell with a handle to that object. Values
// that are not objects (structs, doubles, callback tables) are not guessed.
#include <dlfcn.h>
#include <malloc/malloc.h>
static NSMutableSet *class_set;
static int is_host_object(void *v)
{
	if (!v || ((uintptr_t)v & 7)) return 0;
	Dl_info di;
	if (!malloc_zone_from_ptr(v) && !dladdr(v, &di)) return 0;	// heap or inside an image
	if (!class_set) {
		unsigned n = 0;
		Class *cl = objc_copyClassList(&n);
		class_set = [[NSMutableSet alloc] initWithCapacity:n];
		for (unsigned i = 0; i < n; i++) [class_set addObject:[NSValue valueWithPointer:(__bridge void *)cl[i]]];
		free(cl);
	}
	uintptr_t isa = *(uintptr_t *)v & 0x00007ffffffffff8ull;	// x86_64 ISA_MASK
	return [class_set containsObject:[NSValue valueWithPointer:(void *)isa]];
}
g32 host_data_object(const char *name)
{
	if (name[0] != '_') return 0;
	void **p = dlsym(RTLD_DEFAULT, name + 1);
	if (!p) return 0;
	void *v = *p;
	pthread_mutex_lock(&lk);
	int ok = is_host_object(v);
	pthread_mutex_unlock(&lk);
	if (!ok) return 0;
	g32 cell = gcalloc(1, 8);
	U32(cell) = objc_wrap((__bridge id)v);
	LOG("data constant %s from the host\n", name);
	return cell;
}

// ---- C entry points -------------------------------------------------------------
static void t_NSClassFromString(uint32_t *args, struct hres *r)
{
	NSString *s = objc_unwrap(A(0));
	struct gclass *gc = gclass_named(s.UTF8String);
	r->rax = gc ? gc->cls : objc_wrap(NSClassFromString(s));
}
const char *describe_obj(g32 g)
{
	id o = objc_unwrap(g);
	return o ? [[o description] UTF8String] : "(null)";
}
static void t_NSLog(uint32_t *args, struct hres *r)
{
	(void)r;
	@autoreleasepool {
		NSString *fmt = objc_unwrap(A(0));
		int n;
		char *s = gvformat_objc(fmt.UTF8String, &args[1], &n, describe_obj);
		fprintf(stderr, "[guest NSLog] %s\n", s);
		free(s);
	}
}
// NSRunAlertPanel(title, msgFormat, default, alternate, other, ...): show it, and log it
static void t_NSRunAlertPanel(uint32_t *args, struct hres *r)
{
	@autoreleasepool {
		NSString *title = A(0) ? objc_unwrap(A(0)) : @"";
		NSString *fmt = A(1) ? objc_unwrap(A(1)) : @"";
		int n;
		char *c = gvformat_objc(fmt.UTF8String, &args[5], &n, describe_obj);
		NSString *msg = [NSString stringWithUTF8String:c];
		free(c);
		fprintf(stderr, "[mac32] alert: %s: %s\n", title.UTF8String, msg.UTF8String);
		NSAlert *a = [[NSAlert new] autorelease];
		a.messageText = title;
		a.informativeText = msg;
		NSString *b[3] = { A(2) ? objc_unwrap(A(2)) : @"OK", A(3) ? objc_unwrap(A(3)) : nil, A(4) ? objc_unwrap(A(4)) : nil };
		for (int i = 0; i < 3; i++) if (b[i]) [a addButtonWithTitle:b[i]];
		NSModalResponse resp = [a runModal];
		// NSAlertDefaultReturn 1, NSAlertAlternateReturn 0, NSAlertOtherReturn -1
		RET(resp == NSAlertFirstButtonReturn ? 1 : resp == NSAlertSecondButtonReturn ? 0 : (uint32_t)-1);
	}
}
// synthesized accessors of the 10.5+ runtime: (self, _cmd, offset, value, atomic, copy)
static void t_objc_setProperty(uint32_t *args, struct hres *r)
{
	(void)r;
	g32 slot = A(0) + A(2), nv = A(3), old = U32(slot);
	if (nv) {
		id o = objc_unwrap(nv);
		if (A(5) & 0xff) nv = objc_wrap([[o copy] autorelease]), o = objc_unwrap(nv);
		[o retain];
	}
	U32(slot) = nv;
	if (old) [objc_unwrap(old) release];
}
static void t_objc_getProperty(uint32_t *args, struct hres *r)	// (self, _cmd, offset, atomic)
{
	g32 v = U32(A(0) + A(2));
	if (v && (A(3) & 0xff)) [[objc_unwrap(v) retain] autorelease];
	RET(v);
}
static void t_enum_mutation(uint32_t *args, struct hres *r) { (void)args; (void)r; die("objc: collection mutated during enumeration"); }
static void init_nsapp(g32 a) { nsapp_var = a; U32(a) = objc_wrap(NSApp); }
static void init_runloopmode(g32 a) { U32(a) = objc_wrap(NSDefaultRunLoopMode); }

// ---- ARC entry points (SDL2 and other clang-built code) ---------------------
// A guest object is a handle; ARC calls act on the host object behind it and
// hand back the same handle.
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *);
#include <objc/objc-sync.h>

static id arc_obj(g32 g) { return g ? objc_unwrap(g) : nil; }
static void t_objc_retain(uint32_t *args, struct hres *r) { [arc_obj(A(0)) retain]; RET(A(0)); }
static void t_objc_release(uint32_t *args, struct hres *r) { (void)r; [arc_obj(A(0)) release]; }
static void t_objc_autorelease(uint32_t *args, struct hres *r) { [arc_obj(A(0)) autorelease]; RET(A(0)); }
static void t_objc_retainAutorelease(uint32_t *args, struct hres *r) { [[arc_obj(A(0)) retain] autorelease]; RET(A(0)); }
static void t_objc_storeStrong(uint32_t *args, struct hres *r)
{
	(void)r;
	g32 *loc = G2H(A(0)), obj = A(1), old = *loc;
	if (old == obj) return;
	[arc_obj(obj) retain];
	*loc = obj;
	[arc_obj(old) release];
}
// weak references are not zeroed when the object dies: fine for the window
// and view back-pointers SDL keeps, which outlive their users
static void t_objc_storeWeak(uint32_t *args, struct hres *r) { *(g32 *)G2H(A(0)) = A(1); RET(A(1)); }
static void t_objc_loadWeakRetained(uint32_t *args, struct hres *r) { g32 g = *(g32 *)G2H(A(0)); [arc_obj(g) retain]; RET(g); }
static void t_objc_loadWeak(uint32_t *args, struct hres *r) { RET(*(g32 *)G2H(A(0))); }
static void t_objc_destroyWeak(uint32_t *args, struct hres *r) { (void)r; *(g32 *)G2H(A(0)) = 0; }
static void t_objc_copyWeak(uint32_t *args, struct hres *r) { (void)r; *(g32 *)G2H(A(0)) = *(g32 *)G2H(A(1)); }

// pool tokens are host pointers: the guest gets the depth instead
static __thread void *pools[1024];
static __thread uint32_t npools;
static void t_pool_push(uint32_t *args, struct hres *r)
{
	(void)args;
	if (npools == 1024) die("objc: autorelease pools nested too deep");
	pools[npools++] = objc_autoreleasePoolPush();
	RET(npools);
}
static void t_pool_pop(uint32_t *args, struct hres *r)
{
	(void)r;
	uint32_t d = A(0);
	if (!d || d > npools) return;
	objc_autoreleasePoolPop(pools[d - 1]);
	npools = d - 1;
}
static void t_objc_sync_enter(uint32_t *args, struct hres *r) { RET(objc_sync_enter(arc_obj(A(0)))); }
static void t_objc_sync_exit(uint32_t *args, struct hres *r) { RET(objc_sync_exit(arc_obj(A(0)))); }

// ---- blocks -----------------------------------------------------------------
// i386 block: isa, flags, reserved, invoke, descriptor, captures...
// descriptor: reserved, size, [copy, dispose], [signature]
enum { BLK_REFCOUNT = 0xfffe, BLK_NEEDS_FREE = 1 << 24, BLK_COPY_DISPOSE = 1 << 25,
       BLK_IS_GLOBAL = 1 << 28, BLK_HAS_SIG = 1 << 30 };
static g32 isa_stack, isa_malloc, isa_global;
static void init_isa_stack(g32 a) { isa_stack = a; }
static void init_isa_malloc(g32 a) { isa_malloc = a; }
static void init_isa_global(g32 a) { isa_global = a; }
static int is_block(g32 g) { g32 i = U32(g); return i && (i == isa_stack || i == isa_malloc || i == isa_global); }

static g32 block_copy(g32 b)
{
	if (!b) return 0;
	g32 isa = U32(b);
	if (isa == isa_global) return b;
	if (isa == isa_malloc) { __sync_fetch_and_add((uint32_t *)G2H(b + 4), 2); return b; }
	g32 desc = U32(b + 16);
	uint32_t size = U32(desc + 4), flags = U32(b + 4);
	g32 n = gmalloc(size);
	memcpy(G2H(n), G2H(b), size);
	U32(n) = isa_malloc;
	U32(n + 4) = (flags & ~BLK_REFCOUNT) | BLK_NEEDS_FREE | 2;
	if (flags & BLK_COPY_DISPOSE) gcall(U32(desc + 8), 2, n, b);
	return n;
}
static void block_release(g32 b)
{
	if (!b || U32(b) != isa_malloc) return;
	uint32_t f = __sync_sub_and_fetch((uint32_t *)G2H(b + 4), 2);
	if (f & BLK_REFCOUNT) return;
	if (f & BLK_COPY_DISPOSE) gcall(U32(U32(b + 16) + 12), 1, b);
	gfree(b);
}

// __block variables: isa, forwarding, flags, size, [keep, destroy], value
static g32 byref_copy(g32 src)
{
	g32 fwd = U32(src + 4);
	if (U32(fwd + 8) & BLK_NEEDS_FREE) { __sync_fetch_and_add((uint32_t *)G2H(fwd + 8), 2); return fwd; }
	uint32_t size = U32(src + 12), flags = U32(src + 8);
	g32 n = gmalloc(size);
	memcpy(G2H(n), G2H(src), size);
	U32(n + 4) = n;
	U32(src + 4) = n;
	U32(n + 8) = (flags & ~BLK_REFCOUNT) | BLK_NEEDS_FREE | 4;	// one for the stack copy, one for the block
	if (flags & BLK_COPY_DISPOSE) gcall(U32(src + 16), 2, n, src);
	return n;
}

static void t_Block_object_assign(uint32_t *args, struct hres *r)
{
	(void)r;
	g32 *dst = G2H(A(0)), src = A(1);
	uint32_t f = A(2);
	if (f & 128) { *dst = src; return; }		// BLOCK_BYREF_CALLER: plain store
	switch (f & 0x1f) {
	case 3: [arc_obj(src) retain]; *dst = src; break;		// object
	case 7: *dst = block_copy(src); break;			// block
	case 8: case 24: *dst = byref_copy(src); break;		// __block (weak) variable
	default: *dst = src; break;
	}
}
static void t_Block_object_dispose(uint32_t *args, struct hres *r)
{
	(void)r;
	g32 o = A(0);
	switch (A(1) & 0x1f) {
	case 3: [arc_obj(o) release]; break;
	case 7: block_release(o); break;
	default: break;		// __block storage: kept (small, and other blocks may share it)
	}
}
static void t_Block_copy(uint32_t *args, struct hres *r) { RET(block_copy(A(0))); }
static void t_Block_release(uint32_t *args, struct hres *r) { (void)r; block_release(A(0)); }

// A guest block handed to a host method becomes a host block that calls it.
// Its argument and return types come from the block's signature.
static const char *block_sig(g32 b)
{
	uint32_t f = U32(b + 4);
	if (!(f & BLK_HAS_SIG)) return NULL;
	g32 d = U32(b + 16);
	return G2H(U32(d + ((f & BLK_COPY_DISPOSE) ? 16 : 8)));
}
static const char *next_type(const char *t)	// past one type and its offset
{
	t = skipq(t);
	if (*t == '{' || *t == '(') {
		int depth = 0;
		do { if (*t == '{' || *t == '(') depth++; else if (*t == '}' || *t == ')') depth--; t++; } while (depth && *t);
	} else if (*t == '^') return next_type(t + 1);
	else if (*t == '@' && t[1] == '?') t += 2;
	else if (*t == '@' && t[1] == '"') { t = strchr(t + 2, '"') + 1; }
	else t++;
	while (*t >= '0' && *t <= '9') t++;
	return t;
}
static id host_block_for(g32 gb)
{
	g32 b = block_copy(gb);
	const char *sig = block_sig(b);
	if (!sig) die("objc: a block without a signature was passed to the host");
	char ret = *skipq(sig);
	struct { char c[4]; } bt = {{0}};
	char *at = bt.c;
	int n = 0;
	const char *t = next_type(next_type(sig));	// skip the return type and the block itself
	while (*t && n < 4) { at[n++] = *skipq(t); t = next_type(t); }
	for (int i = 0; i < n; i++)
		if (!strchr("@#ciIlLsSCBq*^", at[i])) die("objc: block argument type %c not bridged (%s)", at[i], sig);
	if (!strchr("v@cCBiIlL", ret)) die("objc: block return type %c not bridged (%s)", ret, sig);
	void *(^hb)(void *, void *, void *, void *) = ^void *(void *a0, void *a1, void *a2, void *a3) {
		void *in[4] = { a0, a1, a2, a3 };
		const char *at = bt.c;
		uint32_t v[5];
		v[0] = b;
		for (int i = 0; i < n; i++)
			v[i + 1] = at[i] == '@' || at[i] == '#' ? objc_wrap((id)in[i])
				 : at[i] == '*' || at[i] == '^' ? ((uintptr_t)in[i] < LOW_END ? (g32)(uintptr_t)in[i] : hobj_wrap(in[i]))
				 : (uint32_t)(uintptr_t)in[i];
		uint64_t rv = gcallv(U32(b + 12), n + 1, v, NULL);
		if (ret == '@') return (void *)objc_unwrap((g32)rv);
		if (ret == 'v') return NULL;
		return (void *)(uintptr_t)(ret == 'c' || ret == 'C' || ret == 'B' ? (rv & 0xff) : (uint32_t)rv);
	};
	return [hb copy];	// kept for the life of the process, like the guest copy
}

// ---- dispatch with guest blocks -----------------------------------------------
static g32 main_q;
static void init_main_q(g32 a) { main_q = a; }
static dispatch_queue_t host_queue(g32 q) { return q == main_q ? dispatch_get_main_queue() : (dispatch_queue_t)objc_unwrap(q); }
static void t_dispatch_async(uint32_t *args, struct hres *r)
{
	(void)r;
	g32 b = block_copy(A(1));
	dispatch_async(host_queue(A(0)), ^{ gcall(U32(b + 12), 1, b); block_release(b); });
}
static void t_dispatch_sync(uint32_t *args, struct hres *r)
{
	(void)r;
	g32 b = A(1);
	dispatch_sync(host_queue(A(0)), ^{ gcall(U32(b + 12), 1, b); });
}
static void t_dispatch_get_main_queue(uint32_t *args, struct hres *r) { (void)args; RET(main_q); }

// ---- AppKit / Foundation constants ------------------------------------------------
#define NSCONST(sym) static void init_##sym(g32 a) { U32(a) = objc_wrap((id)sym); }
NSCONST(NSApplicationDidBecomeActiveNotification) NSCONST(NSApplicationDidHideNotification)
NSCONST(NSApplicationDidUnhideNotification) NSCONST(NSApplicationWillUnhideNotification) NSCONST(NSBackingPropertyOldScaleFactorKey)
NSCONST(NSCurrentLocaleDidChangeNotification) NSCONST(NSDeviceRGBColorSpace)
NSCONST(NSFilenamesPboardType) NSCONST(NSStringPboardType) NSCONST(NSPasteboardTypeString) NSCONST(NSURLIsAliasFileKey)
NSCONST(NSWindowDidBecomeKeyNotification) NSCONST(NSWindowDidChangeBackingPropertiesNotification)
NSCONST(NSWindowDidChangeScreenProfileNotification) NSCONST(NSWindowDidDeminiaturizeNotification)
NSCONST(NSWindowDidEnterFullScreenNotification) NSCONST(NSWindowDidExitFullScreenNotification)
NSCONST(NSWindowDidExposeNotification) NSCONST(NSWindowDidMiniaturizeNotification)
NSCONST(NSWindowDidMoveNotification) NSCONST(NSWindowDidResignKeyNotification)
NSCONST(NSWindowDidResizeNotification) NSCONST(NSWindowWillCloseNotification)
NSCONST(NSWindowWillEnterFullScreenNotification) NSCONST(NSWindowWillExitFullScreenNotification)
static void init_appkit_version(g32 a) { double v = NSAppKitVersionNumber; memcpy(G2H(a), &v, 8); }
static void init_zero16(g32 a) { memset(G2H(a), 0, 16); }
#define NSC(sym) D("_" #sym, 4, init_##sym)
// a guest SEL is its canonical name string
static void t_sel_registerName(uint32_t *args, struct hres *r) { pthread_mutex_lock(&lk); RET(canon_sel(G2H(A(0)), 0)); pthread_mutex_unlock(&lk); }
static void t_sel_getName(uint32_t *args, struct hres *r) { RET(A(0)); }

static struct hfn objc_fns[] = {
	R("_objc_msgSend", t_msgSend), R("_objc_msgSend_stret", t_msgSend_stret),
	R("_objc_msgSend_fpret", t_msgSend_fpret), R("_objc_msgSendSuper", t_msgSendSuper),
	R("_objc_msgSendSuper_stret", t_msgSendSuper_stret),
	R("_objc_enumerationMutation", t_enum_mutation),
	R("_objc_setProperty", t_objc_setProperty), R("_objc_getProperty", t_objc_getProperty),
	R("_NSClassFromString", t_NSClassFromString), R("_NSRunAlertPanel", t_NSRunAlertPanel), R("_NSLog", t_NSLog),
	D("_NSApp", 4, init_nsapp), D("_NSDefaultRunLoopMode", 4, init_runloopmode),
	D("___CFConstantStringClassReference", 32, init_cfconst),
	R("_objc_retain", t_objc_retain), R("_objc_release", t_objc_release),
	R("_objc_autorelease", t_objc_autorelease), R("_objc_autoreleaseReturnValue", t_objc_autorelease),
	R("_objc_retainAutoreleasedReturnValue", t_objc_retain), R("_objc_unsafeClaimAutoreleasedReturnValue", t_objc_retain),
	R("_objc_retainAutorelease", t_objc_retainAutorelease), R("_objc_retainAutoreleaseReturnValue", t_objc_retainAutorelease),
	R("_objc_storeStrong", t_objc_storeStrong), R("_objc_storeWeak", t_objc_storeWeak), R("_objc_initWeak", t_objc_storeWeak),
	R("_objc_loadWeakRetained", t_objc_loadWeakRetained), R("_objc_loadWeak", t_objc_loadWeak),
	R("_objc_destroyWeak", t_objc_destroyWeak), R("_objc_copyWeak", t_objc_copyWeak),
	R("_objc_autoreleasePoolPush", t_pool_push), R("_objc_autoreleasePoolPop", t_pool_pop),
	R("_objc_sync_enter", t_objc_sync_enter), R("_objc_sync_exit", t_objc_sync_exit),
	D("__NSConcreteStackBlock", 32, init_isa_stack), D("__NSConcreteMallocBlock", 32, init_isa_malloc),
	D("__NSConcreteGlobalBlock", 32, init_isa_global),
	R("__Block_object_assign", t_Block_object_assign), R("__Block_object_dispose", t_Block_object_dispose),
	R("__Block_copy", t_Block_copy), R("__Block_release", t_Block_release),
	D("__dispatch_main_q", 128, init_main_q), R("_dispatch_get_main_queue", t_dispatch_get_main_queue),
	R("_dispatch_async", t_dispatch_async), R("_dispatch_sync", t_dispatch_sync),
	NSC(NSApplicationDidBecomeActiveNotification), NSC(NSApplicationDidHideNotification),
	NSC(NSApplicationDidUnhideNotification), NSC(NSApplicationWillUnhideNotification), NSC(NSBackingPropertyOldScaleFactorKey),
	NSC(NSCurrentLocaleDidChangeNotification), NSC(NSDeviceRGBColorSpace),
	NSC(NSFilenamesPboardType), NSC(NSStringPboardType),
	R("_sel_registerName", t_sel_registerName), R("_sel_getUid", t_sel_registerName), R("_sel_getName", t_sel_getName), NSC(NSPasteboardTypeString), NSC(NSURLIsAliasFileKey),
	NSC(NSWindowDidBecomeKeyNotification), NSC(NSWindowDidChangeBackingPropertiesNotification),
	NSC(NSWindowDidChangeScreenProfileNotification), NSC(NSWindowDidDeminiaturizeNotification),
	NSC(NSWindowDidEnterFullScreenNotification), NSC(NSWindowDidExitFullScreenNotification),
	NSC(NSWindowDidExposeNotification), NSC(NSWindowDidMiniaturizeNotification),
	NSC(NSWindowDidMoveNotification), NSC(NSWindowDidResignKeyNotification),
	NSC(NSWindowDidResizeNotification), NSC(NSWindowWillCloseNotification),
	NSC(NSWindowWillEnterFullScreenNotification), NSC(NSWindowWillExitFullScreenNotification),
	D("_NSAppKitVersionNumber", 8, init_appkit_version),
	D("_NSZeroPoint", 8, init_zero16), D("_NSZeroSize", 8, init_zero16), D("_NSZeroRect", 16, init_zero16),
};

// an Objective-C exception nobody catches: say where it came from
static void uncaught(NSException *e)
{
	fprintf(stderr, "[mac32] uncaught Objective-C exception %s: %s\n", e.name.UTF8String, e.reason.UTF8String);
	for (NSString *l in [NSThread callStackSymbols]) fprintf(stderr, "  %s\n", l.UTF8String);
	extern void print_recent_calls(void);
	print_recent_calls();
}

// every Objective-C throw passes here first, on any thread: log the ones a
// bridged object causes (unrecognized selectors) with the stack that threw
static objc_exception_preprocessor prev_pre;
static id preprocess(id exc)
{
	NSException *e = exc;
	if ([e isKindOfClass:[NSException class]] && [e.reason containsString:@"unrecognized selector"]) {
		fprintf(stderr, "[mac32] Objective-C exception: %s\n", e.reason.UTF8String);
		for (NSString *l in [NSThread callStackSymbols]) fprintf(stderr, "  %s\n", l.UTF8String);
	}
	return prev_pre ? prev_pre(exc) : exc;
}

void thunks_objc_init(void)
{
	REGISTER(objc_fns);
	NSSetUncaughtExceptionHandler(uncaught);
	prev_pre = objc_setExceptionPreprocessor(preprocess);
}
