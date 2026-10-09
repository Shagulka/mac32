// More of CoreFoundation, CoreGraphics, CoreVideo, IOKit, Carbon and AppKit:
// what SDL2 (GoldSrc's window and input layer) and the GoldSrc engine call.
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreVideo/CoreVideo.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDManager.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <IOKit/graphics/IOGraphicsLib.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <mach/mach.h>
#include "thunks.h"

#define U32(a) (*(uint32_t *)G2H(a))
static float F32(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

// ---- CoreFoundation -----------------------------------------------------------
static void t_CFDictionaryGetValueIfPresent(uint32_t *args, struct hres *r)
{
	const void *v = NULL;
	Boolean ok = CFDictionaryGetValueIfPresent(hobj_unwrap(A(0)), hobj_unwrap(A(1)), &v);
	if (A(2)) U32(A(2)) = ok ? hobj_wrap((void *)v) : 0;
	RET(ok);
}
static void t_CFPropertyListCreateFromXMLData(uint32_t *args, struct hres *r)
{
	CFStringRef err = NULL;
	CFPropertyListRef p = CFPropertyListCreateFromXMLData(NULL, hobj_unwrap(A(1)), A(2), &err);
	if (A(3)) U32(A(3)) = err ? hobj_wrap((void *)err) : 0;
	RET(hobj_wrap((void *)p));
}
static void t_CFSetGetValues(uint32_t *args, struct hres *r)
{
	(void)r;
	CFSetRef s = hobj_unwrap(A(0));
	CFIndex n = CFSetGetCount(s);
	const void **v = malloc(sizeof *v * (size_t)(n ? n : 1));
	CFSetGetValues(s, v);
	for (CFIndex i = 0; i < n; i++) ((g32 *)G2H(A(1)))[i] = hobj_wrap((void *)v[i]);
	free(v);
}
static void t_CFURLCreateDataAndPropertiesFromResource(uint32_t *args, struct hres *r)
{
	CFDataRef d = NULL;
	CFDictionaryRef props = NULL;
	SInt32 err = 0;
	Boolean ok = CFURLCreateDataAndPropertiesFromResource(NULL, hobj_unwrap(A(1)), A(2) ? &d : NULL,
							      A(3) ? &props : NULL, hobj_unwrap(A(4)), &err);
	if (A(2)) U32(A(2)) = hobj_wrap((void *)d);
	if (A(3)) U32(A(3)) = hobj_wrap((void *)props);
	if (A(5)) U32(A(5)) = (uint32_t)err;
	RET(ok);
}

// run loop sources with a guest perform callback
// i386 context: version info retain release copyDescription equal hash schedule cancel perform
struct gsource { g32 perform, info; };
static void source_perform(void *p) { struct gsource *s = p; gcall(s->perform, 1, s->info); }
static void t_CFRunLoopSourceCreate(uint32_t *args, struct hres *r)
{
	g32 c = A(2);
	if (U32(c)) die("CFRunLoopSourceCreate: version %u sources are not bridged", U32(c));
	struct gsource *s = malloc(sizeof *s);
	s->perform = U32(c + 36);
	s->info = U32(c + 4);
	CFRunLoopSourceContext hc = { .info = s, .perform = source_perform };
	RET(hobj_wrap(CFRunLoopSourceCreate(NULL, (CFIndex)(int32_t)A(1), &hc)));
}
// old SDL adds the (absent) notification port's source without checking it
static void t_CFRunLoopAddSource(uint32_t *args, struct hres *r)
{
	(void)r;
	if (A(1)) CFRunLoopAddSource(hobj_unwrap(A(0)), hobj_unwrap(A(1)), hobj_unwrap(A(2)));
}
static void silent_port(CFMachPortRef p, void *m, CFIndex n, void *i) { (void)p; (void)m; (void)n; (void)i; }
static void t_CGEventTapCreate(uint32_t *args, struct hres *r)
{
	(void)args;
	RET(hobj_wrap(CFMachPortCreate(NULL, silent_port, NULL, NULL)));
}
static CFRunLoopSourceRef w_CFMachPortCreateRunLoopSource(void *a, CFMachPortRef p, int order) { (void)a; return CFMachPortCreateRunLoopSource(NULL, p, order); }
static void init_rlmode(g32 a) { U32(a) = hobj_wrap((void *)kCFRunLoopDefaultMode); }
static void init_rlcommon(g32 a) { U32(a) = hobj_wrap((void *)kCFRunLoopCommonModes); }

// ---- CoreGraphics displays --------------------------------------------------------
// Display capture and fades would take the user's screens over (and leave them
// black if the game died); the game gets a borderless window instead.
static void t_ok(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_fade_reserve(uint32_t *args, struct hres *r) { if (A(1)) U32(A(1)) = 1; RET(0); }
static int w_CGDisplayIsMain(uint32_t d) { return CGDisplayIsMain(d); }
static uint32_t w_CGDisplayModeGetIOFlags(CGDisplayModeRef m) { return CGDisplayModeGetIOFlags(m); }
static int w_CGDisplayModeIsUsableForDesktopGUI(CGDisplayModeRef m) { return CGDisplayModeIsUsableForDesktopGUI(m); }
static CFStringRef w_CGDisplayModeCopyPixelEncoding(CGDisplayModeRef m)
{
	(void)m;
	return CFStringCreateCopy(NULL, CFSTR(IO32BitDirectPixels));	// what every mode reported on 32-bit macOS
}
static void t_CGDisplayMoveCursorToPoint(uint32_t *args, struct hres *r)
{
	RET(CGDisplayMoveCursorToPoint(A(0), CGPointMake(F32(A(1)), F32(A(2)))));
}
static void t_CGDisplayScreenSize(uint32_t *args, struct hres *r)
{
	CGSize s = CGDisplayScreenSize(A(0));
	float f[2] = { (float)s.width, (float)s.height };
	memcpy(&r->rax, f, 8);
}
static void t_CGRectIntersection(uint32_t *args, struct hres *r)
{
	CGRect a = CGRectMake(F32(A(1)), F32(A(2)), F32(A(3)), F32(A(4)));
	CGRect b = CGRectMake(F32(A(5)), F32(A(6)), F32(A(7)), F32(A(8)));
	CGRect c = CGRectIntersection(a, b);
	float *g = G2H(A(0));
	g[0] = (float)c.origin.x; g[1] = (float)c.origin.y; g[2] = (float)c.size.width; g[3] = (float)c.size.height;
	if (CGRectIsNull(c)) { g[0] = g[1] = INFINITY; g[2] = g[3] = 0; }
	r->rax = A(0);
	r->flags |= F_POP4;
}
static void t_CGRectIsNull(uint32_t *args, struct hres *r)
{
	RET(CGRectIsNull(CGRectMake(F32(A(0)), F32(A(1)), F32(A(2)), F32(A(3)))));
}
static void init_dupmodes(g32 a) { U32(a) = hobj_wrap((void *)kCGDisplayShowDuplicateLowResolutionModes); }
static uint32_t w_CGDisplayModeGetPixelWidth(CGDisplayModeRef m) { return (uint32_t)CGDisplayModeGetPixelWidth(m); }
static uint32_t w_CGDisplayModeGetPixelHeight(CGDisplayModeRef m) { return (uint32_t)CGDisplayModeGetPixelHeight(m); }
static int w_CGShieldingWindowLevel(void) { return CGShieldingWindowLevel(); }
static void init_utfileurl(g32 a) { U32(a) = hobj_wrap((void *)kUTTypeFileURL); }
static void init_black(g32 a) { U32(a) = hobj_wrap((void *)kCGColorBlack); }
static CGColorRef w_CGColorGetConstantColor(CFStringRef n) { return CGColorGetConstantColor(n); }

// private WindowServer calls SDL uses to keep Cmd-Tab working in fullscreen
extern uint32_t _CGSDefaultConnection(void);
extern int CGSSetGlobalHotKeyOperatingMode(uint32_t conn, int mode);
static uint32_t w_CGSDefaultConnection(void) { return _CGSDefaultConnection(); }
static int w_CGSSetGlobalHotKeyOperatingMode(uint32_t c, int m) { return CGSSetGlobalHotKeyOperatingMode(c, m); }

// ---- CoreVideo ----------------------------------------------------------------------
static void t_CVDisplayLinkCreateWithCGDisplay(uint32_t *args, struct hres *r)
{
	CVDisplayLinkRef l = NULL;
	CVReturn rc = CVDisplayLinkCreateWithCGDisplay(A(0), &l);
	U32(A(1)) = hobj_wrap(l);
	RET(rc);
}
static void t_CVDisplayLinkGetNominalOutputVideoRefreshPeriod(uint32_t *args, struct hres *r)
{
	CVTime t = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(hobj_unwrap(A(1)));
	uint8_t *g = G2H(A(0));	// int64 timeValue, int32 timeScale, int32 flags
	memcpy(g, &t.timeValue, 8);
	memcpy(g + 8, &t.timeScale, 4);
	memcpy(g + 12, &t.flags, 4);
	r->rax = A(0);
	r->flags |= F_POP4;
}
static void w_CVDisplayLinkRelease(CVDisplayLinkRef l) { CVDisplayLinkRelease(l); }

// ---- IOKit -----------------------------------------------------------------------------
// HID: a real manager that never reports devices, so SDL sees no game
// controllers (their callbacks are not bridged yet) and asks for no permission.
static IOHIDManagerRef w_IOHIDManagerCreate(void *alloc, uint32_t opts) { (void)alloc; return IOHIDManagerCreate(NULL, opts); }
static void w_IOHIDManagerSetDeviceMatchingMultiple(IOHIDManagerRef m, CFArrayRef a) { IOHIDManagerSetDeviceMatchingMultiple(m, a); }
static void t_nothing(uint32_t *args, struct hres *r) { (void)args; (void)r; }
static void t_zero(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_IOServiceAddMatchingNotification(uint32_t *args, struct hres *r)
{
	if (A(4)) U32(A(4)) = 0;
	RET(kIOReturnUnsupported);
}
static void t_IOServiceAddInterestNotification(uint32_t *args, struct hres *r)
{
	if (A(5)) U32(A(5)) = 0;	// (port, service, type, callback, refcon, *notification)
	RET(kIOReturnUnsupported);
}
static uint32_t w_IONotificationPortGetMachPort(IONotificationPortRef p) { return IONotificationPortGetMachPort(p); }
static int w_IOIteratorIsValid(uint32_t i) { return IOIteratorIsValid(i); }
static int w_IOObjectIsEqualTo(uint32_t a, uint32_t b) { return IOObjectIsEqualTo(a, b); }
static int w_IOObjectRetain(uint32_t o) { return IOObjectRetain(o); }
static CFDictionaryRef w_IODisplayCreateInfoDictionary(uint32_t s, uint32_t o) { return IODisplayCreateInfoDictionary(s, o); }
static void t_IOPMAssertionCreateWithDescription(uint32_t *args, struct hres *r)
{
	double timeout;
	memcpy(&timeout, &args[5], 8);
	IOPMAssertionID id = 0;
	IOReturn rc = IOPMAssertionCreateWithDescription(hobj_unwrap(A(0)), hobj_unwrap(A(1)), hobj_unwrap(A(2)),
							  hobj_unwrap(A(3)), hobj_unwrap(A(4)), timeout, hobj_unwrap(A(7)), &id);
	if (A(8)) U32(A(8)) = id;	// (type, name, details, reason, bundle, double timeout, timeoutAction, *id)
	RET(rc);
}
static int w_IOPMAssertionRelease(uint32_t id) { return IOPMAssertionRelease(id); }

// ---- keyboard layout ---------------------------------------------------------------------
static TISInputSourceRef w_TISCopyCurrentKeyboardLayoutInputSource(void) { return TISCopyCurrentKeyboardLayoutInputSource(); }
static void *w_TISGetInputSourceProperty(TISInputSourceRef s, CFStringRef k) { return TISGetInputSourceProperty(s, k); }
static void init_uchr(g32 a) { U32(a) = hobj_wrap((void *)kTISPropertyUnicodeKeyLayoutData); }
static void t_UCKeyTranslate(uint32_t *args, struct hres *r)
{
	UniCharCount len = 0;
	OSStatus rc = UCKeyTranslate(G2H(A(0)), (UInt16)A(1), (UInt16)A(2), A(3), A(4), A(5),
				     G2H(A(6)), A(7), &len, G2H(A(9)));
	U32(A(8)) = (uint32_t)len;
	RET(rc);
}
static uint32_t w_KBGetLayoutType(int t) { return KBGetLayoutType((SInt16)t); }
static uint32_t w_LMGetKbdType(void) { return LMGetKbdType(); }

// ---- AppKit and Launch Services ---------------------------------------------------------
static void t_LSOpenCFURLRef(uint32_t *args, struct hres *r)
{
	if (A(1)) U32(A(1)) = 0;
	RET(LSOpenCFURLRef(hobj_unwrap(A(0)), NULL));
}
static void t_NSMouseInRect(uint32_t *args, struct hres *r)
{
	NSPoint p = NSMakePoint(F32(A(0)), F32(A(1)));
	NSRect rc = NSMakeRect(F32(A(2)), F32(A(3)), F32(A(4)), F32(A(5)));
	RET(NSMouseInRect(p, rc, (A(6) & 0xff) != 0));
}
static int w_CGSetLocalEventsSuppressionInterval(double s) { return CGSetLocalEventsSuppressionInterval(s); }
static int w_CGEventSourceKeyState(int st, uint32_t key) { return CGEventSourceKeyState(st, (CGKeyCode)key); }
static void t_HideMenuBar(uint32_t *args, struct hres *r) { (void)args; (void)r; [NSMenu setMenuBarVisible:NO]; }
static void t_ShowMenuBar(uint32_t *args, struct hres *r) { (void)args; (void)r; [NSMenu setMenuBarVisible:YES]; }
static void t_NSPointInRect(uint32_t *args, struct hres *r)
{
	RET(NSPointInRect(NSMakePoint(F32(A(0)), F32(A(1))), NSMakeRect(F32(A(2)), F32(A(3)), F32(A(4)), F32(A(5)))));
}
static void t_NSRectFill(uint32_t *args, struct hres *r)
{
	(void)r;
	NSRectFill(NSMakeRect(F32(A(0)), F32(A(1)), F32(A(2)), F32(A(3))));
}
static NSArray *w_NSSearchPathForDirectoriesInDomains(uint32_t d, uint32_t m, int expand)
{
	return NSSearchPathForDirectoriesInDomains(d, m, (expand & 0xff) != 0);
}

// ---- Carbon leftovers ------------------------------------------------------------------------
static void t_Microseconds(uint32_t *args, struct hres *r)
{
	(void)r;
	uint64_t us = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
	memcpy(G2H(A(0)), &us, 8);	// UnsignedWide on Intel: lo, hi
}
static void t_one(uint32_t *args, struct hres *r) { (void)args; RET(1); }
static int w_GetTextEncodingFromScriptInfo(int s, int l, int rgn, uint32_t *enc) { return GetTextEncodingFromScriptInfo((ScriptCode)s, (LangCode)l, (RegionCode)rgn, enc); }
static int w_FSPathMakeRef(const char *p, FSRef *ref, Boolean *dir) { return FSPathMakeRef((const UInt8 *)p, ref, dir); }

// ---- CoreAudio / mach ----------------------------------------------------------------------------
static int w_AudioObjectGetPropertyDataSize(uint32_t o, const AudioObjectPropertyAddress *a, uint32_t qs, const void *q, uint32_t *out)
{
	return AudioObjectGetPropertyDataSize(o, a, qs, q, out);
}
static void t_AudioQueueNewInput(uint32_t *args, struct hres *r) { (void)args; RET(kAudioQueueErr_InvalidDevice); }	// no microphone (voice chat) yet
static uint32_t w_mach_host_self(void) { return mach_host_self(); }
static void t_host_page_size(uint32_t *args, struct hres *r) { U32(A(1)) = 4096; RET(0); }
static int w_host_statistics(uint32_t h, int flavor, void *info, uint32_t *cnt)
{
	mach_msg_type_number_t c = *cnt;
	int rc = host_statistics(h, flavor, info, &c);
	*cnt = c;
	return rc;
}

static struct hfn apple2_fns[] = {
	G("_CFArrayExchangeValuesAtIndices", CFArrayExchangeValuesAtIndices, "v:oii"),
	R("_CFDictionaryGetValueIfPresent", t_CFDictionaryGetValueIfPresent),
	R("_CFPropertyListCreateFromXMLData", t_CFPropertyListCreateFromXMLData),
	G("_CFSetGetCount", CFSetGetCount, "i:o"), R("_CFSetGetValues", t_CFSetGetValues),
	R("_CFURLCreateDataAndPropertiesFromResource", t_CFURLCreateDataAndPropertiesFromResource),
	R("_CFRunLoopSourceCreate", t_CFRunLoopSourceCreate),
	R("_CFRunLoopAddSource", t_CFRunLoopAddSource), G("_CFRunLoopSourceSignal", CFRunLoopSourceSignal, "v:o"),
	G("_CFRunLoopStop", CFRunLoopStop, "v:o"), G("_CFRunLoopWakeUp", CFRunLoopWakeUp, "v:o"),
	D("_kCFRunLoopDefaultMode", 4, init_rlmode), D("_kCFRunLoopCommonModes", 4, init_rlcommon),
	G("_CFRunLoopRemoveSource", CFRunLoopRemoveSource, "v:ooo"),
	G("_CFBundleGetInfoDictionary", CFBundleGetInfoDictionary, "o:o"),
	// a real event tap needs the Accessibility permission: hand out a port
	// that never fires, so callers that assume success (VLC's media keys) run on
	R("_CGEventTapCreate", t_CGEventTapCreate), R("_CGEventTapEnable", t_nothing),
	G("_CFMachPortCreateRunLoopSource", w_CFMachPortCreateRunLoopSource, "o:ooi"),
	G("_CFRunLoopRun", CFRunLoopRun, "v:"),
	G("_CGSetLocalEventsSuppressionInterval", w_CGSetLocalEventsSuppressionInterval, "i:d"),
	G("_CGEventSourceKeyState", w_CGEventSourceKeyState, "i:iu"),
	R("_HideMenuBar", t_HideMenuBar), R("_ShowMenuBar", t_ShowMenuBar), R("_UpdateSystemActivity", t_zero),
	R("_NSPointInRect", t_NSPointInRect),

	R("_CGAcquireDisplayFadeReservation", t_fade_reserve), R("_CGDisplayFade", t_ok),
	R("_CGReleaseDisplayFadeReservation", t_ok),
	R("_CGCaptureAllDisplays", t_ok), R("_CGDisplayCapture", t_ok),
	R("_CGDisplayRelease", t_ok), R("_CGReleaseAllDisplays", t_ok),
	G("_CGDisplayIsMain", w_CGDisplayIsMain, "i:u"), G("_CGDisplayMirrorsDisplay", CGDisplayMirrorsDisplay, "u:u"),
	G("_CGDisplayModeCopyPixelEncoding", w_CGDisplayModeCopyPixelEncoding, "o:o"),
	G("_CGDisplayModeGetIOFlags", w_CGDisplayModeGetIOFlags, "u:o"),
	G("_CGDisplayModeIsUsableForDesktopGUI", w_CGDisplayModeIsUsableForDesktopGUI, "i:o"),
	R("_CGDisplayMoveCursorToPoint", t_CGDisplayMoveCursorToPoint),
	R("_CGDisplayScreenSize", t_CGDisplayScreenSize),
	G("_CGDisplaySetDisplayMode", CGDisplaySetDisplayMode, "i:uoo"),
	G("_CGGetDisplayTransferByTable", CGGetDisplayTransferByTable, "i:uupppp"),
	G("_CGGetOnlineDisplayList", CGGetOnlineDisplayList, "i:upp"),
	D("_kCGDisplayShowDuplicateLowResolutionModes", 4, init_dupmodes),
	G("_CGDisplayModeGetPixelWidth", w_CGDisplayModeGetPixelWidth, "u:o"),
	G("_CGDisplayModeGetPixelHeight", w_CGDisplayModeGetPixelHeight, "u:o"),
	R("_CGRectIntersection", t_CGRectIntersection), R("_CGRectIsNull", t_CGRectIsNull),
	G("_CGShieldingWindowLevel", w_CGShieldingWindowLevel, "i:"),
	G("_CGColorGetConstantColor", w_CGColorGetConstantColor, "o:o"), D("_kCGColorBlack", 4, init_black),
	G("__CGSDefaultConnection", w_CGSDefaultConnection, "u:"),
	G("_CGSSetGlobalHotKeyOperatingMode", w_CGSSetGlobalHotKeyOperatingMode, "i:ui"),

	R("_CVDisplayLinkCreateWithCGDisplay", t_CVDisplayLinkCreateWithCGDisplay),
	R("_CVDisplayLinkGetNominalOutputVideoRefreshPeriod", t_CVDisplayLinkGetNominalOutputVideoRefreshPeriod),
	G("_CVDisplayLinkRelease", w_CVDisplayLinkRelease, "v:o"),

	G("_IOHIDManagerCreate", w_IOHIDManagerCreate, "o:pu"),
	G("_IOHIDManagerSetDeviceMatchingMultiple", w_IOHIDManagerSetDeviceMatchingMultiple, "v:oo"),
	R("_IOHIDManagerRegisterDeviceMatchingCallback", t_nothing),
	R("_IOHIDManagerRegisterDeviceRemovalCallback", t_nothing),
	R("_IOHIDManagerScheduleWithRunLoop", t_nothing), R("_IOHIDManagerUnscheduleFromRunLoop", t_nothing),
	R("_IOHIDManagerOpen", t_zero), R("_IOHIDManagerClose", t_zero), R("_IOHIDManagerCopyDevices", t_zero),
	R("_IOServiceAddMatchingNotification", t_IOServiceAddMatchingNotification),
	// no notification ports: SDL's HID discovery then stays off (it also
	// destroys a port twice when matching fails)
	R("_IONotificationPortCreate", t_zero), R("_IONotificationPortDestroy", t_nothing),
	R("_IONotificationPortGetRunLoopSource", t_zero), R("_IOServiceAddInterestNotification", t_IOServiceAddInterestNotification),
	G("_IONotificationPortGetMachPort", w_IONotificationPortGetMachPort, "u:o"),
	R("_IODispatchCalloutFromMessage", t_nothing),
	G("_IOIteratorIsValid", w_IOIteratorIsValid, "i:u"), G("_IOObjectIsEqualTo", w_IOObjectIsEqualTo, "i:uu"),
	G("_IOObjectRetain", w_IOObjectRetain, "i:u"),
	G("_IODisplayCreateInfoDictionary", w_IODisplayCreateInfoDictionary, "o:uu"),
	R("_IOPSCopyPowerSourcesInfo", t_zero),		// "power state unknown"
	R("_IOPMAssertionCreateWithDescription", t_IOPMAssertionCreateWithDescription),
	G("_IOPMAssertionRelease", w_IOPMAssertionRelease, "i:u"),

	G("_TISCopyCurrentKeyboardLayoutInputSource", w_TISCopyCurrentKeyboardLayoutInputSource, "o:"),
	G("_TISGetInputSourceProperty", w_TISGetInputSourceProperty, "o:oo"),
	D("_kTISPropertyUnicodeKeyLayoutData", 4, init_uchr),
	R("_UCKeyTranslate", t_UCKeyTranslate),
	G("_KBGetLayoutType", w_KBGetLayoutType, "u:i"), G("_LMGetKbdType", w_LMGetKbdType, "u:"),

	D("_kUTTypeFileURL", 4, init_utfileurl), R("_LSOpenCFURLRef", t_LSOpenCFURLRef), R("_NSMouseInRect", t_NSMouseInRect), R("_NSRectFill", t_NSRectFill),
	G("_NSSearchPathForDirectoriesInDomains", w_NSSearchPathForDirectoriesInDomains, "o:uui"),

	R("_Microseconds", t_Microseconds), R("_NSUnLinkModule", t_one), R("_WaitNextEvent", t_zero),
	G("_GetTextEncodingFromScriptInfo", w_GetTextEncodingFromScriptInfo, "i:iiip"),
	G("_FSPathMakeRef", w_FSPathMakeRef, "i:ppp"),

	G("_AudioObjectGetPropertyDataSize", w_AudioObjectGetPropertyDataSize, "i:upupp"),
	R("_AudioQueueNewInput", t_AudioQueueNewInput),
	G("_mach_host_self", w_mach_host_self, "u:"), R("_host_page_size", t_host_page_size),
	G("_host_statistics", w_host_statistics, "i:uipp"),
};

void thunks_apple2_init(void) { REGISTER(apple2_fns); }
