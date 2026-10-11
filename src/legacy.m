// Calls from programs built for Mac OS X 10.4-10.6: the old crt1 start-up
// hooks, global errno/tzname, gzip, CoreGraphics images, ColorSync and
// assorted CoreServices calls (first case: OpenTTD 1.1).
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <zlib.h>
#include <pthread.h>
#include <langinfo.h>
#include <mach-o/arch.h>
#include <xlocale.h>
#include <libgen.h>
#include <semaphore.h>
#include <poll.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <syslog.h>
#include <signal.h>
#include <dirent.h>
#include <termios.h>
#include <sys/statvfs.h>
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
// Window contexts. Old programs (SDL 1.2) paint a frame by drawing an image
// into [NSGraphicsContext graphicsContextWithWindow:] outside drawRect:.
// Layer-backed windows never show that, so such a draw goes to a layer laid
// over the window's content view instead. The bridge notes each window
// context it hands out (objc.m); the program's CGContextFlush/Release of it
// are dropped, since AppKit owns that context.
enum { NWCTX = 16 };
static struct { CGContextRef c; NSWindow *w; } wctx[NWCTX];
static unsigned wctx_next;
static pthread_mutex_t wctx_lk = PTHREAD_MUTEX_INITIALIZER;
static char frame_layer_key;

void window_context_note(CGContextRef c, NSWindow *w)
{
	pthread_mutex_lock(&wctx_lk);
	int i = 0;
	while (i < NWCTX && wctx[i].c != c) i++;
	if (i == NWCTX) i = wctx_next++ % NWCTX;
	wctx[i].c = c;
	wctx[i].w = w;
	pthread_mutex_unlock(&wctx_lk);
}

NSWindow *window_of_context(CGContextRef c)
{
	NSWindow *w = nil;
	pthread_mutex_lock(&wctx_lk);
	for (int i = 0; i < NWCTX && !w; i++) if (c && wctx[i].c == c) w = wctx[i].w;
	pthread_mutex_unlock(&wctx_lk);
	return w;
}

static void show_frame(NSWindow *w, CGImageRef img)
{
	NSView *v = w.contentView;
	if (!v) return;
	CALayer *l = objc_getAssociatedObject(v, &frame_layer_key);
	if (!l) {
		v.wantsLayer = YES;
		l = [CALayer layer];
		l.frame = v.layer.bounds;
		l.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
		l.contentsGravity = kCAGravityResize;
		l.magnificationFilter = kCAFilterNearest;
		l.zPosition = 1;
		[v.layer addSublayer:l];
		objc_setAssociatedObject(v, &frame_layer_key, l, OBJC_ASSOCIATION_RETAIN);
	}
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	l.contents = (__bridge id)img;
	[CATransaction commit];
}

