// Sockets, users, wide-char formatting, scandir and other libc pieces the
// Source engine needs, plus honest failure stubs for subsystems not bridged
// yet (audio, curl, Carbon windows). See docs/journal.md for what each stub means.
#include "thunks.h"
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>
#include <wchar.h>
#include <dirent.h>
#include <pwd.h>
#include <netdb.h>
#include <errno.h>
#include <copyfile.h>
#include <uuid/uuid.h>
#include <malloc/malloc.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <math.h>
#include <sched.h>
#include <ApplicationServices/ApplicationServices.h>

// ---- sockets: sockaddr and fd_set have the same layout on i386 ---------------
static uint32_t w_htonl(uint32_t v) { return htonl(v); }
static uint32_t w_ntohl(uint32_t v) { return ntohl(v); }
static int32_t w_send(int fd, const void *b, uint32_t n, int fl) { return (int32_t)send(fd, b, n, fl); }
static int32_t w_recv(int fd, void *b, uint32_t n, int fl) { return (int32_t)recv(fd, b, n, fl); }
static int32_t w_sendto(int fd, const void *b, uint32_t n, int fl, const struct sockaddr *a, uint32_t al)
{ return (int32_t)sendto(fd, b, n, fl, a, al); }
static int32_t w_recvfrom(int fd, void *b, uint32_t n, int fl, struct sockaddr *a, socklen_t *al)
{ return (int32_t)recvfrom(fd, b, n, fl, a, al); }
static int is_timeval_opt(int level, int opt) { return level == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO); }
static void t_setsockopt(uint32_t *args, struct hres *r)
{
	int fd = (int)A(0), lvl = (int)A(1), opt = (int)A(2);
	if (is_timeval_opt(lvl, opt)) {
		int32_t *g = G2H(A(3));
		struct timeval tv = { g[0], g[1] };
		RET(setsockopt(fd, lvl, opt, &tv, sizeof tv));
		return;
	}
	RET(setsockopt(fd, lvl, opt, G2H(A(3)), A(4)));
}
static void t_getsockopt(uint32_t *args, struct hres *r)
{
	int fd = (int)A(0), lvl = (int)A(1), opt = (int)A(2);
	if (is_timeval_opt(lvl, opt)) {
		struct timeval tv;
		socklen_t l = sizeof tv;
		int rc = getsockopt(fd, lvl, opt, &tv, &l);
		int32_t *g = G2H(A(3));
		g[0] = (int32_t)tv.tv_sec; g[1] = tv.tv_usec;
		*(uint32_t *)G2H(A(4)) = 8;
		RET(rc);
		return;
	}
	RET(getsockopt(fd, lvl, opt, G2H(A(3)), G2H(A(4))));
}
static void t_select(uint32_t *args, struct hres *r)
{
	struct timeval tv, *tp = NULL;
	if (A(4)) { int32_t *g = G2H(A(4)); tv.tv_sec = g[0]; tv.tv_usec = g[1]; tp = &tv; }
	int rc = select((int)A(0), A(1) ? G2H(A(1)) : NULL, A(2) ? G2H(A(2)) : NULL, A(3) ? G2H(A(3)) : NULL, tp);
	if (tp) { int32_t *g = G2H(A(4)); g[0] = (int32_t)tv.tv_sec; g[1] = tv.tv_usec; }
	RET(rc);
}
static __thread g32 ntoa_buf;
static void t_inet_ntoa(uint32_t *args, struct hres *r)
{
	struct in_addr a = { A(0) };
	if (!ntoa_buf) ntoa_buf = gcalloc(1, 32);
	strcpy(G2H(ntoa_buf), inet_ntoa(a));
	RET(ntoa_buf);
}
// struct hostent: name, aliases, addrtype, length, addr_list (5 words)
static g32 strv_copy(char **v)
{
	int n = 0;
	while (v && v[n]) n++;
	g32 a = gcalloc((uint32_t)n + 1, 4);
	for (int i = 0; i < n; i++) ((g32 *)G2H(a))[i] = gstrdup(v[i]);
	return a;
}
static __thread g32 hostent_buf;
static void t_gethostbyname(uint32_t *args, struct hres *r)
{
	struct hostent *h = gethostbyname(G2H(A(0)));
	if (!h) { RET(0); return; }
	if (!hostent_buf) hostent_buf = gcalloc(1, 20);	// old copies leak; DNS lookups are rare
	uint32_t *g = G2H(hostent_buf);
	g[0] = gstrdup(h->h_name);
	g[1] = strv_copy(h->h_aliases);
	g[2] = (uint32_t)h->h_addrtype;
	g[3] = (uint32_t)h->h_length;
	int n = 0;
	while (h->h_addr_list[n]) n++;
	g[4] = gcalloc((uint32_t)n + 1, 4);
	for (int i = 0; i < n; i++) {
		g32 ad = gmalloc((uint32_t)h->h_length);
		memcpy(G2H(ad), h->h_addr_list[i], (size_t)h->h_length);
		((g32 *)G2H(g[4]))[i] = ad;
	}
	RET(hostent_buf);
}
// struct passwd (i386): name passwd uid gid change class gecos dir shell expire
static __thread g32 pw_buf;
static void t_getpwuid(uint32_t *args, struct hres *r)
{
	struct passwd *p = getpwuid(A(0));
	if (!p) { RET(0); return; }
	if (!pw_buf) pw_buf = gcalloc(1, 40);
	uint32_t *g = G2H(pw_buf);
	g[0] = gstrdup(p->pw_name); g[1] = gstrdup(p->pw_passwd ? p->pw_passwd : "");
	g[2] = p->pw_uid; g[3] = p->pw_gid; g[4] = (uint32_t)p->pw_change;
	g[5] = gstrdup(p->pw_class ? p->pw_class : ""); g[6] = gstrdup(p->pw_gecos ? p->pw_gecos : "");
	g[7] = gstrdup(p->pw_dir); g[8] = gstrdup(p->pw_shell); g[9] = (uint32_t)p->pw_expire;
	RET(pw_buf);
}

