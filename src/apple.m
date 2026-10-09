// CoreFoundation, CoreGraphics, IOKit, Carbon and CGL for the guest.
// Opaque objects travel as handles ('o' in a signature, see objc.m).
#import <Foundation/Foundation.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <IOKit/IOKitLib.h>
#import <OpenGL/OpenGL.h>
#import <ImageIO/ImageIO.h>
#import <CFNetwork/CFNetwork.h>
#import <SystemConfiguration/SystemConfiguration.h>
#include "thunks.h"

// Boolean / OSErr results: the host leaves junk above the low byte/half
static int w_CFBooleanGetValue(CFBooleanRef b) { return CFBooleanGetValue(b) ? 1 : 0; }
static int w_Gestalt(uint32_t sel, int32_t *resp) { SInt32 v = 0; OSErr e = Gestalt(sel, &v); if (resp) *resp = v; return e; }
static int w_SetFrontProcess(const ProcessSerialNumber *p) { return SetFrontProcess(p); }
static int w_TransformProcessType(const ProcessSerialNumber *p, uint32_t t) { return TransformProcessType(p, t); }
static int w_SetSystemUIMode(uint32_t m, uint32_t o) { return SetSystemUIMode(m, o); }
static int w_CGWarpMouseCursorPosition(float x, float y) { return CGWarpMouseCursorPosition(CGPointMake(x, y)); }
static int32_t w_CFArrayGetCount(CFArrayRef a) { return (int32_t)CFArrayGetCount(a); }
static const void *w_CFArrayGetValueAtIndex(CFArrayRef a, int32_t i) { return CFArrayGetValueAtIndex(a, i); }
static int32_t w_CFDataGetLength(CFDataRef d) { return (int32_t)CFDataGetLength(d); }
static int32_t w_CFStringGetLength(CFStringRef s) { return (int32_t)CFStringGetLength(s); }
static int w_CFStringGetCString(CFStringRef s, char *b, int32_t n, uint32_t e) { return CFStringGetCString(s, b, n, e) ? 1 : 0; }
static uint32_t w_CGDisplayPixelsWide(uint32_t d) { return (uint32_t)CGDisplayPixelsWide(d); }
static uint32_t w_CGDisplayPixelsHigh(uint32_t d) { return (uint32_t)CGDisplayPixelsHigh(d); }
static int w_CGCursorIsVisible(void) { return CGCursorIsVisible() ? 1 : 0; }
static uint32_t w_CGDisplayModeGetWidth(CGDisplayModeRef m) { return (uint32_t)CGDisplayModeGetWidth(m); }
static uint32_t w_CGDisplayModeGetHeight(CGDisplayModeRef m) { return (uint32_t)CGDisplayModeGetHeight(m); }
static int w_CGDisplayIsActive(uint32_t d) { return CGDisplayIsActive(d) ? 1 : 0; }

// the guest gets its own copy of CFData bytes (host memory is above 4 GB)
static void t_CFDataGetBytePtr(uint32_t *args, struct hres *r)
{
	CFDataRef d = hobj_unwrap(A(0));
	CFIndex n = CFDataGetLength(d);
	g32 g = gmalloc((uint32_t)n + 1);	// leaks with the CFData; fine for config blobs
	memcpy(G2H(g), CFDataGetBytePtr(d), (size_t)n);
	RET(g);
}

// i386 long/CFIndex/NSInteger are 32 bits and CGFloat is a float
static void t_CFNumberGetValue(uint32_t *args, struct hres *r)
{
	CFNumberType t = (CFNumberType)A(1);
	if (t == kCFNumberLongType || t == kCFNumberCFIndexType || t == kCFNumberNSIntegerType) t = kCFNumberSInt32Type;
	if (t == kCFNumberCGFloatType) t = kCFNumberFloat32Type;
	RET(CFNumberGetValue(hobj_unwrap(A(0)), t, G2H(A(2))) ? 1 : 0);
}
static void t_CFNumberCreate(uint32_t *args, struct hres *r)
{
	CFNumberType t = (CFNumberType)A(1);
	if (t == kCFNumberLongType || t == kCFNumberCFIndexType || t == kCFNumberNSIntegerType) t = kCFNumberSInt32Type;
	if (t == kCFNumberCGFloatType) t = kCFNumberFloat32Type;
	RET(hobj_wrap((void *)CFNumberCreate(NULL, t, G2H(A(2)))));
}

