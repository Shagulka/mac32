// QuickDraw, as much as old SDL 1.2 (Quartz driver, up to 1.2.13) needs to draw
// a window: 64-bit macOS has no QuickDraw at all.
//
// SDL makes an NSQuickDrawView, asks it for its qdPort, writes pixels straight
// into the port's pixel map and calls QDFlushPortBuffer to show them. Here
// NSQuickDrawView is a plain NSView whose port is a small record in guest
// memory: a PixMap whose pixels are a guest buffer the size of the whole
// window (SDL offsets into it past the title bar, as QuickDraw window ports
// worked). A flush copies the view's part of the buffer into its layer.
//
// Regions are kept as their bounding box only, and handles are a master
// pointer to a guest block: enough for the dirty-rectangle bookkeeping
// around the flush.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include "thunks.h"
#include <pthread.h>
#include <sys/mman.h>

// ---- ports --------------------------------------------------------------------------------
// guest port record: +0 'qdpt', +4 master pointer (= PixMapHandle) -> +8 PixMap
enum { P_MAGIC = 0, P_MASTER = 4, P_PIXMAP = 8, P_SIZE = 64 };
// PixMap (68k alignment): baseAddr 0, rowBytes 4, bounds 6, pmVersion 14, packType 16,
// packSize 18, hRes 22, vRes 26, pixelType 30, pixelSize 32, cmpCount 34, cmpSize 36,
// pixelFormat 38, pmTable 42, pmExt 46
#define U32AT(a) (*(uint32_t *)G2H(a))
#define U16AT(a) (*(uint16_t *)G2H(a))

struct port { g32 g; NSView *view; g32 buf; int w, h, rb; struct port *next; };
static struct port *ports;
static pthread_mutex_t plk = PTHREAD_MUTEX_INITIALIZER;

static struct port *port_of(g32 g)
{
	pthread_mutex_lock(&plk);
	struct port *p = ports;
	while (p && p->g != g) p = p->next;
	pthread_mutex_unlock(&plk);
	return p;
}

static void put_rect(g32 r, int top, int left, int bottom, int right)
{
	U16AT(r) = (uint16_t)top;
	U16AT(r + 2) = (uint16_t)left;
	U16AT(r + 4) = (uint16_t)bottom;
	U16AT(r + 6) = (uint16_t)right;
}

// size the pixel buffer to the view's window (QuickDraw window ports covered
// the whole window, title bar included)
static void port_fit(struct port *p)
{
	__block NSSize s;
	void (^measure)(void) = ^{
		NSWindow *w = p->view.window;
		s = w ? w.frame.size : p->view.frame.size;
	};
	if ([NSThread isMainThread]) measure();
	else dispatch_sync(dispatch_get_main_queue(), measure);
	int w = (int)s.width, h = (int)s.height;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (p->buf && w == p->w && h == p->h) return;
	gfree(p->buf);
	p->w = w;
	p->h = h;
	p->rb = (w * 4 + 15) & ~15;
	p->buf = gcalloc(1, (uint32_t)(p->rb * h));
	g32 pm = p->g + P_PIXMAP;
	U32AT(pm) = p->buf;
	U16AT(pm + 4) = (uint16_t)(0x8000 | (p->rb & 0x3fff));
	put_rect(pm + 6, 0, 0, h, w);
	U32AT(pm + 22) = U32AT(pm + 26) = 72 << 16;
	U16AT(pm + 30) = 16;			// RGBDirect
	U16AT(pm + 32) = 32;
	U16AT(pm + 34) = 3;
	U16AT(pm + 36) = 8;
	U32AT(pm + 38) = 'BGRA';		// SDL's masks 0xff0000/0xff00/0xff on little-endian
}

