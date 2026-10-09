// Calls from programs built for Mac OS X 10.4-10.6: the old crt1 start-up
// hooks, global errno/tzname, gzip, CoreGraphics images, ColorSync and
// assorted CoreServices calls (first case: OpenTTD 1.1).
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#include <zlib.h>
#include <pthread.h>
#include <langinfo.h>
#include <mach-o/arch.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <sys/socket.h>
#include "thunks.h"

#define U32(a) (*(uint32_t *)G2H(a))
static float F32(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

// ---- crt1 and libgcc of the 10.4 era -----------------------------------------
// crt1 calls *mach_init_routine and *_cthread_init_routine if they are set.
static void init_zero_ptr(g32 a) { U32(a) = 0; }
// keymgr: a handful of process-wide pointers old libgcc keeps for its unwinder
static g32 keymgr[16];
static pthread_mutex_t keymgr_lk = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static void t_keymgr_get_and_lock(uint32_t *args, struct hres *r)
{
	pthread_mutex_lock(&keymgr_lk);
	RET(A(0) < 16 ? keymgr[A(0)] : 0);
}
static void t_keymgr_get_and_lock_2(uint32_t *args, struct hres *r)	// (key, int *err)
{
	pthread_mutex_lock(&keymgr_lk);
	if (A(1)) U32(A(1)) = 0;
	RET(A(0) < 16 ? keymgr[A(0)] : 0);
}
static void t_keymgr_set_and_unlock(uint32_t *args, struct hres *r)
{
	if (A(0) < 16) keymgr[A(0)] = A(1);
	pthread_mutex_unlock(&keymgr_lk);
	RET(0);
}
static void t_nothing(uint32_t *args, struct hres *r) { (void)args; (void)r; }

// global errno (pre-threads code) and tzname: snapshots, not live values
static void init_errno(g32 a) { U32(a) = 0; }
static void init_tzname(g32 a)
{
	tzset();
	U32(a) = gstrdup(tzname[0]);
	U32(a + 4) = gstrdup(tzname[1]);
}

// ---- libc odds and ends ---------------------------------------------------------
static const char *w_nl_langinfo(int item) { return nl_langinfo(item); }
static const char *w_strsignal(int s) { return strsignal(s); }
// NXGetLocalArchInfo: {name, cputype, cpusubtype, byteorder, description}
static void t_NXGetLocalArchInfo(uint32_t *args, struct hres *r)
{
	(void)args;
	static g32 info;
	if (!info) {
		info = gcalloc(1, 20);
		U32(info) = gstrdup("i386");
		U32(info + 4) = CPU_TYPE_I386;
		U32(info + 8) = CPU_SUBTYPE_I386_ALL;
		U32(info + 12) = NX_LittleEndian;
		U32(info + 16) = gstrdup("Intel 80x86");
	}
	RET(info);
}

// ---- network lists, rebuilt in 32-bit layout --------------------------------------
static g32 copy_sockaddr(const struct sockaddr *sa)
{
	if (!sa) return 0;
	uint32_t n = sa->sa_len ? sa->sa_len : sizeof(struct sockaddr);
	g32 g = gmalloc(n);
	memcpy(G2H(g), sa, n);
	return g;
}
// struct ifaddrs (i386): next, name, flags, addr, netmask, dstaddr, data
static void t_getifaddrs(uint32_t *args, struct hres *r)
{
	struct ifaddrs *h;
	if (getifaddrs(&h)) { RET(-1); return; }
	g32 head = 0, *link = &head;
	for (struct ifaddrs *i = h; i; i = i->ifa_next) {
		g32 g = gcalloc(1, 28);
		U32(g + 4) = gstrdup(i->ifa_name);
		U32(g + 8) = i->ifa_flags;
		U32(g + 12) = copy_sockaddr(i->ifa_addr);
		U32(g + 16) = copy_sockaddr(i->ifa_netmask);
		U32(g + 20) = copy_sockaddr(i->ifa_dstaddr);
		*link = g;
		link = (g32 *)G2H(g);
	}
	freeifaddrs(h);
	U32(A(0)) = head;
	RET(0);
}
static void t_freeifaddrs(uint32_t *args, struct hres *r)
{
	(void)r;
	for (g32 g = A(0), n; g; g = n) {
		n = U32(g);
		for (int k = 1; k <= 5; k++) if (k != 2) gfree(U32(g + 4 * k));
		gfree(g);
	}
}
// struct addrinfo (i386): flags, family, socktype, protocol, addrlen, canonname, addr, next
static void t_getaddrinfo(uint32_t *args, struct hres *r)
{
	struct addrinfo hints, *hp = NULL, *res;
	if (A(2)) {
		uint32_t *h = G2H(A(2));
		memset(&hints, 0, sizeof hints);
		hints.ai_flags = (int)h[0]; hints.ai_family = (int)h[1];
		hints.ai_socktype = (int)h[2]; hints.ai_protocol = (int)h[3];
		hp = &hints;
	}
	int rc = getaddrinfo(A(0) ? G2H(A(0)) : NULL, A(1) ? G2H(A(1)) : NULL, hp, &res);
	if (rc) { RET(rc); return; }
	g32 head = 0, *link = &head;
	for (struct addrinfo *i = res; i; i = i->ai_next) {
		g32 g = gcalloc(1, 32);
		U32(g) = (uint32_t)i->ai_flags; U32(g + 4) = (uint32_t)i->ai_family;
		U32(g + 8) = (uint32_t)i->ai_socktype; U32(g + 12) = (uint32_t)i->ai_protocol;
		U32(g + 16) = i->ai_addrlen;
		U32(g + 20) = i->ai_canonname ? gstrdup(i->ai_canonname) : 0;
		U32(g + 24) = copy_sockaddr(i->ai_addr);
		*link = g;
		link = (g32 *)G2H(g + 28);
	}
	freeaddrinfo(res);
	U32(A(3)) = head;
	RET(0);
}
static void t_freeaddrinfo(uint32_t *args, struct hres *r)
{
	(void)r;
	for (g32 g = A(0), n; g; g = n) {
		n = U32(g + 28);
		gfree(U32(g + 20));
		gfree(U32(g + 24));
		gfree(g);
	}
}
static int w_getnameinfo(const struct sockaddr *sa, uint32_t salen, char *host, uint32_t hl, char *serv, uint32_t sl, int fl)
{
	return getnameinfo(sa, salen, host, hl, serv, sl, fl);
}
static const char *w_gai_strerror(int e) { return gai_strerror(e); }

// ---- zlib's gz* API on a host gzFile ------------------------------------------------
static gzFile w_gzdopen(int fd, const char *mode) { return gzdopen(fd, mode); }
static gzFile w_gzopen(const char *path, const char *mode) { return gzopen(path, mode); }
static int w_gzread(gzFile f, void *b, uint32_t n) { return gzread(f, b, n); }
static int w_gzwrite(gzFile f, const void *b, uint32_t n) { return gzwrite(f, b, n); }
static int w_gzclose(gzFile f) { return gzclose(f); }
static const char *w_gzerror(gzFile f, int *e) { return gzerror(f, e); }
static const char *w_zlibVersion(void) { return zlibVersion(); }

// ---- CoreFoundation / File Manager ---------------------------------------------------
static CFURLRef w_CFURLCreateFromFileSystemRepresentation(void *a, const uint8_t *p, int n, int dir)
{
	(void)a;
	return CFURLCreateFromFileSystemRepresentation(NULL, p, n, (dir & 0xff) != 0);
}
static int w_CFURLGetFSRef(CFURLRef u, FSRef *ref) { return CFURLGetFSRef(u, ref); }
static int w_FSRefMakePath(const FSRef *ref, uint8_t *p, uint32_t max) { return FSRefMakePath(ref, p, max); }
static int w_FSCompareFSRefs(const FSRef *a, const FSRef *b) { return FSCompareFSRefs(a, b); }

// ---- CoreGraphics images ----------------------------------------------------------------
static CGImageRef w_CGBitmapContextCreateImage(CGContextRef c) { return CGBitmapContextCreateImage(c); }
static void w_CGImageRelease(CGImageRef i) { CGImageRelease(i); }
static void w_CGContextSetInterpolationQuality(CGContextRef c, int q) { CGContextSetInterpolationQuality(c, q); }
static void t_CGContextDrawImage(uint32_t *args, struct hres *r)
{
	(void)r;
	CGContextDrawImage(hobj_unwrap(A(0)), CGRectMake(F32(A(1)), F32(A(2)), F32(A(3)), F32(A(4))), hobj_unwrap(A(5)));
}
static void t_CGImageCreateWithImageInRect(uint32_t *args, struct hres *r)
{
	RET(hobj_wrap(CGImageCreateWithImageInRect(hobj_unwrap(A(0)), CGRectMake(F32(A(1)), F32(A(2)), F32(A(3)), F32(A(4))))));
}
// ColorSync profiles are gone; the device RGB space stands in for "the system profile"
static void t_CMGetSystemProfile(uint32_t *args, struct hres *r) { if (A(0)) U32(A(0)) = 0; RET(0); }
static void t_zero(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static CGColorSpaceRef w_CGColorSpaceCreateWithPlatformColorSpace(void *p) { (void)p; return CGColorSpaceCreateDeviceRGB(); }

static struct hfn legacy_fns[] = {
	D("_mach_init_routine", 4, init_zero_ptr), D("__cthread_init_routine", 4, init_zero_ptr),
	R("__keymgr_get_and_lock_processwide_ptr", t_keymgr_get_and_lock),
	R("__keymgr_get_and_lock_processwide_ptr_2", t_keymgr_get_and_lock_2),
	R("__keymgr_set_and_unlock_processwide_ptr", t_keymgr_set_and_unlock),
	R("___keymgr_dwarf2_register_sections", t_nothing),
	D("_errno", 4, init_errno), D("_tzname", 8, init_tzname),
	G("_nl_langinfo", w_nl_langinfo, "S:i"), G("_strsignal", w_strsignal, "S:i"),
	R("_NXGetLocalArchInfo", t_NXGetLocalArchInfo),
	R("_getifaddrs", t_getifaddrs), R("_freeifaddrs", t_freeifaddrs),
	R("_getaddrinfo", t_getaddrinfo), R("_freeaddrinfo", t_freeaddrinfo),
	G("_getnameinfo", w_getnameinfo, "i:pupupui"), G("_gai_strerror", w_gai_strerror, "S:i"),
	G("_gzdopen", w_gzdopen, "o:ip"), G("_gzopen", w_gzopen, "o:pp"), G("_gzread", w_gzread, "i:opu"),
	G("_gzwrite", w_gzwrite, "i:opu"), G("_gzclose", w_gzclose, "i:o"), G("_gzerror", w_gzerror, "S:op"),
	G("_zlibVersion", w_zlibVersion, "S:"),
	G("_CFURLCreateFromFileSystemRepresentation", w_CFURLCreateFromFileSystemRepresentation, "o:ppii"),
	G("_CFURLGetFSRef", w_CFURLGetFSRef, "b:op"), G("_FSRefMakePath", w_FSRefMakePath, "i:ppu"),
	G("_FSCompareFSRefs", w_FSCompareFSRefs, "i:pp"),
	G("_CGBitmapContextCreateImage", w_CGBitmapContextCreateImage, "o:o"), G("_CGImageRelease", w_CGImageRelease, "v:o"),
	G("_CGContextSetInterpolationQuality", w_CGContextSetInterpolationQuality, "v:oi"),
	R("_CGContextDrawImage", t_CGContextDrawImage), R("_CGImageCreateWithImageInRect", t_CGImageCreateWithImageInRect),
	R("_CMGetSystemProfile", t_CMGetSystemProfile), R("_CMCloseProfile", t_zero),
	R("_CGDisplayRestoreColorSyncSettings", t_nothing),
	G("_CGColorSpaceCreateWithPlatformColorSpace", w_CGColorSpaceCreateWithPlatformColorSpace, "o:p"),
};

void thunks_legacy_init(void) { REGISTER(legacy_fns); }