static void t_CFUserNotificationDisplayAlert(uint32_t *args, struct hres *r)
{
	// timeout(double) flags icon sound loc header message default alt other *response
	NSString *h = hobj_unwrap(A(7)), *m = hobj_unwrap(A(8));
	fprintf(stderr, "[mac32] alert: %s\n  %s\n", h.UTF8String ?: "", m.UTF8String ?: "");
	if (A(12)) *(uint32_t *)G2H(A(12)) = kCFUserNotificationDefaultResponse;
	RET(0);
}

// CGL: object out-parameters become handles
static void t_CGLChoosePixelFormat(uint32_t *args, struct hres *r)
{
	CGLPixelFormatObj pix = NULL;
	GLint n = 0;
	CGLError e = CGLChoosePixelFormat(G2H(A(0)), &pix, &n);
	*(g32 *)G2H(A(1)) = hobj_wrap(pix);
	if (A(2)) *(int32_t *)G2H(A(2)) = n;
	RET(e);
}
static void t_CGLCreateContext(uint32_t *args, struct hres *r)
{
	CGLContextObj c = NULL;
	CGLError e = CGLCreateContext(hobj_unwrap(A(0)), hobj_unwrap(A(1)), &c);
	*(g32 *)G2H(A(2)) = hobj_wrap(c);
	RET(e);
}
static void t_CGLQueryRendererInfo(uint32_t *args, struct hres *r)
{
	CGLRendererInfoObj ri = NULL;
	GLint n = 0;
	CGLError e = CGLQueryRendererInfo(A(0), &ri, &n);
	*(g32 *)G2H(A(1)) = hobj_wrap(ri);
	if (A(2)) *(int32_t *)G2H(A(2)) = n;
	RET(e);
}

// CGDisplayAvailableModes/CurrentMode return dictionaries keyed by
// kCGDisplayWidth etc. If this macOS no longer builds them, make our own.
static CFDictionaryRef mode_dict(CGDisplayModeRef m)
{
	NSMutableDictionary *d = [NSMutableDictionary dictionary];
	d[@"Width"] = @(CGDisplayModeGetWidth(m));
	d[@"Height"] = @(CGDisplayModeGetHeight(m));
	d[@"BitsPerPixel"] = @32;
	d[@"BitsPerSample"] = @8;
	d[@"SamplesPerPixel"] = @3;
	d[@"RefreshRate"] = @(CGDisplayModeGetRefreshRate(m) ?: 60);
	d[@"Mode"] = @(CGDisplayModeGetIODisplayModeID(m));
	d[@"IOFlags"] = @(CGDisplayModeGetIOFlags(m));
	d[@"UsableForDesktopGUI"] = @(CGDisplayModeIsUsableForDesktopGUI(m));
	return (CFDictionaryRef)CFBridgingRetain(d);
}
static void t_CGDisplayAvailableModes(uint32_t *args, struct hres *r)
{
	CFArrayRef a = CGDisplayAvailableModes(A(0));
	if (!a) {
		NSMutableArray *out = [NSMutableArray array];
		CFArrayRef ms = CGDisplayCopyAllDisplayModes(A(0), NULL);
		for (CFIndex i = 0; ms && i < CFArrayGetCount(ms); i++)
			[out addObject:(id)mode_dict((CGDisplayModeRef)CFArrayGetValueAtIndex(ms, i))];
		if (ms) CFRelease(ms);
		a = (CFArrayRef)CFBridgingRetain(out);
	}
	RET(hobj_wrap((void *)a));
}
static void t_CGDisplayCurrentMode(uint32_t *args, struct hres *r)
{
	CFDictionaryRef d = CGDisplayCurrentMode(A(0));
	if (!d) {
		CGDisplayModeRef m = CGDisplayCopyDisplayMode(A(0));
		d = mode_dict(m);
		CGDisplayModeRelease(m);
	}
	RET(hobj_wrap((void *)d));
}