// ---- time and process info ---------------------------------------------------
static void t_getrusage(uint32_t *args, struct hres *r)
{
	struct rusage u;
	int rc = getrusage((int)A(0), &u);
	int32_t *g = G2H(A(1));
	memset(g, 0, 72);
	g[0] = (int32_t)u.ru_utime.tv_sec; g[1] = u.ru_utime.tv_usec;
	g[2] = (int32_t)u.ru_stime.tv_sec; g[3] = u.ru_stime.tv_usec;
	long *l = &u.ru_maxrss;
	for (int i = 0; i < 14; i++) g[4 + i] = (int32_t)l[i];
	RET(rc);
}
static void t_futimes(uint32_t *args, struct hres *r)
{
	struct timeval tv[2], *tp = NULL;
	if (A(1)) {
		int32_t *g = G2H(A(1));
		tv[0].tv_sec = g[0]; tv[0].tv_usec = g[1]; tv[1].tv_sec = g[2]; tv[1].tv_usec = g[3];
		tp = tv;
	}
	RET(futimes((int)A(0), tp));
}
static __thread g32 ctime_buf;
static void t_ctime(uint32_t *args, struct hres *r)
{
	time_t t = *(int32_t *)G2H(A(0));
	char b[64];
	if (!ctime_buf) ctime_buf = gcalloc(1, 64);
	strcpy(G2H(ctime_buf), ctime_r(&t, b));
	RET(ctime_buf);
}
static void t_ctime_r(uint32_t *args, struct hres *r)
{
	time_t t = *(int32_t *)G2H(A(0));
	RET(ctime_r(&t, G2H(A(1))) ? A(1) : 0);
}
// mstats() returns a 20-byte struct through a hidden pointer
static void t_mstats(uint32_t *args, struct hres *r)
{
	struct mstats m = mstats();
	uint32_t *g = G2H(A(0));
	g[0] = (uint32_t)m.bytes_total; g[1] = (uint32_t)m.chunks_used; g[2] = (uint32_t)m.bytes_used;
	g[3] = (uint32_t)m.chunks_free; g[4] = (uint32_t)m.bytes_free;
	r->rax = A(0);
	r->flags |= F_POP4;
}
static int32_t w_atol(const char *s) { return (int32_t)atol(s); }
static float w_wcstof(const wchar_t *s, uint32_t endp)
{
	wchar_t *e;
	float v = wcstof(s, &e);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v;
}
static int w_copyfile(const char *from, const char *to, uint32_t state, uint32_t flags)
{
	if (state) die("copyfile with a state object is not supported");
	return copyfile(from, to, NULL, flags);
}

// ---- wide printf/scanf: run the narrow engine on a UTF-8 copy ------------------
static char *w2u(const wchar_t *w)
{
	size_t n = wcslen(w);
	char *o = malloc(n * 4 + 1), *p = o;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = (uint32_t)w[i];
		if (c < 0x80) *p++ = (char)c;
		else if (c < 0x800) { *p++ = (char)(0xc0 | c >> 6); *p++ = (char)(0x80 | (c & 0x3f)); }
		else if (c < 0x10000) { *p++ = (char)(0xe0 | c >> 12); *p++ = (char)(0x80 | (c >> 6 & 0x3f)); *p++ = (char)(0x80 | (c & 0x3f)); }
		else { *p++ = (char)(0xf0 | c >> 18); *p++ = (char)(0x80 | (c >> 12 & 0x3f)); *p++ = (char)(0x80 | (c >> 6 & 0x3f)); *p++ = (char)(0x80 | (c & 0x3f)); }
	}
	*p = 0;
	return o;
}
static int u2w(const char *s, wchar_t *out, uint32_t cap)
{
	const uint8_t *p = (const uint8_t *)s;
	uint32_t n = 0;
	while (*p) {
		uint32_t c = *p++;
		if (c >= 0xf0) { c = (c & 7) << 18 | (p[0] & 0x3fu) << 12 | (p[1] & 0x3fu) << 6 | (p[2] & 0x3fu); p += 3; }
		else if (c >= 0xe0) { c = (c & 15) << 12 | (p[0] & 0x3fu) << 6 | (p[1] & 0x3fu); p += 2; }
		else if (c >= 0xc0) { c = (c & 31) << 6 | (p[0] & 0x3fu); p += 1; }
		if (out && n + 1 < cap) out[n] = (wchar_t)c;
		n++;
	}
	if (out && cap) out[n < cap ? n : cap - 1] = 0;
	return (int)n;
}
static void wide_format(g32 dst, uint32_t cap, g32 wfmt, const uint32_t *ap, struct hres *r)
{
	char *f = w2u(G2H(wfmt));
	int n;
	char *s = gvformat(f, ap, &n);
	int wn = u2w(s, G2H(dst), cap);
	free(f); free(s);
	RET((uint32_t)wn < cap ? wn : -1);	// swprintf fails when the result does not fit
}
static void t_swprintf(uint32_t *args, struct hres *r) { wide_format(A(0), A(1), A(2), &args[3], r); }
static void t_vswprintf(uint32_t *args, struct hres *r) { wide_format(A(0), A(1), A(2), G2H(A(3)), r); }
static void t_swscanf(uint32_t *args, struct hres *r)
{
	// narrow scan of a UTF-8 copy; %ls targets are not supported
	char *in = w2u(G2H(A(0))), *f = w2u(G2H(A(1)));
	extern int mac32_sscanf(const char *s, const char *fmt, const uint32_t *ap);
	RET(mac32_sscanf(in, f, &args[2]));
	free(in); free(f);
}