static void port_show(struct port *p)
{
	NSView *v = p->view;
	if (!p->buf || !v) return;
	__block NSRect f;
	__block CGFloat wh;
	void (^measure)(void) = ^{ f = v.frame; wh = v.window ? v.window.frame.size.height : f.size.height; };
	if ([NSThread isMainThread]) measure();
	else dispatch_sync(dispatch_get_main_queue(), measure);
	// the view's pixels start where SDL put them: below the title bar
	int x = (int)f.origin.x, y = (int)(wh - f.size.height - f.origin.y);
	int w = (int)f.size.width, h = (int)f.size.height;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x + w > p->w) w = p->w - x;
	if (y + h > p->h) h = p->h - y;
	if (w <= 0 || h <= 0) return;
	CFMutableDataRef d = CFDataCreateMutable(NULL, (CFIndex)w * 4 * h);
	CFDataSetLength(d, (CFIndex)w * 4 * h);
	uint8_t *dst = CFDataGetMutableBytePtr(d);
	const uint8_t *src = G2H(p->buf);
	for (int row = 0; row < h; row++) memcpy(dst + (size_t)row * w * 4, src + (size_t)(y + row) * p->rb + x * 4, (size_t)w * 4);
	CGDataProviderRef dp = CGDataProviderCreateWithCFData(d);
	CFRelease(d);
	CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
	CGImageRef img = CGImageCreate(w, h, 8, 32, w * 4, cs,
				       kCGBitmapByteOrder32Little | (CGBitmapInfo)kCGImageAlphaNoneSkipFirst, dp, NULL, false,
				       kCGRenderingIntentDefault);
	CGColorSpaceRelease(cs);
	CGDataProviderRelease(dp);
	void (^show)(void) = ^{
		v.wantsLayer = YES;
		[CATransaction begin];
		[CATransaction setDisableActions:YES];
		v.layer.contents = (__bridge id)img;
		v.layer.magnificationFilter = kCAFilterNearest;
		[CATransaction commit];
		CGImageRelease(img);
	};
	if ([NSThread isMainThread]) show();
	else dispatch_async(dispatch_get_main_queue(), show);
}

@interface NSQuickDrawView : NSView
- (uint32_t)qdPort;
@end

@implementation NSQuickDrawView
- (BOOL)isOpaque { return YES; }
- (uint32_t)qdPort
{
	pthread_mutex_lock(&plk);
	struct port *p = ports;
	while (p && p->view != self) p = p->next;
	if (!p) {
		p = calloc(1, sizeof *p);
		p->view = self;
		p->g = gcalloc(1, P_SIZE);
		U32AT(p->g + P_MAGIC) = 'qdpt';
		U32AT(p->g + P_MASTER) = p->g + P_PIXMAP;
		p->next = ports;
		ports = p;
	}
	pthread_mutex_unlock(&plk);
	if (!p->buf) port_fit(p);
	return p->g;
}
- (void)dealloc
{
	pthread_mutex_lock(&plk);
	for (struct port **q = &ports; *q; q = &(*q)->next)
		if ((*q)->view == self) {
			struct port *p = *q;
			*q = p->next;
			// the guest may still hold the port: keep its record, drop the pixels
			gfree(p->buf);
			U32AT(p->g + P_PIXMAP) = 0;
			free(p);
			break;
		}
	pthread_mutex_unlock(&plk);
	[super dealloc];
}
@end