static void init_zero(g32 a) { *(uint32_t *)G2H(a) = 0; }

const char *describe_obj(g32 g);

// CFArrayApplyFunction(array, CFRange{loc,len}, applier, context): call the 32-bit applier
static void t_CFArrayApplyFunction(uint32_t *args, struct hres *r)
{
	(void)r;
	CFArrayRef a = hobj_unwrap(A(0));
	for (uint32_t i = A(1); i < A(1) + A(2); i++)
		gcall(A(3), 2, hobj_wrap((void *)CFArrayGetValueAtIndex(a, i)), A(4));
}
// CFUUIDBytes is 16 bytes: passed by value (4 words), returned through a hidden pointer
static void t_CFUUIDGetConstantUUIDWithBytes(uint32_t *args, struct hres *r)
{
	uint8_t *b = (uint8_t *)&args[1];
	RET(hobj_wrap((void *)CFUUIDGetConstantUUIDWithBytes(NULL, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
							       b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15])));
}
static void t_CFUUIDGetUUIDBytes(uint32_t *args, struct hres *r)
{
	CFUUIDBytes u = CFUUIDGetUUIDBytes(hobj_unwrap(A(1)));
	memcpy(G2H(A(0)), &u, 16);
	r->rax = A(0);
	r->flags |= F_POP4;
}
static void t_IORegistryEntryCreateCFProperties(uint32_t *args, struct hres *r)
{
	CFMutableDictionaryRef d = NULL;
	kern_return_t k = IORegistryEntryCreateCFProperties(A(0), &d, NULL, A(3));
	*(g32 *)G2H(A(1)) = hobj_wrap(d);
	RET(k);
}
static uint32_t w_CFGetTypeID(CFTypeRef o) { return (uint32_t)CFGetTypeID(o); }
static uint32_t w_CFArrayGetTypeID(void) { return (uint32_t)CFArrayGetTypeID(); }
static uint32_t w_CFDictionaryGetTypeID(void) { return (uint32_t)CFDictionaryGetTypeID(); }
static uint32_t w_CFStringGetTypeID(void) { return (uint32_t)CFStringGetTypeID(); }
static uint32_t w_CFNumberGetTypeID(void) { return (uint32_t)CFNumberGetTypeID(); }
static uint32_t w_CFDataGetTypeID(void) { return (uint32_t)CFDataGetTypeID(); }
static void t_GetGlobalMouse(uint32_t *args, struct hres *r)
{
	(void)r;
	CGEventRef e = CGEventCreate(NULL);
	CGPoint p = CGEventGetLocation(e);
	CFRelease(e);
	int16_t *pt = G2H(A(0));	// Point {v, h}
	pt[0] = (int16_t)p.y;
	pt[1] = (int16_t)p.x;
}
static void t_io_unsupported(uint32_t *args, struct hres *r) { (void)args; RET(0xe00002c7); }	// kIOReturnUnsupported
static void t_ff_none(uint32_t *args, struct hres *r) { (void)args; RET(0x80040001); }		// FFERR_NOINTERFACE
static int w_CFEqual(CFTypeRef a, CFTypeRef b) { return CFEqual(a, b) ? 1 : 0; }
static CFURLRef w_CFURLCreateWithBytes(CFAllocatorRef a, const UInt8 *b, int32_t n, uint32_t enc, CFURLRef base)
{ return CFURLCreateWithBytes(a, b, n, enc, base); }
static void t_CFStringCreateWithFormat(uint32_t *args, struct hres *r)
{
	// alloc, formatOptions, format, ...: %@ handled by our formatter
	NSString *f = hobj_unwrap(A(2));
	int n;
	char *s = gvformat_objc(f.UTF8String, &args[3], &n, describe_obj);
	RET(hobj_wrap((void *)CFStringCreateWithCString(NULL, s, kCFStringEncodingUTF8)));
	free(s);
}
// CGRect return: four floats through the hidden pointer on i386
static void t_CGDisplayBounds(uint32_t *args, struct hres *r)
{
	CGRect b = CGDisplayBounds(A(1));
	float *g = G2H(A(0));
	g[0] = (float)b.origin.x; g[1] = (float)b.origin.y; g[2] = (float)b.size.width; g[3] = (float)b.size.height;
	r->rax = A(0);
	r->flags |= F_POP4;
}
static CFArrayRef w_CFNetworkCopyProxiesForURL(CFURLRef u, CFDictionaryRef s) { return CFNetworkCopyProxiesForURL(u, s); }
static CFArrayRef w_CFNetworkCopyProxiesForAutoConfigurationScript(CFStringRef s, CFURLRef u, uint32_t err)
{
	if (err) *(uint32_t *)G2H(err) = 0;
	return CFNetworkCopyProxiesForAutoConfigurationScript(s, u, NULL);
}
#define KEY(sym) static void init_##sym(g32 a) { *(uint32_t *)G2H(a) = hobj_wrap((void *)sym); }
KEY(kCFProxyAutoConfigurationURLKey) KEY(kCFProxyHostNameKey) KEY(kCFProxyPasswordKey)
KEY(kCFProxyPortNumberKey) KEY(kCFProxyTypeAutoConfigurationURL) KEY(kCFProxyTypeHTTP)
KEY(kCFProxyTypeKey) KEY(kCFProxyTypeNone) KEY(kCFProxyUsernameKey)
#define DKEY(sym) D("_" #sym, 4, init_##sym)