// ---- scandir with guest callbacks; old 264-byte i386 struct dirent -----------
// old i386 dirent: ino u32, reclen u16, type u8, namlen u8, name[256] (264 bytes)
// $INODE64 dirent: ino u64, seekoff u64, reclen u16, namlen u16, type u8, name[1024]
#define NAME_OLD 8
#define NAME_64 21
static void alphasort_at(uint32_t *args, struct hres *r, uint32_t off)
{
	g32 a = *(g32 *)G2H(*(g32 *)G2H(A(0))), b = *(g32 *)G2H(*(g32 *)G2H(A(1)));
	RET(strcoll((char *)G2H(a + off), (char *)G2H(b + off)));
}
static void t_alphasort(uint32_t *args, struct hres *r) { alphasort_at(args, r, NAME_OLD); }
static void t_alphasort64(uint32_t *args, struct hres *r) { alphasort_at(args, r, NAME_64); }
static void scandir_as(uint32_t *args, struct hres *r, int inode64)
{
	DIR *d = opendir(G2H(A(0)));
	if (!d) { RET(-1); return; }
	g32 sel = A(2), cmp = A(3);
	uint32_t n = 0, cap = 64;
	g32 list = gmalloc(cap * 4);
	struct dirent *e;
	while ((e = readdir(d))) {
		g32 g;
		if (inode64) {
			uint32_t len = (NAME_64 + e->d_namlen + 1 + 3) & ~3u;
			g = gcalloc(1, len);
			uint8_t *p = G2H(g);
			memcpy(p, &e->d_ino, 8);
			memcpy(p + 8, &e->d_seekoff, 8);
			*(uint16_t *)(p + 16) = (uint16_t)len;
			*(uint16_t *)(p + 18) = e->d_namlen;
			p[20] = e->d_type;
			memcpy(p + NAME_64, e->d_name, e->d_namlen);
		} else {
			g = gcalloc(1, 264);
			uint8_t *p = G2H(g);
			*(uint32_t *)p = (uint32_t)e->d_ino;
			*(uint16_t *)(p + 4) = 264;
			p[6] = e->d_type;
			p[7] = (uint8_t)(e->d_namlen > 255 ? 255 : e->d_namlen);
			memcpy(p + NAME_OLD, e->d_name, p[7]);
		}
		if (sel && !(uint32_t)gcall(sel, 1, g)) { gfree(g); continue; }
		if (n == cap) { cap *= 2; list = grealloc(list, cap * 4); }
		((g32 *)G2H(list))[n++] = g;
	}
	closedir(d);
	if (cmp) {	// insertion sort through the guest comparator
		g32 *v = G2H(list);
		g32 pa = gmalloc(4), pb = gmalloc(4);
		for (uint32_t i = 1; i < n; i++)
			for (uint32_t j = i; j > 0; j--) {
				*(g32 *)G2H(pa) = v[j - 1];
				*(g32 *)G2H(pb) = v[j];
				if ((int32_t)gcall(cmp, 2, pa, pb) <= 0) break;
				g32 t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
			}
		gfree(pa); gfree(pb);
	}
	*(g32 *)G2H(A(1)) = list;
	RET(n);
}
static void t_scandir(uint32_t *args, struct hres *r) { scandir_as(args, r, 0); }
static void t_scandir64(uint32_t *args, struct hres *r) { scandir_as(args, r, 1); }

// ---- more stdio / locale ----------------------------------------------------
static void t_clearerr(uint32_t *args, struct hres *r) { (void)r; clearerr(gfile_host(A(0))); }
static void t_tmpfile(uint32_t *args, struct hres *r) { (void)args; RET(gfile_new(tmpfile())); }
static g32 tmpnam_buf;
static void t_tmpnam(uint32_t *args, struct hres *r)
{
	char b[L_tmpnam + 16];
	snprintf(b, sizeof b, "/tmp/mac32.%d.%u", getpid(), arc4random());
	if (A(0)) { strcpy(G2H(A(0)), b); RET(A(0)); return; }
	if (!tmpnam_buf) tmpnam_buf = gcalloc(1, 64);
	strcpy(G2H(tmpnam_buf), b);
	RET(tmpnam_buf);
}
extern FILE *gfile_reopen(g32 g, FILE *h);
static void t_freopen(uint32_t *args, struct hres *r)
{
	FILE *h = freopen(A(0) ? G2H(A(0)) : NULL, G2H(A(1)), gfile_host(A(2)));
	RET(h ? (gfile_reopen(A(2), h), A(2)) : 0);
}
static uint32_t w_clock(void) { return (uint32_t)clock(); }
static double w_difftime(int32_t a, int32_t b) { return difftime(a, b); }
static int w_finite(double d) { return isfinite(d); }
// struct lconv (i386): 10 char pointers, then 14 chars
static g32 lconv_buf;
static void t_localeconv(uint32_t *args, struct hres *r)
{
	(void)args;
	struct lconv *l = localeconv();
	if (!lconv_buf) lconv_buf = gcalloc(1, 56);
	g32 *p = G2H(lconv_buf);
	char *s[10] = { l->decimal_point, l->thousands_sep, l->grouping, l->int_curr_symbol, l->currency_symbol,
			l->mon_decimal_point, l->mon_thousands_sep, l->mon_grouping, l->positive_sign, l->negative_sign };
	for (int i = 0; i < 10; i++) p[i] = gstrdup(s[i] ? s[i] : "");
	char *c = (char *)(p + 10);
	char v[14] = { l->int_frac_digits, l->frac_digits, l->p_cs_precedes, l->p_sep_by_space, l->n_cs_precedes,
		       l->n_sep_by_space, l->p_sign_posn, l->n_sign_posn, l->int_p_cs_precedes, l->int_n_cs_precedes,
		       l->int_p_sep_by_space, l->int_n_sep_by_space, l->int_p_sign_posn, l->int_n_sign_posn };
	memcpy(c, v, 14);
	RET(lconv_buf);
}

