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
#include <pthread.h>
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
		if (!hc) die("objc: %s's superclass %s does not exist on this macOS", gc->name, gc->host_super);
		U32(gc->cls + C_SUPER) = objc_wrap(hc);
		U32(gc->meta + C_SUPER) = objc_wrap(object_getClass(hc));
	}
	// metaclass isa (root meta) is not used by our dispatch
}

struct catdef { const char *cls; g32 im, cm; };

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
					read_methods(U32(c + 8), &gc->im, &gc->nim);
					read_methods(U32(c + 12), &gc->cm, &gc->ncm);
				} else {
					LOG("objc: category on host class %s not supported yet\n", cn);
				}
			}
		}
	}
	for (int i = 0; i < ngclasses; i++) link_class(gclasses[i]);
	if (macho_section(img, "__OBJC", "__cls_refs", &a, &sz))
		for (uint32_t i = 0; i < sz; i += 4) {
			const char *n = G2H(U32(a + i));
			struct gclass *gc = gclass_named(n);
			if (gc) U32(a + i) = gc->cls;
			else {
				Class hc = objc_getClass(n);
				if (!hc) LOG("objc: class %s not found\n", n);
				U32(a + i) = hc ? objc_wrap(hc) : 0;
			}
		}
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
	if (super_cls) [inv invokeUsingIMP:object_isClass(target) ? method_getImplementation(class_getClassMethod(super_cls, hsel))
								   : class_getMethodImplementation(super_cls, hsel)];
	else [inv invoke];
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

static void msgsend(uint32_t *a, struct hres *r, int kind)
{
	g32 sret = 0;
	if (kind == K_STRET || kind == K_SUPER_STRET) { sret = *a++; r->flags |= F_POP4; }
	g32 self, gsel = a[1], supcls = 0;
	if (kind == K_SUPER || kind == K_SUPER_STRET) {
		self = U32(a[0]);
		supcls = U32(a[0] + 4);
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
		if (*p >= '0' && *p <= '9') continue;
		if (!strncmp(p, "{_NSRect=", 9)) { [s appendString:@"{CGRect={CGPoint=dd}{CGSize=dd}}"]; p = strstr(p, "}}") + 1; continue; }
		if (!strncmp(p, "{_NSPoint=ff}", 13)) { [s appendString:@"{CGPoint=dd}"]; p += 12; continue; }
		if (!strncmp(p, "{_NSSize=ff}", 12)) { [s appendString:@"{CGSize=dd}"]; p += 11; continue; }
		char c = *p == 'l' ? 'i' : *p == 'L' ? 'I' : *p;
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

static void install(Class cls, Class lookup, struct gmeth *m, int n, int meta)
{
	for (int i = 0; i < n; i++) {
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
static void t_enum_mutation(uint32_t *args, struct hres *r) { (void)args; (void)r; die("objc: collection mutated during enumeration"); }
static void init_nsapp(g32 a) { nsapp_var = a; U32(a) = objc_wrap(NSApp); }
static void init_runloopmode(g32 a) { U32(a) = objc_wrap(NSDefaultRunLoopMode); }

static struct hfn objc_fns[] = {
	R("_objc_msgSend", t_msgSend), R("_objc_msgSend_stret", t_msgSend_stret),
	R("_objc_msgSend_fpret", t_msgSend_fpret), R("_objc_msgSendSuper", t_msgSendSuper),
	R("_objc_msgSendSuper_stret", t_msgSendSuper_stret),
	R("_objc_enumerationMutation", t_enum_mutation),
	R("_NSClassFromString", t_NSClassFromString), R("_NSLog", t_NSLog),
	D("_NSApp", 4, init_nsapp), D("_NSDefaultRunLoopMode", 4, init_runloopmode),
	D("___CFConstantStringClassReference", 32, init_cfconst),
};

void thunks_objc_init(void) { REGISTER(objc_fns); }