static void t_CGContextDrawImage(uint32_t *args, struct hres *r)
{
	(void)r;
	CGContextRef c = hobj_unwrap(A(0));
	CGImageRef img = hobj_unwrap(A(5));
	NSWindow *w = window_of_context(c);
	if (!w) {
		CGContextDrawImage(c, CGRectMake(F32(A(1)), F32(A(2)), F32(A(3)), F32(A(4))), img);
		return;
	}
	if ([NSThread isMainThread]) { show_frame(w, img); return; }
	CGImageRetain(img);
	dispatch_async(dispatch_get_main_queue(), ^{ show_frame(w, img); CGImageRelease(img); });
}
static void t_CGImageCreateWithImageInRect(uint32_t *args, struct hres *r)
{
	RET(hobj_wrap(CGImageCreateWithImageInRect(hobj_unwrap(A(0)), CGRectMake(F32(A(1)), F32(A(2)), F32(A(3)), F32(A(4))))));
}
// ColorSync profiles are gone; the device RGB space stands in for "the system profile"
static void t_CMGetSystemProfile(uint32_t *args, struct hres *r) { if (A(0)) U32(A(0)) = 0; RET(0); }
static void t_zero(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static CGColorSpaceRef w_CGColorSpaceCreateWithPlatformColorSpace(void *p) { (void)p; return CGColorSpaceCreateDeviceRGB(); }

// ---- more libc for VLC 1.1 ---------------------------------------------------------
static uint32_t w_geteuid(void) { return geteuid(); }
static uint32_t w_getgid(void) { return getgid(); }
static uint32_t w_getegid(void) { return getegid(); }
static int w_setuid(uint32_t u) { return setuid(u); }
static g32 w_stpcpy(g32 d, const char *s) { size_t n = strlen(s); memcpy(G2H(d), s, n + 1); return d + (g32)n; }
// strsep(char **sp, delim): the string and the pointer both live in guest memory
static void t_strsep(uint32_t *args, struct hres *r)
{
	g32 *sp = G2H(A(0));
	if (!*sp) { RET(0); return; }
	char *str = G2H(*sp), *tok = str;
	char *e = strpbrk(str, G2H(A(1)));
	if (e) { *e = 0; *sp = H2G(e + 1); } else *sp = 0;
	RET(H2G(tok));
}
// locale_t: host handles; LC_GLOBAL_LOCALE is (locale_t)-1 on both sides
static locale_t loc_in(g32 g) { return g == 0xffffffffu ? LC_GLOBAL_LOCALE : g ? hobj_unwrap(g) : NULL; }
static g32 loc_out(locale_t l) { return l == LC_GLOBAL_LOCALE ? 0xffffffffu : l ? hobj_wrap(l) : 0; }
static void t_newlocale(uint32_t *args, struct hres *r) { RET(loc_out(newlocale((int)A(0), A(1) ? G2H(A(1)) : NULL, loc_in(A(2))))); }
static void t_uselocale(uint32_t *args, struct hres *r) { RET(loc_out(uselocale(loc_in(A(0))))); }
static void t_freelocale(uint32_t *args, struct hres *r) { (void)r; if (A(0) && A(0) != 0xffffffffu) freelocale(loc_in(A(0))); }
static void init_cfprefs_app(g32 a) { U32(a) = hobj_wrap((void *)kCFPreferencesCurrentApplication); }
static void init_in6addr_any(g32 a) { memset(G2H(a), 0, 16); }
static int w_inet_pton(int af, const char *s, void *d) { return inet_pton(af, s, d); }
static void t_inet_ntop(uint32_t *args, struct hres *r) { RET(inet_ntop((int)A(0), G2H(A(1)), G2H(A(2)), A(3)) ? A(2) : 0); }
static uint32_t w_if_nametoindex(const char *n) { return if_nametoindex(n); }
static int w_socketpair(int d, int t, int p, int *sv) { return socketpair(d, t, p, sv); }
static int w_sem_init(sem_t *s, int ps, uint32_t v) { return sem_init(s, ps, v); }
static int w_sem_wait(sem_t *s) { return sem_wait(s); }
static int w_sem_post(sem_t *s) { return sem_post(s); }
static int w_sem_destroy(sem_t *s) { return sem_destroy(s); }
static int w_poll(struct pollfd *f, uint32_t n, int t) { return poll(f, n, t); }
static void t_setcancelstate(uint32_t *args, struct hres *r)
{
	int old = 0;
	int rc = pthread_setcancelstate((int)A(0), &old);
	if (A(1)) U32(A(1)) = (uint32_t)old;
	RET(rc);
}
static void t_testcancel(uint32_t *args, struct hres *r) { (void)args; (void)r; pthread_testcancel(); }
static int32_t w_nrand48(unsigned short *x) { return (int32_t)nrand48(x); }
static int32_t w_jrand48(unsigned short *x) { return (int32_t)jrand48(x); }
static double w_erand48(unsigned short *x) { return erand48(x); }
static void t_div(uint32_t *args, struct hres *r) { div_t d = div((int)A(0), (int)A(1)); r->rax = (uint32_t)d.quot | (uint64_t)(uint32_t)d.rem << 32; }
static void t_lldiv(uint32_t *args, struct hres *r)	// 16-byte struct: through a hidden pointer
{
	long long n, dv;
	memcpy(&n, &args[1], 8);
	memcpy(&dv, &args[3], 8);
	lldiv_t q = lldiv(n, dv);
	memcpy(G2H(A(0)), &q.quot, 8);
	memcpy(G2H(A(0) + 8), &q.rem, 8);
	r->rax = A(0);
	r->flags |= F_POP4;
}
static const char *w_dirname(char *p) { return dirname(p); }
static const char *w_basename(char *p) { return basename(p); }
static int w_daemon(int a, int b) { return daemon(a, b); }
static void t_backtrace(uint32_t *args, struct hres *r) { (void)args; RET(0); }	// no guest unwinder here
static void t_vsprintf_chk(uint32_t *args, struct hres *r)	// (buf, flag, size, fmt, va)
{
	int n;
	char *c = gvformat(G2H(A(3)), G2H(A(4)), &n);
	uint32_t lim = A(2) ? A(2) : 0x7fffffff;
	uint32_t k = (uint32_t)n < lim - 1 ? (uint32_t)n : lim - 1;
	memcpy(G2H(A(0)), c, k);
	((char *)G2H(A(0)))[k] = 0;
	free(c);
	RET(n);
}
static void t_syslog(uint32_t *args, struct hres *r)
{
	(void)r;
	int n;
	char *c = gvformat(G2H(A(1)), &args[2], &n);
	fprintf(stderr, "[guest syslog %u] %s\n", A(0), c);
	free(c);
}
static double w_rint(double x) { return rint(x); }
static int32_t w_lrint(double x) { return (int32_t)lrint(x); }
static int32_t w_lrintf(float x) { return (int32_t)lrintf(x); }
static int w_fpclassifyd(double x) { return fpclassify(x); }
static int w_mlock(void *p, uint32_t n) { return mlock(p, n); }
static int w_munlock(void *p, uint32_t n) { return munlock(p, n); }
static int w_mkstemp(char *t) { return mkstemp(t); }
static void w_swab(const void *a, void *b, int n) { swab(a, b, n); }
static int w_fchdir(int fd) { return fchdir(fd); }
static int w_setpriority(int w, uint32_t who, int p) { return setpriority(w, who, p); }
static int w_putenv(char *s) { return putenv(s); }
static int w_link(const char *a, const char *b) { return link(a, b); }
static int w_popcount(uint32_t x) { return __builtin_popcount(x); }
static int w_uname(struct utsname *u) { return uname(u); }
static void w_srandomdev(void) { srandomdev(); }
static void t_atfork(uint32_t *args, struct hres *r) { (void)args; RET(0); }	// no fork in guests yet
// struct iovec (i386): {base, len} 8 bytes
static struct iovec *iov_in(g32 g, int n)
{
	struct iovec *v = calloc((size_t)(n > 0 ? n : 1), sizeof *v);
	for (int i = 0; i < n; i++) { v[i].iov_base = G2H(U32(g + 8 * (g32)i)); v[i].iov_len = U32(g + 8 * (g32)i + 4); }
	return v;
}
static void t_writev(uint32_t *args, struct hres *r) { struct iovec *v = iov_in(A(1), (int)A(2)); RET((uint32_t)writev((int)A(0), v, (int)A(2))); free(v); }
static void t_readv(uint32_t *args, struct hres *r) { struct iovec *v = iov_in(A(1), (int)A(2)); RET((uint32_t)readv((int)A(0), v, (int)A(2))); free(v); }
// struct msghdr (i386): name, namelen, iov, iovlen, control, controllen, flags
static void msg_in(g32 g, struct msghdr *m)
{
	memset(m, 0, sizeof *m);
	m->msg_name = U32(g) ? G2H(U32(g)) : NULL;
	m->msg_namelen = U32(g + 4);
	m->msg_iovlen = (int)U32(g + 12);
	m->msg_iov = iov_in(U32(g + 8), m->msg_iovlen);
	m->msg_control = U32(g + 16) ? G2H(U32(g + 16)) : NULL;
	m->msg_controllen = U32(g + 20);
	m->msg_flags = (int)U32(g + 24);
}
static void t_sendmsg(uint32_t *args, struct hres *r)
{
	struct msghdr m;
	msg_in(A(1), &m);
	RET((uint32_t)sendmsg((int)A(0), &m, (int)A(2)));
	free(m.msg_iov);
}
static void t_recvmsg(uint32_t *args, struct hres *r)
{
	struct msghdr m;
	msg_in(A(1), &m);
	ssize_t n = recvmsg((int)A(0), &m, (int)A(2));
	U32(A(1) + 4) = m.msg_namelen;
	U32(A(1) + 20) = m.msg_controllen;
	U32(A(1) + 24) = (uint32_t)m.msg_flags;
	free(m.msg_iov);
	RET((uint32_t)n);
}
// struct timeval (i386): {long, int} 8 bytes
static void t_utimes(uint32_t *args, struct hres *r)
{
	struct timeval tv[2], *p = NULL;
	if (A(1)) {
		int32_t *g = G2H(A(1));
		tv[0] = (struct timeval){ g[0], g[1] };
		tv[1] = (struct timeval){ g[2], g[3] };
		p = tv;
	}
	RET(utimes(G2H(A(0)), p));
}
static void t_setitimer(uint32_t *args, struct hres *r)
{
	int32_t *g = G2H(A(1));
	struct itimerval v = { { g[0], g[1] }, { g[2], g[3] } }, o;
	int rc = setitimer((int)A(0), &v, &o);
	if (A(2)) { int32_t *q = G2H(A(2)); q[0] = (int32_t)o.it_interval.tv_sec; q[1] = o.it_interval.tv_usec; q[2] = (int32_t)o.it_value.tv_sec; q[3] = o.it_value.tv_usec; }
	RET(rc);
}
// dyld's image list
static uint32_t w_dyld_image_count(void) { return macho_image_count(); }
static void t_dyld_get_image_name(uint32_t *args, struct hres *r)
{
	const char *n = macho_image_name(A(0));
	static g32 names[512];
	if (!n || A(0) >= 512) { RET(0); return; }
	if (!names[A(0)]) names[A(0)] = gstrdup(n);	// stays valid, like dyld's
	RET(names[A(0)]);
}
static uint32_t w_dyld_get_image_header(uint32_t i) { return macho_image_header(i); }
static int32_t w_dyld_get_image_vmaddr_slide(uint32_t i) { return macho_image_slide(i); }
static int w_shm_open(const char *n, int fl, int mode) { return shm_open(n, fl, mode); }
static int w_shm_unlink(const char *n) { return shm_unlink(n); }
static int w_sigwait(const sigset_t *set, int *sig) { return sigwait(set, sig); }
static void t_libiconv_set_relocation_prefix(uint32_t *args, struct hres *r) { (void)args; (void)r; }
static CFArrayRef w_CFLocaleCopyAvailableLocaleIdentifiers(void) { return CFLocaleCopyAvailableLocaleIdentifiers(); }
static CFArrayRef w_CFBundleCopyLocalizationsForPreferences(CFArrayRef a, CFArrayRef b) { return CFBundleCopyLocalizationsForPreferences(a, b); }

// struct statvfs (i386): bsize, frsize, blocks, bfree, bavail, files, ffree,
// favail, fsid, flag, namemax: eleven 32-bit words
static void statvfs_out(const struct statvfs *v, g32 g)
{
	uint32_t w[11] = { (uint32_t)v->f_bsize, (uint32_t)v->f_frsize, v->f_blocks, v->f_bfree, v->f_bavail,
			   v->f_files, v->f_ffree, v->f_favail, (uint32_t)v->f_fsid, (uint32_t)v->f_flag, (uint32_t)v->f_namemax };
	memcpy(G2H(g), w, sizeof w);
}
static void t_fstatvfs(uint32_t *args, struct hres *r) { struct statvfs v; int rc = fstatvfs((int)A(0), &v); if (!rc) statvfs_out(&v, A(1)); RET(rc); }
static void t_statvfs(uint32_t *args, struct hres *r) { struct statvfs v; int rc = statvfs(G2H(A(0)), &v); if (!rc) statvfs_out(&v, A(1)); RET(rc); }

// struct termios (i386): iflag oflag cflag lflag (4 bytes each), cc[20], ispeed, ospeed: 44 bytes
static void t_tcgetattr(uint32_t *args, struct hres *r)
{
	struct termios t;
	int rc = tcgetattr((int)A(0), &t);
	if (!rc) {
		uint8_t *g = G2H(A(1));
		uint32_t f[4] = { (uint32_t)t.c_iflag, (uint32_t)t.c_oflag, (uint32_t)t.c_cflag, (uint32_t)t.c_lflag };
		memcpy(g, f, 16);
		memcpy(g + 16, t.c_cc, 20);
		uint32_t sp[2] = { (uint32_t)t.c_ispeed, (uint32_t)t.c_ospeed };
		memcpy(g + 36, sp, 8);
	}
	RET(rc);
}
static void t_tcsetattr(uint32_t *args, struct hres *r)
{
	struct termios t;
	const uint8_t *g = G2H(A(2));
	uint32_t f[4], sp[2];
	memcpy(f, g, 16);
	memcpy(sp, g + 36, 8);
	memset(&t, 0, sizeof t);
	t.c_iflag = f[0]; t.c_oflag = f[1]; t.c_cflag = f[2]; t.c_lflag = f[3];
	memcpy(t.c_cc, g + 16, 20);
	t.c_ispeed = sp[0]; t.c_ospeed = sp[1];
	RET(tcsetattr((int)A(0), (int)A(1), &t));
}

// ---- Carbon Event Manager with guest handlers ---------------------------------------
// UPPs are plain function pointers on Mach-O. A guest handler gets a host
// handler that calls it with (EventHandlerCallRef, EventRef, userData).
struct ghandler { g32 fn, ud; };
static OSStatus call_guest_handler(EventHandlerCallRef call, EventRef ev, void *p)
{
	struct ghandler *h = p;
	return (OSStatus)gcall(h->fn, 3, hobj_wrap(call), hobj_wrap(ev), h->ud);
}
static void t_upp_identity(uint32_t *args, struct hres *r) { RET(A(0)); }
static void t_InstallEventHandler(uint32_t *args, struct hres *r)
{
	struct ghandler *h = malloc(sizeof *h);	// lives as long as the handler
	h->fn = A(1);
	h->ud = A(4);
	EventHandlerRef ref = NULL;
	OSStatus rc = InstallEventHandler(hobj_unwrap(A(0)), call_guest_handler, A(2), G2H(A(3)), h, A(5) ? &ref : NULL);
	if (A(5)) U32(A(5)) = hobj_wrap(ref);
	RET(rc);
}
static EventTargetRef w_GetApplicationEventTarget(void) { return GetApplicationEventTarget(); }
static EventQueueRef w_GetMainEventQueue(void) { return GetMainEventQueue(); }
static uint32_t w_GetEventClass(EventRef e) { return GetEventClass(e); }
static uint32_t w_GetEventKind(EventRef e) { return GetEventKind(e); }
static int w_RemoveEventHandler(EventHandlerRef h) { return RemoveEventHandler(h); }
static int w_CallNextEventHandler(EventHandlerCallRef c, EventRef e) { return CallNextEventHandler(c, e); }
// (event, name, desiredType, *actualType, bufferSize, *actualSize (ByteCount), data)
static void t_GetEventParameter(uint32_t *args, struct hres *r)
{
	ByteCount n = 0;
	EventParamType t = 0;
	OSStatus rc = GetEventParameter(hobj_unwrap(A(0)), A(1), A(2), A(3) ? &t : NULL, A(4), A(5) ? &n : NULL,
					A(6) ? G2H(A(6)) : NULL);
	if (A(3)) U32(A(3)) = t;
	if (A(5)) U32(A(5)) = (uint32_t)n;
	RET(rc);
}
static int w_GetFrontProcess(ProcessSerialNumber *p) { return GetFrontProcess(p); }
static int w_SameProcess(const ProcessSerialNumber *a, const ProcessSerialNumber *b, Boolean *res) { return SameProcess(a, b, res); }
static CFDictionaryRef w_ProcessInformationCopyDictionary(const ProcessSerialNumber *p, uint32_t m) { return ProcessInformationCopyDictionary(p, m); }
static int w_SetEventParameter(EventRef e, uint32_t name, uint32_t type, uint32_t size, const void *data) { return SetEventParameter(e, name, type, size, data); }

static struct hfn legacy_fns[] = {
	R("_tcgetattr", t_tcgetattr), R("_tcsetattr", t_tcsetattr),
	R("_fstatvfs", t_fstatvfs), R("_statvfs", t_statvfs),
	R("_NewEventHandlerUPP", t_upp_identity), R("_DisposeEventHandlerUPP", t_nothing),
	R("_InstallEventHandler", t_InstallEventHandler),
	G("_GetApplicationEventTarget", w_GetApplicationEventTarget, "o:"), G("_GetMainEventQueue", w_GetMainEventQueue, "o:"),
	G("_GetEventClass", w_GetEventClass, "u:o"), G("_GetEventKind", w_GetEventKind, "u:o"),
	G("_RemoveEventHandler", w_RemoveEventHandler, "i:o"), G("_CallNextEventHandler", w_CallNextEventHandler, "i:oo"),
	G("_GetFrontProcess", w_GetFrontProcess, "i:p"), G("_SameProcess", w_SameProcess, "i:ppp"),
	G("_ProcessInformationCopyDictionary", w_ProcessInformationCopyDictionary, "o:pu"),
	R("_GetEventParameter", t_GetEventParameter), G("_SetEventParameter", w_SetEventParameter, "i:ouuup"),
	G("_geteuid", w_geteuid, "u:"), G("_getgid", w_getgid, "u:"), G("_getegid", w_getegid, "u:"), G("_setuid", w_setuid, "i:u"),
	G("_stpcpy", w_stpcpy, "u:up"), R("_strsep", t_strsep),
	R("_newlocale", t_newlocale), R("_uselocale", t_uselocale), R("_freelocale", t_freelocale),
	D("_kCFPreferencesCurrentApplication", 4, init_cfprefs_app), D("_in6addr_any", 16, init_in6addr_any),
	G("_inet_pton", w_inet_pton, "i:ipp"), R("_inet_ntop", t_inet_ntop), G("_if_nametoindex", w_if_nametoindex, "u:p"),
	G("_socketpair", w_socketpair, "i:iiip"), G("_socketpair$UNIX2003", w_socketpair, "i:iiip"),
	G("_sem_init", w_sem_init, "i:piu"), G("_sem_wait", w_sem_wait, "i:p"), G("_sem_wait$UNIX2003", w_sem_wait, "i:p"),
	G("_sem_post", w_sem_post, "i:p"), G("_sem_destroy", w_sem_destroy, "i:p"),
	G("_poll", w_poll, "i:pui"), G("_poll$UNIX2003", w_poll, "i:pui"),
	R("_pthread_setcancelstate", t_setcancelstate), R("_pthread_setcancelstate$UNIX2003", t_setcancelstate),
	R("_pthread_testcancel", t_testcancel), R("_pthread_testcancel$UNIX2003", t_testcancel),
	G("_nrand48", w_nrand48, "i:p"), G("_jrand48", w_jrand48, "i:p"), G("_erand48", w_erand48, "d:p"),
	R("_div", t_div), R("_lldiv", t_lldiv),
	G("_dirname", w_dirname, "S:p"), G("_basename", w_basename, "S:p"),
	G("_daemon", w_daemon, "i:ii"), G("_daemon$1050", w_daemon, "i:ii"),
	R("_backtrace", t_backtrace), R("_backtrace_symbols_fd", t_nothing),
	R("___vsprintf_chk", t_vsprintf_chk), R("_syslog", t_syslog),
	G("_rint", w_rint, "d:d"), G("_lrint", w_lrint, "i:d"), G("_lrintf", w_lrintf, "i:f"), G("___fpclassifyd", w_fpclassifyd, "i:d"),
	G("_mlock", w_mlock, "i:pu"), G("_munlock", w_munlock, "i:pu"), G("_mkstemp", w_mkstemp, "i:p"),
	G("_swab", w_swab, "v:ppi"), G("_fchdir", w_fchdir, "i:i"), G("_setpriority", w_setpriority, "i:iui"),
	G("_putenv", w_putenv, "i:p"), G("_putenv$UNIX2003", w_putenv, "i:p"), G("_link", w_link, "i:pp"),
	G("___popcountsi2", w_popcount, "i:u"), G("_uname", w_uname, "i:p"), G("_srandomdev", w_srandomdev, "v:"),
	R("_pthread_atfork", t_atfork),
	R("_writev", t_writev), R("_writev$UNIX2003", t_writev), R("_readv", t_readv), R("_readv$UNIX2003", t_readv),
	R("_sendmsg", t_sendmsg), R("_sendmsg$UNIX2003", t_sendmsg), R("_recvmsg", t_recvmsg), R("_recvmsg$UNIX2003", t_recvmsg),
	R("_utimes", t_utimes), R("_setitimer", t_setitimer),
	G("__dyld_image_count", w_dyld_image_count, "u:"), R("__dyld_get_image_name", t_dyld_get_image_name),
	G("__dyld_get_image_header", w_dyld_get_image_header, "u:u"),
	G("__dyld_get_image_vmaddr_slide", w_dyld_get_image_vmaddr_slide, "i:u"),
	R("_libiconv_set_relocation_prefix", t_libiconv_set_relocation_prefix),
	G("_shm_open", w_shm_open, "i:pii"), G("_shm_unlink", w_shm_unlink, "i:p"),
	G("_sigwait", w_sigwait, "i:pp"), G("_sigwait$UNIX2003", w_sigwait, "i:pp"),
	G("_CFLocaleCopyAvailableLocaleIdentifiers", w_CFLocaleCopyAvailableLocaleIdentifiers, "o:"),
	G("_CFBundleCopyLocalizationsForPreferences", w_CFBundleCopyLocalizationsForPreferences, "o:oo"),
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