// ---- OSAtomic / OSSpinLock (guest pointers are 32-bit, so Ptr ops are 32-bit ops)
#include <libkern/OSAtomic.h>
static int32_t a_add32(int32_t v, volatile int32_t *p) { return __sync_add_and_fetch(p, v); }
static int32_t a_inc32(volatile int32_t *p) { return __sync_add_and_fetch(p, 1); }
static int32_t a_dec32(volatile int32_t *p) { return __sync_sub_and_fetch(p, 1); }
static int64_t a_add64(int64_t v, volatile int64_t *p) { return __sync_add_and_fetch(p, v); }
static int a_cas32(int32_t o, int32_t n, volatile int32_t *p) { return __sync_bool_compare_and_swap(p, o, n); }
static int a_cas64(int64_t o, int64_t n, volatile int64_t *p) { return __sync_bool_compare_and_swap(p, o, n); }
static int32_t a_or32(uint32_t v, volatile uint32_t *p) { return (int32_t)__sync_or_and_fetch(p, v); }
static int32_t a_and32(uint32_t v, volatile uint32_t *p) { return (int32_t)__sync_and_and_fetch(p, v); }
static int32_t a_xor32(uint32_t v, volatile uint32_t *p) { return (int32_t)__sync_xor_and_fetch(p, v); }
static int32_t a_or32o(uint32_t v, volatile uint32_t *p) { return (int32_t)__sync_fetch_and_or(p, v); }
static int32_t a_and32o(uint32_t v, volatile uint32_t *p) { return (int32_t)__sync_fetch_and_and(p, v); }
static int a_tas(uint32_t n, volatile void *p)
{
	volatile uint8_t *b = (volatile uint8_t *)p + (n >> 3);
	uint8_t m = (uint8_t)(0x80 >> (n & 7));
	return (__sync_fetch_and_or(b, m) & m) != 0;
}
static int a_tac(uint32_t n, volatile void *p)
{
	volatile uint8_t *b = (volatile uint8_t *)p + (n >> 3);
	uint8_t m = (uint8_t)(0x80 >> (n & 7));
	return (__sync_fetch_and_and(b, (uint8_t)~m) & m) != 0;
}
static void a_barrier(void) { __sync_synchronize(); }
static void a_spin_lock(volatile int32_t *l) { while (!__sync_bool_compare_and_swap(l, 0, 1)) sched_yield(); }
static int a_spin_try(volatile int32_t *l) { return __sync_bool_compare_and_swap(l, 0, 1); }
static void a_spin_unlock(volatile int32_t *l) { __sync_synchronize(); *l = 0; }