static struct hfn apple[] = {
	// CoreFoundation
	G("_CFArrayGetCount", w_CFArrayGetCount, "i:o"),
	G("_CFArrayGetValueAtIndex", w_CFArrayGetValueAtIndex, "o:oi"),
	G("_CFBooleanGetValue", w_CFBooleanGetValue, "i:o"),
	R("_CFDataGetBytePtr", t_CFDataGetBytePtr),
	G("_CFDataGetLength", w_CFDataGetLength, "i:o"),
	G("_CFDictionaryGetValue", CFDictionaryGetValue, "o:oo"),
	R("_CFNumberGetValue", t_CFNumberGetValue), R("_CFNumberCreate", t_CFNumberCreate),
	G("_CFRelease", CFRelease, "v:o"), G("_CFRetain", CFRetain, "o:o"),
	G("_CFStringCreateWithCString", CFStringCreateWithCString, "o:opu"),
	G("_CFStringGetSystemEncoding", CFStringGetSystemEncoding, "u:"),
	G("_CFStringGetLength", w_CFStringGetLength, "i:o"),
	G("_CFStringGetCString", w_CFStringGetCString, "i:opiu"),
	G("_CFURLCreateWithFileSystemPath", CFURLCreateWithFileSystemPath, "o:ooii"),
	R("_CFUserNotificationDisplayAlert", t_CFUserNotificationDisplayAlert),
	D("_kCFAllocatorDefault", 4, init_zero),
	G("_CFEqual", w_CFEqual, "i:oo"),
	R("_CFStringCreateWithFormat", t_CFStringCreateWithFormat),
	G("_CFURLCreateWithBytes", w_CFURLCreateWithBytes, "o:opiuo"),
	R("_CGDisplayBounds", t_CGDisplayBounds),
	R("_CFArrayApplyFunction", t_CFArrayApplyFunction),
	R("_CFUUIDGetConstantUUIDWithBytes", t_CFUUIDGetConstantUUIDWithBytes),
	R("_CFUUIDGetUUIDBytes", t_CFUUIDGetUUIDBytes),
	G("_CFGetTypeID", w_CFGetTypeID, "u:o"), G("_CFArrayGetTypeID", w_CFArrayGetTypeID, "u:"),
	G("_CFDictionaryGetTypeID", w_CFDictionaryGetTypeID, "u:"), G("_CFStringGetTypeID", w_CFStringGetTypeID, "u:"),
	G("_CFNumberGetTypeID", w_CFNumberGetTypeID, "u:"), G("_CFDataGetTypeID", w_CFDataGetTypeID, "u:"),
	G("_IOMasterPort", IOMasterPort, "i:up"),
	R("_IORegistryEntryCreateCFProperties", t_IORegistryEntryCreateCFProperties),
	R("_IOCreatePlugInInterfaceForService", t_io_unsupported),
	R("_FFIsForceFeedback", t_ff_none), R("_FFCreateDevice", t_ff_none),
	R("_FFReleaseDevice", t_ff_none), R("_FFDeviceEscape", t_ff_none),
	R("_GetGlobalMouse", t_GetGlobalMouse),
	G("_CGGetActiveDisplayList", CGGetActiveDisplayList, "i:upp"),
	G("_SCDynamicStoreCopyProxies", SCDynamicStoreCopyProxies, "o:o"),
	G("_CFNetworkCopyProxiesForURL", w_CFNetworkCopyProxiesForURL, "o:oo"),
	G("_CFNetworkCopyProxiesForAutoConfigurationScript", w_CFNetworkCopyProxiesForAutoConfigurationScript, "o:oop"),
	DKEY(kCFProxyAutoConfigurationURLKey), DKEY(kCFProxyHostNameKey), DKEY(kCFProxyPasswordKey),
	DKEY(kCFProxyPortNumberKey), DKEY(kCFProxyTypeAutoConfigurationURL), DKEY(kCFProxyTypeHTTP),
	DKEY(kCFProxyTypeKey), DKEY(kCFProxyTypeNone), DKEY(kCFProxyUsernameKey),
	// CoreGraphics
	G("_CGGetLastMouseDelta", CGGetLastMouseDelta, "v:pp"),
	R("_CGDisplayAvailableModes", t_CGDisplayAvailableModes),
	R("_CGDisplayCurrentMode", t_CGDisplayCurrentMode),
	G("_CGDisplayCopyDisplayMode", CGDisplayCopyDisplayMode, "o:u"),
	G("_CGDisplayCopyAllDisplayModes", CGDisplayCopyAllDisplayModes, "o:uo"),
	G("_CGDisplayModeGetWidth", w_CGDisplayModeGetWidth, "u:o"),
	G("_CGDisplayModeGetHeight", w_CGDisplayModeGetHeight, "u:o"),
	G("_CGDisplayModeGetRefreshRate", CGDisplayModeGetRefreshRate, "d:o"),
	G("_CGDisplayModeGetIODisplayModeID", CGDisplayModeGetIODisplayModeID, "i:o"),
	G("_CGDisplayModeRelease", CGDisplayModeRelease, "v:o"),
	G("_CGDisplayModeRetain", CGDisplayModeRetain, "o:o"),
	G("_CGDisplayHideCursor", CGDisplayHideCursor, "i:u"),
	G("_CGDisplayShowCursor", CGDisplayShowCursor, "i:u"),
	G("_CGCursorIsVisible", w_CGCursorIsVisible, "i:"),
	G("_CGWarpMouseCursorPosition", w_CGWarpMouseCursorPosition, "i:ff"),
	G("_CGAssociateMouseAndMouseCursorPosition", CGAssociateMouseAndMouseCursorPosition, "i:i"),
	G("_CGWindowLevelForKey", CGWindowLevelForKey, "i:i"),
	G("_CGDisplayIDToOpenGLDisplayMask", CGDisplayIDToOpenGLDisplayMask, "u:u"),
	G("_CGOpenGLDisplayMaskToDisplayID", CGOpenGLDisplayMaskToDisplayID, "u:u"),
	G("_CGDisplayIsActive", w_CGDisplayIsActive, "i:u"),
	G("_CGDisplayPixelsHigh", w_CGDisplayPixelsHigh, "u:u"),
	G("_CGDisplayPixelsWide", w_CGDisplayPixelsWide, "u:u"),
	G("_CGMainDisplayID", CGMainDisplayID, "u:"),
	G("_CGDisplayIOServicePort", CGDisplayIOServicePort, "u:u"),
	G("_CGSetDisplayTransferByTable", CGSetDisplayTransferByTable, "i:uuppp"),
	G("_CGImageSourceCreateWithURL", CGImageSourceCreateWithURL, "o:oo"),
	G("_CGImageSourceCreateImageAtIndex", CGImageSourceCreateImageAtIndex, "o:oio"),
	// IOKit (io_object_t are 32-bit Mach port names on both sides)
	D("_kIOMasterPortDefault", 4, init_zero),
	G("_IOServiceMatching", IOServiceMatching, "o:p"),
	G("_IOServiceGetMatchingServices", IOServiceGetMatchingServices, "i:uop"),
	G("_IOIteratorNext", IOIteratorNext, "u:u"),
	G("_IOObjectRelease", IOObjectRelease, "i:u"),
	G("_IORegistryEntryCreateCFProperty", IORegistryEntryCreateCFProperty, "o:uoou"),
	G("_IORegistryEntrySearchCFProperty", IORegistryEntrySearchCFProperty, "o:upoou"),
	G("_IORegistryEntryGetParentEntry", IORegistryEntryGetParentEntry, "i:upp"),
	// Carbon
	G("_Gestalt", w_Gestalt, "i:up"),
	G("_GetCurrentEventButtonState", GetCurrentEventButtonState, "u:"),
	G("_GetCurrentKeyModifiers", GetCurrentKeyModifiers, "u:"),
	G("_SetFrontProcess", w_SetFrontProcess, "i:p"),
	G("_TransformProcessType", w_TransformProcessType, "i:pu"),
	G("_SetSystemUIMode", w_SetSystemUIMode, "i:uu"),
	// CGL
	R("_CGLChoosePixelFormat", t_CGLChoosePixelFormat),
	R("_CGLCreateContext", t_CGLCreateContext),
	R("_CGLQueryRendererInfo", t_CGLQueryRendererInfo),
	G("_CGLDestroyContext", CGLDestroyContext, "i:o"),
	G("_CGLDestroyPixelFormat", CGLDestroyPixelFormat, "i:o"),
	G("_CGLDestroyRendererInfo", CGLDestroyRendererInfo, "i:o"),
	G("_CGLSetCurrentContext", CGLSetCurrentContext, "i:o"),
	G("_CGLGetCurrentContext", CGLGetCurrentContext, "o:"),
	G("_CGLEnable", CGLEnable, "i:ou"),
	G("_CGLDisable", CGLDisable, "i:ou"),
	G("_CGLSetParameter", CGLSetParameter, "i:oup"),
	G("_CGLGetParameter", CGLGetParameter, "i:oup"),
	G("_CGLDescribeRenderer", CGLDescribeRenderer, "i:oiup"),
	G("_CGLFlushDrawable", CGLFlushDrawable, "i:o"),
	G("_CGLLockContext", CGLLockContext, "i:o"),
	G("_CGLUnlockContext", CGLUnlockContext, "i:o"),
};

void thunks_apple_init(void) { REGISTER(apple); }