static void t_LockPortBits(uint32_t *args, struct hres *r)
{
	struct port *p = port_of(A(0));
	if (p) port_fit(p);
	RET(0);
}
static void t_UnlockPortBits(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_GetPortPixMap(uint32_t *args, struct hres *r) { RET(A(0) ? A(0) + P_MASTER : 0); }
static g32 pixmap(g32 h) { return h ? U32AT(h) : 0; }
static void t_GetPixBaseAddr(uint32_t *args, struct hres *r) { g32 pm = pixmap(A(0)); RET(pm ? U32AT(pm) : 0); }
static void t_GetPixRowBytes(uint32_t *args, struct hres *r) { g32 pm = pixmap(A(0)); RET(pm ? U16AT(pm + 4) & 0x3fff : 0); }
static void t_GetPixDepth(uint32_t *args, struct hres *r) { g32 pm = pixmap(A(0)); RET(pm ? U16AT(pm + 32) : 0); }
static void t_GetPortBounds(uint32_t *args, struct hres *r)
{
	struct port *p = port_of(A(0));
	if (A(1)) put_rect(A(1), 0, 0, p ? p->h : 0, p ? p->w : 0);
	RET(A(1));
}
static void t_QDFlushPortBuffer(uint32_t *args, struct hres *r)
{
	(void)r;
	struct port *p = port_of(A(0));
	if (p) port_show(p);
}
static void t_true(uint32_t *args, struct hres *r) { (void)args; RET(1); }

// ---- regions: bounding box only -----------------------------------------------------------
// RgnHandle -> master pointer -> {short rgnSize; Rect rgnBBox}
static g32 rgn_new(void)
{
	g32 h = gmalloc(4);
	g32 b = gcalloc(1, 10);
	U16AT(b) = 10;
	U32AT(h) = b;
	return h;
}
static g32 rgn_box(g32 h) { return h && U32AT(h) ? U32AT(h) + 2 : 0; }
static int16_t S16(g32 a) { return (int16_t)U16AT(a); }
static int rect_empty(g32 r) { return S16(r + 4) <= S16(r) || S16(r + 6) <= S16(r + 2); }

static void t_NewRgn(uint32_t *args, struct hres *r) { (void)args; RET(rgn_new()); }
static void t_DisposeRgn(uint32_t *args, struct hres *r)
{
	(void)r;
	if (A(0)) { gfree(U32AT(A(0))); gfree(A(0)); }
}
static void t_SetEmptyRgn(uint32_t *args, struct hres *r) { (void)r; g32 b = rgn_box(A(0)); if (b) put_rect(b, 0, 0, 0, 0); }
static void t_SetRectRgn(uint32_t *args, struct hres *r)
{
	// (rgn, left, top, right, bottom), shorts in 4-byte slots
	(void)r;
	g32 b = rgn_box(A(0));
	if (b) put_rect(b, (int16_t)A(2), (int16_t)A(1), (int16_t)A(4), (int16_t)A(3));
	if (b && rect_empty(b)) put_rect(b, 0, 0, 0, 0);
}
static void t_RectRgn(uint32_t *args, struct hres *r)
{
	(void)r;
	g32 b = rgn_box(A(0));
	if (b && A(1)) memcpy(G2H(b), G2H(A(1)), 8);
}
static void t_UnionRgn(uint32_t *args, struct hres *r)
{
	// (a, b, dst)
	(void)r;
	g32 a = rgn_box(A(0)), b = rgn_box(A(1)), d = rgn_box(A(2));
	if (!a || !b || !d) return;
	if (rect_empty(a)) { memmove(G2H(d), G2H(b), 8); return; }
	if (rect_empty(b)) { memmove(G2H(d), G2H(a), 8); return; }
	int top = MIN(S16(a), S16(b)), left = MIN(S16(a + 2), S16(b + 2));
	int bottom = MAX(S16(a + 4), S16(b + 4)), right = MAX(S16(a + 6), S16(b + 6));
	put_rect(d, top, left, bottom, right);
}
static void t_RectInRgn(uint32_t *args, struct hres *r)
{
	// (const Rect *, rgn): do they overlap?
	g32 a = A(0), b = rgn_box(A(1));
	if (!a || !b || rect_empty(a) || rect_empty(b)) { RET(0); return; }
	RET(S16(a) < S16(b + 4) && S16(b) < S16(a + 4) && S16(a + 2) < S16(b + 6) && S16(b + 2) < S16(a + 6));
}
static void t_EmptyRgn(uint32_t *args, struct hres *r) { g32 b = rgn_box(A(0)); RET(!b || rect_empty(b)); }
static void t_GetRegionBounds(uint32_t *args, struct hres *r)
{
	g32 b = rgn_box(A(0));
	if (A(1)) { if (b) memcpy(G2H(A(1)), G2H(b), 8); else put_rect(A(1), 0, 0, 0, 0); }
	RET(A(1));
}
static void t_SetRect(uint32_t *args, struct hres *r)
{
	// (Rect *, left, top, right, bottom)
	(void)r;
	if (A(0)) put_rect(A(0), (int16_t)A(2), (int16_t)A(1), (int16_t)A(4), (int16_t)A(3));
}

// ---- Memory Manager handles: master pointer -> guest block ---------------------------------
static void t_NewHandle(uint32_t *args, struct hres *r)
{
	g32 h = gmalloc(4);
	U32AT(h) = gcalloc(1, A(0) ? A(0) : 1);
	RET(h);
}
static void t_DisposeHandle(uint32_t *args, struct hres *r)
{
	(void)r;
	if (A(0)) { gfree(U32AT(A(0))); gfree(A(0)); }
}
static void t_GetHandleSize(uint32_t *args, struct hres *r) { RET(A(0) && U32AT(A(0)) ? low_block_len(U32AT(A(0))) : 0); }

// ---- Foundation's page allocator: zeroed, page-aligned guest memory --------------------------
static void t_NSAllocateMemoryPages(uint32_t *args, struct hres *r)
{
	g32 p = low_pages(A(0) ? A(0) : 1, PROT_READ | PROT_WRITE);
	if (p) memset(G2H(p), 0, A(0));
	RET(p);
}
static void t_NSDeallocateMemoryPages(uint32_t *args, struct hres *r) { (void)r; if (A(0)) low_free(A(0)); }
static void t_NSCopyMemoryPages(uint32_t *args, struct hres *r) { (void)r; memmove(G2H(A(1)), G2H(A(0)), A(2)); }

// ---- Mach VM: zeroed pages below 4 GB (the task argument is always this task) ---------------
static void t_vm_allocate(uint32_t *args, struct hres *r)
{
	// (task, vm_address_t *addr, size, flags); VM_FLAGS_FIXED asks for *addr exactly
	if (!(A(3) & 1)) { RET(3); return; }		// KERN_NO_SPACE: only VM_FLAGS_ANYWHERE
	g32 p = low_pages(A(2) ? A(2) : 1, PROT_READ | PROT_WRITE);
	if (!p) { RET(3); return; }
	memset(G2H(p), 0, A(2));
	*(g32 *)G2H(A(1)) = p;
	RET(0);
}
static void t_vm_deallocate(uint32_t *args, struct hres *r) { if (A(1)) low_free(A(1)); RET(0); }

static struct hfn qd[] = {
	R("_LockPortBits", t_LockPortBits), R("_UnlockPortBits", t_UnlockPortBits),
	R("_GetPortPixMap", t_GetPortPixMap), R("_GetPixBaseAddr", t_GetPixBaseAddr),
	R("_GetPixRowBytes", t_GetPixRowBytes), R("_GetPixDepth", t_GetPixDepth),
	R("_GetPortBounds", t_GetPortBounds), R("_QDFlushPortBuffer", t_QDFlushPortBuffer),
	R("_QDIsPortBuffered", t_true),
	R("_NewRgn", t_NewRgn), R("_DisposeRgn", t_DisposeRgn), R("_SetEmptyRgn", t_SetEmptyRgn),
	R("_SetRectRgn", t_SetRectRgn), R("_MacSetRectRgn", t_SetRectRgn), R("_RectRgn", t_RectRgn),
	R("_UnionRgn", t_UnionRgn), R("_MacUnionRgn", t_UnionRgn), R("_RectInRgn", t_RectInRgn),
	R("_EmptyRgn", t_EmptyRgn), R("_GetRegionBounds", t_GetRegionBounds), R("_SetRect", t_SetRect),
	R("_NewHandle", t_NewHandle), R("_NewHandleClear", t_NewHandle), R("_DisposeHandle", t_DisposeHandle),
	R("_NSAllocateMemoryPages", t_NSAllocateMemoryPages), R("_NSDeallocateMemoryPages", t_NSDeallocateMemoryPages),
	R("_NSCopyMemoryPages", t_NSCopyMemoryPages),
	R("_vm_allocate", t_vm_allocate), R("_vm_deallocate", t_vm_deallocate),
	R("_HLock", t_UnlockPortBits), R("_HUnlock", t_UnlockPortBits), R("_GetHandleSize", t_GetHandleSize),
};

void thunks_qd_init(void) { REGISTER(qd); }