// ---- Classic Mac OS calls used by the Bink video library ----------------------
#include <mach/mach_time.h>
#include <pthread.h>
static g32 w_NewPtr(uint32_t n) { return gcalloc(1, n ? n : 1); }
static void w_DisposePtr(uint32_t p) { gfree(p); }
static uint32_t w_MaxBlock(void) { return 256u << 20; }
// Handle = pointer to a master pointer to the block
static void t_TempNewHandle(uint32_t *args, struct hres *r)
{
	g32 h = gmalloc(4);
	*(g32 *)G2H(h) = gcalloc(1, A(0) ? A(0) : 1);
	if (A(1)) *(int16_t *)G2H(A(1)) = 0;
	RET(h);
}
static void t_TempDisposeHandle(uint32_t *args, struct hres *r)
{
	if (A(0)) { gfree(*(g32 *)G2H(A(0))); gfree(A(0)); }
	if (A(1)) *(int16_t *)G2H(A(1)) = 0;
	(void)r;
}
static void t_TempHLockUnlock(uint32_t *args, struct hres *r) { if (A(1)) *(int16_t *)G2H(A(1)) = 0; (void)r; }
// AbsoluteTime / Nanoseconds are 8-byte structs, returned in edx:eax
static uint64_t w_UpTime(void) { return mach_absolute_time(); }
static uint64_t w_AbsoluteToNanoseconds(uint64_t t)
{
	static mach_timebase_info_data_t tb;
	if (!tb.denom) mach_timebase_info(&tb);
	return t * tb.numer / tb.denom;
}
static int32_t w_OTAtomicAdd32(int32_t v, volatile int32_t *p) { return __sync_add_and_fetch(p, v); }
// MPCriticalRegionID -> handle holding a recursive host mutex
static void t_MPCreateCriticalRegion(uint32_t *args, struct hres *r)
{
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_t *m = malloc(sizeof *m);
	pthread_mutex_init(m, &a);
	*(g32 *)G2H(A(0)) = hobj_wrap(m);
	RET(0);
}
static void t_MPDeleteCriticalRegion(uint32_t *args, struct hres *r) { pthread_mutex_destroy(hobj_unwrap(A(0))); RET(0); }
static void t_MPEnterCriticalRegion(uint32_t *args, struct hres *r) { pthread_mutex_lock(hobj_unwrap(A(0))); RET(0); }
static void t_MPExitCriticalRegion(uint32_t *args, struct hres *r) { pthread_mutex_unlock(hobj_unwrap(A(0))); RET(0); }
static void t_HideCursor(uint32_t *args, struct hres *r) { (void)args; (void)r; CGDisplayHideCursor(0); }
static void t_ShowCursor(uint32_t *args, struct hres *r) { (void)args; (void)r; CGDisplayShowCursor(0); }
static void t_cfm_fail(uint32_t *args, struct hres *r) { (void)args; RET(-2804); }	// cfragNoLibraryErr
static void t_snd_fail(uint32_t *args, struct hres *r) { (void)args; RET(-201); }	// notEnoughHardwareErr
// Time Manager (Bink's timer). TMTask is 2-byte packed: qLink@0 qType@4
// tmAddr@6 tmCount@10 tmWakeUp@14 tmReserved@18. PrimeTime(task, n) calls
// tmAddr(task) once after n ms (n > 0) or -n microseconds (n < 0).
struct tmtask { g32 task; int active; uint64_t gen; };
static struct tmtask tms[64];
static pthread_mutex_t tm_lock = PTHREAD_MUTEX_INITIALIZER;
static dispatch_queue_t tm_queue;
static struct tmtask *tm_find(g32 task, int create)
{
	struct tmtask *f = NULL;
	for (int i = 0; i < 64; i++) {
		if (tms[i].task == task) return &tms[i];
		if (!tms[i].task && !f) f = &tms[i];
	}
	if (create && f) { f->task = task; f->active = 0; f->gen = 0; return f; }
	return NULL;
}
static void t_InstallTimeTask(uint32_t *args, struct hres *r)
{
	pthread_mutex_lock(&tm_lock);
	struct tmtask *t = tm_find(A(0), 1);
	if (t) t->active = 1;
	pthread_mutex_unlock(&tm_lock);
	if (!tm_queue) tm_queue = dispatch_queue_create("mac32.timemanager", DISPATCH_QUEUE_SERIAL);
	RET(t ? 0 : -108);	// memFullErr
}
static void t_RemoveTimeTask(uint32_t *args, struct hres *r)
{
	pthread_mutex_lock(&tm_lock);
	struct tmtask *t = tm_find(A(0), 0);
	if (t) { t->active = 0; t->task = 0; }
	pthread_mutex_unlock(&tm_lock);
	RET(0);
}
static void t_PrimeTime(uint32_t *args, struct hres *r)
{
	g32 task = A(0);
	int32_t n = (int32_t)A(1);
	pthread_mutex_lock(&tm_lock);
	struct tmtask *t = tm_find(task, 0);
	uint64_t gen = t ? ++t->gen : 0;
	pthread_mutex_unlock(&tm_lock);
	if (!t) { RET(-1); return; }
	int64_t ns = n >= 0 ? (int64_t)n * 1000000 : -(int64_t)n * 1000;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ns), tm_queue, ^{
		pthread_mutex_lock(&tm_lock);
		struct tmtask *u = tm_find(task, 0);
		int fire = u && u->active && u->gen == gen;
		pthread_mutex_unlock(&tm_lock);
		if (!fire) return;
		g32 proc;
		memcpy(&proc, G2H(task + 6), 4);
		if (proc) gcall(proc, 1, task);
	});
	RET(0);
}
// UPPs are plain function pointers on Mach-O
static void t_upp(uint32_t *args, struct hres *r) { RET(A(0)); }

// ---- more libc ---------------------------------------------------------------
static void *w_memcpy_chk(void *d, const void *s, uint32_t n, uint32_t dl) { if (n > dl) die("__memcpy_chk overflow"); return memcpy(d, s, n); }
static uint32_t w_mbstowcs(wchar_t *d, const char *s, uint32_t n) { return (uint32_t)mbstowcs(d, s, n); }
static uint32_t w_wcstombs(char *d, const wchar_t *s, uint32_t n) { return (uint32_t)wcstombs(d, s, n); }
static double w_wcstod(const wchar_t *s, uint32_t endp)
{
	wchar_t *e;
	double v = wcstod(s, &e);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v;
}
static float w_strtof(const char *s, uint32_t endp)
{
	char *e;
	float v = strtof(s, &e);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v;
}
static void init_daylight(g32 a) { tzset(); *(int32_t *)G2H(a) = daylight; }
static void init_timezone(g32 a) { tzset(); *(int32_t *)G2H(a) = (int32_t)timezone; }

// ---- old dyld API (NSSymbol / NSModule): answered from our loader ----------------
const char *macho_image_path(g32 a);
static char *with_underscore(const char *n) { static __thread char b[512]; snprintf(b, sizeof b, "%s", n); return b; }
static void t_NSIsSymbolNameDefined(uint32_t *args, struct hres *r) { RET(macho_find_any(with_underscore(G2H(A(0)))) != 0); }
// an NSSymbol / NSModule is the symbol's address
static void t_NSLookupAndBindSymbol(uint32_t *args, struct hres *r) { RET(macho_find_any(with_underscore(G2H(A(0))))); }
static void t_NSAddressOfSymbol(uint32_t *args, struct hres *r) { RET(A(0)); }
static void t_NSModuleForSymbol(uint32_t *args, struct hres *r) { RET(A(0)); }
static void t_NSLibraryNameForModule(uint32_t *args, struct hres *r)
{
	const char *p = macho_image_path(A(0));
	RET(p ? guest_cstr(p) : 0);
}
static void t_NSNameOfSymbol(uint32_t *args, struct hres *r)
{
	g32 off;
	const char *n = macho_addr_name(A(0), &off);
	const char *s = n ? strchr(n, '`') : NULL;
	RET(s ? guest_cstr(s + 1) : 0);
}

// ---- dyld helpers for the guest's libunwind ----------------------------------
int macho_find_unwind(g32 addr, g32 out);
int macho_dladdr(g32 addr, g32 out);
static void t_dyld_find_unwind_sections(uint32_t *args, struct hres *r) { RET(macho_find_unwind(A(0), A(1))); }
static void t_dladdr(uint32_t *args, struct hres *r) { RET(macho_dladdr(A(0), A(1))); }
static void t_posix_memalign(uint32_t *args, struct hres *r)
{
	g32 p = gmemalign(A(1), A(2));
	*(g32 *)G2H(A(0)) = p;
	RET(p ? 0 : 12);	// ENOMEM
}
// __cxa_demangle: not provided; status -2 makes callers print the mangled name
static void t_cxa_demangle(uint32_t *args, struct hres *r) { if (A(3)) *(int32_t *)G2H(A(3)) = -2; RET(0); }

// ---- Multiprocessing Services semaphores --------------------------------------------
#include <dispatch/dispatch.h>
static void t_MPCreateSemaphore(uint32_t *args, struct hres *r)
{
	dispatch_semaphore_t s = dispatch_semaphore_create((long)A(1));
	*(g32 *)G2H(A(2)) = hobj_wrap((void *)s);
	RET(0);
}
static void t_MPDeleteSemaphore(uint32_t *args, struct hres *r) { (void)args; RET(0); }	// kept alive: may still be waited on
static void t_MPSignalSemaphore(uint32_t *args, struct hres *r) { dispatch_semaphore_signal(hobj_unwrap(A(0))); RET(0); }
static void t_MPWaitOnSemaphore(uint32_t *args, struct hres *r)
{
	// Duration: >0 milliseconds, <0 microseconds, 0x7FFFFFFF forever
	int32_t d = (int32_t)A(1);
	dispatch_time_t t = d == 0x7fffffff ? DISPATCH_TIME_FOREVER
			   : dispatch_time(DISPATCH_TIME_NOW, d >= 0 ? (int64_t)d * 1000000 : -(int64_t)d * 1000);
	RET(dispatch_semaphore_wait(hobj_unwrap(A(0)), t) ? -29295 : 0);	// kMPTimeoutErr
}
static void t_GetCurrentProcess(uint32_t *args, struct hres *r) { uint32_t *p = G2H(A(0)); p[0] = 0; p[1] = 2; RET(0); }	// kCurrentProcess
static void t_sprintf_chk(uint32_t *args, struct hres *r)
{
	// buf, flag, size, fmt, ...
	int n;
	char *s = gvformat(G2H(A(3)), &args[4], &n);
	if ((uint32_t)n >= A(2)) die("__sprintf_chk overflow");
	memcpy(G2H(A(0)), s, (size_t)n + 1);
	free(s);
	RET(n);
}
static char *w_strcpy_chk(char *d, const char *s, uint32_t n) { if (strlen(s) >= n) die("__strcpy_chk overflow"); return strcpy(d, s); }
static char *w_strcat_chk(char *d, const char *s, uint32_t n) { if (strlen(d) + strlen(s) >= n) die("__strcat_chk overflow"); return strcat(d, s); }
static void *w_memmove_chk(void *d, const void *s, uint32_t n, uint32_t dl) { if (n > dl) die("__memmove_chk overflow"); return memmove(d, s, n); }
static void *w_memset_chk(void *d, int c, uint32_t n, uint32_t dl) { if (n > dl) die("__memset_chk overflow"); return memset(d, c, n); }

// ---- not bridged yet: report failure the way each API defines it -------------
static int warned_audio;
static void audio_off(void)
{
	if (!warned_audio++) fprintf(stderr, "[mac32] sound is not bridged yet; run the game with -nosound\n");
}
static void t_audio_err(uint32_t *args, struct hres *r) { (void)args; audio_off(); RET(0x77686174); }	// 'what'
static void t_noop0(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_curl_fail(uint32_t *args, struct hres *r) { (void)args; RET(2); }	// CURLE_FAILED_INIT
static void t_event_timeout(uint32_t *args, struct hres *r) { (void)args; RET(-9875); }	// eventLoopTimedOutErr

static struct hfn libc3[] = {
	G("_socket", socket, "i:iii"), G("_bind", bind, "i:ipu"), G("_connect", connect, "i:ipu"),
	G("_listen", listen, "i:ii"), G("_accept", accept, "i:ipp"),
	G("_send", w_send, "i:ipui"), G("_recv", w_recv, "i:ipui"),
	G("_sendto", w_sendto, "i:ipuipu"), G("_recvfrom", w_recvfrom, "i:ipuipp"),
	R("_setsockopt", t_setsockopt), R("_getsockopt", t_getsockopt), R("_select", t_select),
	G("_getsockname", getsockname, "i:ipp"), G("_getpeername", getpeername, "i:ipp"),
	G("_shutdown", shutdown, "i:ii"),
	G("_inet_addr", inet_addr, "u:p"), R("_inet_ntoa", t_inet_ntoa),
	G("_htonl", w_htonl, "u:u"), G("_ntohl", w_ntohl, "u:u"),
	R("_gethostbyname", t_gethostbyname), G("_gethostname", gethostname, "i:pu"),
	R("_getpwuid", t_getpwuid),
	R("_getrusage", t_getrusage), R("_futimes", t_futimes), R("_ctime", t_ctime), R("_ctime_r", t_ctime_r), R("_mstats", t_mstats),
	G("_tzset", tzset, "v:"), G("_setlocale", setlocale, "S:ip"),
	G("_uuid_generate", uuid_generate, "v:p"), G("_flock", flock, "i:ii"),
	G("_copyfile", w_copyfile, "i:pppu"), G("_atol", w_atol, "i:p"), G("_strtok", strtok, "p:pp"),
	G("_wcstof", w_wcstof, "f:pp"),
	R("_swprintf", t_swprintf), R("_vswprintf", t_vswprintf), R("_swscanf", t_swscanf),
	R("_scandir", t_scandir), R("_alphasort", t_alphasort),
	R("_scandir$INODE64", t_scandir64), R("_alphasort$INODE64", t_alphasort64),
	R("__dyld_find_unwind_sections", t_dyld_find_unwind_sections),
	R("__dyld_register_func_for_remove_image", t_noop0), R("__dyld_register_func_for_add_image", t_noop0),
	R("_dladdr", t_dladdr), R("_posix_memalign", t_posix_memalign), R("___cxa_demangle", t_cxa_demangle),
	R("_NSIsSymbolNameDefined", t_NSIsSymbolNameDefined), R("_NSLookupAndBindSymbol", t_NSLookupAndBindSymbol),
	R("_NSAddressOfSymbol", t_NSAddressOfSymbol), R("_NSModuleForSymbol", t_NSModuleForSymbol),
	R("_NSLibraryNameForModule", t_NSLibraryNameForModule), R("_NSNameOfSymbol", t_NSNameOfSymbol),
	R("_MPCreateSemaphore", t_MPCreateSemaphore), R("_MPDeleteSemaphore", t_MPDeleteSemaphore),
	R("_MPSignalSemaphore", t_MPSignalSemaphore), R("_MPWaitOnSemaphore", t_MPWaitOnSemaphore),
	R("_GetCurrentProcess", t_GetCurrentProcess), R("_CPSGetCurrentProcess", t_GetCurrentProcess),
	R("___sprintf_chk", t_sprintf_chk), G("___strcpy_chk", w_strcpy_chk, "p:ppu"), G("___strcat_chk", w_strcat_chk, "p:ppu"),
	G("___memmove_chk", w_memmove_chk, "p:ppuu"), G("___memset_chk", w_memset_chk, "p:piuu"),
	G("_sched_get_priority_max", sched_get_priority_max, "i:i"), G("_sched_get_priority_min", sched_get_priority_min, "i:i"),
	R("_SPBOpenDevice", t_snd_fail), R("_SPBCloseDevice", t_snd_fail), R("_SPBGetDeviceInfo", t_snd_fail),
	R("_SPBSetDeviceInfo", t_snd_fail), R("_SPBRecord", t_snd_fail), R("_SPBStopRecording", t_snd_fail),
	G("___memcpy_chk", w_memcpy_chk, "p:ppuu"), G("_mbstowcs", w_mbstowcs, "u:ppu"), G("_wcstombs", w_wcstombs, "u:ppu"),
	G("_wcscoll", wcscoll, "i:pp"), G("_wmemcmp", wmemcmp, "i:ppu"), G("_wcstod", w_wcstod, "d:pp"),
	G("_strtof", w_strtof, "f:pp"), G("_truncf", truncf, "f:f"),
	D("_daylight", 4, init_daylight), D("_timezone", 4, init_timezone),
	G("_OSAtomicAdd32", a_add32, "i:ip"), G("_OSAtomicAdd32Barrier", a_add32, "i:ip"),
	G("_OSAtomicIncrement32", a_inc32, "i:p"), G("_OSAtomicIncrement32Barrier", a_inc32, "i:p"),
	G("_OSAtomicDecrement32", a_dec32, "i:p"), G("_OSAtomicDecrement32Barrier", a_dec32, "i:p"),
	G("_OSAtomicAdd64", a_add64, "l:lp"), G("_OSAtomicAdd64Barrier", a_add64, "l:lp"),
	G("_OSAtomicCompareAndSwap32", a_cas32, "i:iip"), G("_OSAtomicCompareAndSwap32Barrier", a_cas32, "i:iip"),
	G("_OSAtomicCompareAndSwapInt", a_cas32, "i:iip"), G("_OSAtomicCompareAndSwapIntBarrier", a_cas32, "i:iip"),
	G("_OSAtomicCompareAndSwapLong", a_cas32, "i:iip"), G("_OSAtomicCompareAndSwapLongBarrier", a_cas32, "i:iip"),
	G("_OSAtomicCompareAndSwapPtr", a_cas32, "i:iip"), G("_OSAtomicCompareAndSwapPtrBarrier", a_cas32, "i:iip"),
	G("_OSAtomicCompareAndSwap64", a_cas64, "i:llp"), G("_OSAtomicCompareAndSwap64Barrier", a_cas64, "i:llp"),
	G("_OSAtomicOr32", a_or32, "i:up"), G("_OSAtomicOr32Barrier", a_or32, "i:up"),
	G("_OSAtomicAnd32", a_and32, "i:up"), G("_OSAtomicAnd32Barrier", a_and32, "i:up"),
	G("_OSAtomicXor32", a_xor32, "i:up"), G("_OSAtomicXor32Barrier", a_xor32, "i:up"),
	G("_OSAtomicOr32Orig", a_or32o, "i:up"), G("_OSAtomicOr32OrigBarrier", a_or32o, "i:up"),
	G("_OSAtomicAnd32Orig", a_and32o, "i:up"), G("_OSAtomicAnd32OrigBarrier", a_and32o, "i:up"),
	G("_OSAtomicTestAndSet", a_tas, "i:up"), G("_OSAtomicTestAndSetBarrier", a_tas, "i:up"),
	G("_OSAtomicTestAndClear", a_tac, "i:up"), G("_OSAtomicTestAndClearBarrier", a_tac, "i:up"),
	G("_OSMemoryBarrier", a_barrier, "v:"),
	G("_OSSpinLockLock", a_spin_lock, "v:p"), G("_OSSpinLockTry", a_spin_try, "i:p"),
	G("_OSSpinLockUnlock", a_spin_unlock, "v:p"),
	G("_NewPtr", w_NewPtr, "p:u"), G("_NewPtrClear", w_NewPtr, "p:u"), G("_DisposePtr", w_DisposePtr, "v:p"),
	G("_MaxBlock", w_MaxBlock, "u:"),
	R("_TempNewHandle", t_TempNewHandle), R("_TempDisposeHandle", t_TempDisposeHandle),
	R("_TempHLock", t_TempHLockUnlock), R("_TempHUnlock", t_TempHLockUnlock),
	G("_UpTime", w_UpTime, "l:"), G("_AbsoluteToNanoseconds", w_AbsoluteToNanoseconds, "l:l"),
	G("_OTAtomicAdd32", w_OTAtomicAdd32, "i:ip"),
	R("_MPCreateCriticalRegion", t_MPCreateCriticalRegion), R("_MPDeleteCriticalRegion", t_MPDeleteCriticalRegion),
	R("_MPEnterCriticalRegion", t_MPEnterCriticalRegion), R("_MPExitCriticalRegion", t_MPExitCriticalRegion),
	R("_HideCursor", t_HideCursor), R("_ShowCursor", t_ShowCursor),
	R("_GetSharedLibrary", t_cfm_fail), R("_FindSymbol", t_cfm_fail),
	R("_NewTimerUPP", t_upp), R("_DisposeTimerUPP", t_noop0),
	R("_InstallTimeTask", t_InstallTimeTask), R("_InstallXTimeTask", t_InstallTimeTask),
	R("_InsTime", t_InstallTimeTask), R("_InsXTime", t_InstallTimeTask),
	R("_PrimeTime", t_PrimeTime), R("_RemoveTimeTask", t_RemoveTimeTask), R("_RmvTime", t_RemoveTimeTask),
	R("_GetPort", t_noop0), R("_SetPort", t_noop0), R("_GetWindowPort", t_noop0), R("_GetWindowPortBounds", t_noop0),
	R("_LocalToGlobal", t_noop0), R("_GetDeviceList", t_noop0), R("_GetMainDevice", t_noop0), R("_GetNextDevice", t_noop0),
	R("_clearerr", t_clearerr), R("_tmpfile", t_tmpfile), R("_tmpnam", t_tmpnam), R("_freopen", t_freopen),
	G("_clock", w_clock, "u:"), G("_difftime", w_difftime, "d:ii"), G("_finite", w_finite, "i:d"),
	G("_strcoll", strcoll, "i:pp"), R("_localeconv", t_localeconv),
	// audio units: src/audio.c; pulling samples out of a unit is not bridged
	R("_AudioUnitRender", t_audio_err),
	// curl (server browser / news): not available yet
	R("_curl_global_init", t_noop0), R("_curl_easy_init", t_noop0), R("_curl_easy_cleanup", t_noop0),
	R("_curl_easy_setopt", t_curl_fail), R("_curl_easy_perform", t_curl_fail), R("_curl_easy_getinfo", t_curl_fail),
	// Carbon event/window calls: the Cocoa launcher drives events, so report "no events"
	R("_ReceiveNextEvent", t_event_timeout), R("_GetEventDispatcherTarget", t_noop0),
	R("_SendEventToEventTarget", t_noop0), R("_ReleaseEvent", t_noop0),
	R("_ConvertEventRefToEventRecord", t_noop0), R("_AEProcessAppleEvent", t_noop0),
	R("_ShowWindow", t_noop0), R("_CollapseWindow", t_noop0), R("_SetUserFocusWindow", t_noop0),
	R("_InvalWindowRect", t_noop0),
};

void thunks_libc3_init(void) { REGISTER(libc3); }
