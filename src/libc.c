// libSystem thunks: libc, time, files, dl*, sysctl, and libgcc helpers.
#include "thunks.h"
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <os/lock.h>

// ---- guest FILE ----------------------------------------------------------
// i386 FILE is 88 bytes. Inline getc/putc macros read _r/_w; keeping those 0
// sends every access through __srget/__swbuf. _cookie (offset 28) holds an
// index into our table of host FILE pointers.
#define GFILE_SIZE 88
#define GF_COOKIE 28
#define GF_FILE 14
static FILE *files[4096];
static uint32_t nfiles = 1;
static os_unfair_lock flock_ = OS_UNFAIR_LOCK_INIT;

g32 gfile_new(FILE *h)
{
	if (!h) return 0;
	g32 g = gcalloc(1, GFILE_SIZE);
	os_unfair_lock_lock(&flock_);
	uint32_t i = nfiles++;
	if (i >= 4096) die("too many open guest files");
	files[i] = h;
	os_unfair_lock_unlock(&flock_);
	*(uint32_t *)G2H(g + GF_COOKIE) = i;
	*(int16_t *)G2H(g + GF_FILE) = (int16_t)fileno(h);
	return g;
}

FILE *gfile_host(g32 g)
{
	if (!g) return NULL;
	uint32_t i = *(uint32_t *)G2H(g + GF_COOKIE);
	if (i == 0 || i >= nfiles || !files[i]) die("bad guest FILE %#x", g);
	return files[i];
}

FILE *gfile_reopen(g32 g, FILE *h)
{
	files[*(uint32_t *)G2H(g + GF_COOKIE)] = h;
	*(int16_t *)G2H(g + GF_FILE) = (int16_t)fileno(h);
	return h;
}

static void gfile_close(g32 g)
{
	uint32_t i = *(uint32_t *)G2H(g + GF_COOKIE);
	files[i] = NULL;
	gfree(g);
}

static g32 g_stdin, g_stdout, g_stderr;
static void init_stdinp(g32 a) { *(g32 *)G2H(a) = g_stdin; }
static void init_stdoutp(g32 a) { *(g32 *)G2H(a) = g_stdout; }
static void init_stderrp(g32 a) { *(g32 *)G2H(a) = g_stderr; }

static void t_fopen(uint32_t *args, struct hres *r) { RET(gfile_new(fopen(G2H(A(0)), G2H(A(1))))); }
static void t_fdopen(uint32_t *args, struct hres *r) { RET(gfile_new(fdopen((int)A(0), G2H(A(1))))); }
static void t_fclose(uint32_t *args, struct hres *r) { FILE *f = gfile_host(A(0)); RET(fclose(f)); gfile_close(A(0)); }
static void t_fgetc(uint32_t *args, struct hres *r) { RET(fgetc(gfile_host(A(0)))); }
static void t_fputc(uint32_t *args, struct hres *r) { RET(fputc((int)A(0), gfile_host(A(1)))); }
static void t_fputs(uint32_t *args, struct hres *r) { RET(fputs(G2H(A(0)), gfile_host(A(1)))); }
static void t_fgets(uint32_t *args, struct hres *r) { RET(fgets(G2H(A(0)), (int)A(1), gfile_host(A(2))) ? A(0) : 0); }
static void t_fread(uint32_t *args, struct hres *r) { RET(fread(G2H(A(0)), A(1), A(2), gfile_host(A(3)))); }
static void t_fwrite(uint32_t *args, struct hres *r) { RET(fwrite(G2H(A(0)), A(1), A(2), gfile_host(A(3)))); }
static void t_fflush(uint32_t *args, struct hres *r) { RET(fflush(A(0) ? gfile_host(A(0)) : NULL)); }
static void t_fseek(uint32_t *args, struct hres *r) { RET(fseek(gfile_host(A(0)), (int32_t)A(1), (int)A(2))); }
static void t_fseeko(uint32_t *args, struct hres *r) { RET(fseeko(gfile_host(A(0)), (off_t)(A(1) | (uint64_t)A(2) << 32), (int)A(3))); }
static void t_ftell(uint32_t *args, struct hres *r) { RET(ftell(gfile_host(A(0)))); }
static void t_ftello(uint32_t *args, struct hres *r) { RET64(ftello(gfile_host(A(0)))); }
static void t_feof(uint32_t *args, struct hres *r) { RET(feof(gfile_host(A(0)))); }
static void t_ferror(uint32_t *args, struct hres *r) { RET(ferror(gfile_host(A(0)))); }
static void t_fileno(uint32_t *args, struct hres *r) { RET(fileno(gfile_host(A(0)))); }
static void t_rewind(uint32_t *args, struct hres *r) { (void)r; rewind(gfile_host(A(0))); }
static void t_setvbuf(uint32_t *args, struct hres *r) { RET(setvbuf(gfile_host(A(0)), G2H(A(1)), (int)A(2), A(3))); }
static void t_srget(uint32_t *args, struct hres *r) { RET(fgetc(gfile_host(A(0)))); }
static void t_swbuf(uint32_t *args, struct hres *r) { RET(fputc((int)A(0), gfile_host(A(1)))); }
static void t_ungetc(uint32_t *args, struct hres *r) { RET(ungetc((int)A(0), gfile_host(A(1)))); }

// ---- printf family -------------------------------------------------------
static void out_to(FILE *f, const char *fmt, const uint32_t *ap, struct hres *r)
{
	int n;
	char *s = gvformat(fmt, ap, &n);
	fwrite(s, 1, (size_t)n, f);
	free(s);
	RET(n);
}
static void t_printf(uint32_t *args, struct hres *r) { out_to(stdout, G2H(A(0)), &args[1], r); }
static void t_vprintf(uint32_t *args, struct hres *r) { out_to(stdout, G2H(A(0)), G2H(A(1)), r); }
static void t_fprintf(uint32_t *args, struct hres *r) { out_to(gfile_host(A(0)), G2H(A(1)), &args[2], r); }
static void t_vfprintf(uint32_t *args, struct hres *r) { out_to(gfile_host(A(0)), G2H(A(1)), G2H(A(2)), r); }

static void to_buf(g32 dst, uint32_t size, const char *fmt, const uint32_t *ap, struct hres *r)
{
	int n;
	char *s = gvformat(fmt, ap, &n);
	if (size) {
		uint32_t c = (uint32_t)n < size - 1 ? (uint32_t)n : size - 1;
		memcpy(G2H(dst), s, c);
		((char *)G2H(dst))[c] = 0;
	}
	free(s);
	RET(n);
}
static void t_sprintf(uint32_t *args, struct hres *r) { to_buf(A(0), 0x7fffffff, G2H(A(1)), &args[2], r); }
static void t_vsprintf(uint32_t *args, struct hres *r) { to_buf(A(0), 0x7fffffff, G2H(A(1)), G2H(A(2)), r); }
static void t_snprintf(uint32_t *args, struct hres *r) { to_buf(A(0), A(1), G2H(A(2)), &args[3], r); }
static void t_vsnprintf(uint32_t *args, struct hres *r) { to_buf(A(0), A(1), G2H(A(2)), G2H(A(3)), r); }
static void t_vasprintf(uint32_t *args, struct hres *r)
{
	int n;
	char *s = gvformat(G2H(A(1)), G2H(A(2)), &n);
	g32 g = gmalloc((uint32_t)n + 1);
	memcpy(G2H(g), s, (size_t)n + 1);
	free(s);
	*(g32 *)G2H(A(0)) = g;
	RET(n);
}
static void t_asprintf(uint32_t *args, struct hres *r)
{
	uint32_t a[3] = { A(0), A(1), H2G(&args[2]) };
	t_vasprintf(a, r);
}

// ---- environment, process ------------------------------------------------
static void t_getenv(uint32_t *args, struct hres *r)
{
	// copies live forever; getenv results are not freed by callers
	const char *v = getenv(G2H(A(0)));
	RET(v ? gstrdup(v) : 0);
}
static char exe_path[1024];
void set_exe_path(const char *p) { snprintf(exe_path, sizeof exe_path, "%s", p); }
static void t_NSGetExecutablePath(uint32_t *args, struct hres *r)
{
	uint32_t *size = G2H(A(1));
	uint32_t need = (uint32_t)strlen(exe_path) + 1;
	if (*size < need) { *size = need; RET(-1); return; }
	memcpy(G2H(A(0)), exe_path, need);
	RET(0);
}
static void init_NXArgc(g32 a) { *(int32_t *)G2H(a) = g_argc; }
static void init_NXArgv(g32 a) { *(g32 *)G2H(a) = g_argv; }
static void init_environ(g32 a) { *(g32 *)G2H(a) = g_envp; }
static void init_progname(g32 a) { *(g32 *)G2H(a) = g_progname; }
static g32 nsargc, nsargv, nsenviron, nsprogname;
static void t_NSGetArgc(uint32_t *args, struct hres *r) { (void)args; if (!nsargc) { nsargc = gmalloc(4); init_NXArgc(nsargc); } RET(nsargc); }
static void t_NSGetArgv(uint32_t *args, struct hres *r) { (void)args; if (!nsargv) { nsargv = gmalloc(4); init_NXArgv(nsargv); } RET(nsargv); }
static void t_NSGetEnviron(uint32_t *args, struct hres *r) { (void)args; if (!nsenviron) { nsenviron = gmalloc(4); init_environ(nsenviron); } RET(nsenviron); }
static void t_NSGetProgname(uint32_t *args, struct hres *r) { (void)args; if (!nsprogname) { nsprogname = gmalloc(4); init_progname(nsprogname); } RET(nsprogname); }

static g32 errno_slot;
static void t_error(uint32_t *args, struct hres *r)
{
	(void)args;
	if (!errno_slot) errno_slot = gmalloc(4);	// TODO: per thread
	*(int32_t *)G2H(errno_slot) = errno;
	RET(errno_slot);
}

static void t_stack_chk_fail(uint32_t *args, struct hres *r) { (void)args; (void)r; die("guest stack smashing detected"); }
static void init_stack_guard(g32 a) { *(uint32_t *)G2H(a) = arc4random() | 0xff; }

static void t_exit(uint32_t *args, struct hres *r);	// in cxx.c via atexit list
void run_guest_atexit(void);
static void t_exit(uint32_t *args, struct hres *r) { (void)r; run_guest_atexit(); fflush(NULL); exit((int)A(0)); }
static void t__exit(uint32_t *args, struct hres *r) { (void)r; fflush(NULL); _exit((int)A(0)); }
static void t_abort(uint32_t *args, struct hres *r) { (void)args; (void)r; die("guest called abort()"); }

// ---- time ----------------------------------------------------------------
// i386: time_t and long are 32 bits. struct timeval {i32, i32},
// struct timespec {i32, i32}, struct tm = 9 ints + long + char* = 44 bytes.
static void t_time(uint32_t *args, struct hres *r)
{
	int32_t t = (int32_t)time(NULL);
	if (A(0)) *(int32_t *)G2H(A(0)) = t;
	RET(t);
}
static void t_gettimeofday(uint32_t *args, struct hres *r)
{
	struct timeval tv;
	int rc = gettimeofday(&tv, NULL);
	if (A(0)) {
		int32_t *g = G2H(A(0));
		g[0] = (int32_t)tv.tv_sec;
		g[1] = tv.tv_usec;
	}
	if (A(1)) memset(G2H(A(1)), 0, 8);
	RET(rc);
}
static g32 tm_to_guest(const struct tm *t, g32 dst)
{
	int32_t *g = G2H(dst);
	g[0] = t->tm_sec; g[1] = t->tm_min; g[2] = t->tm_hour; g[3] = t->tm_mday;
	g[4] = t->tm_mon; g[5] = t->tm_year; g[6] = t->tm_wday; g[7] = t->tm_yday;
	g[8] = t->tm_isdst; g[9] = (int32_t)t->tm_gmtoff;
	g[10] = t->tm_zone ? gstrdup(t->tm_zone) : 0;	// leaks a few bytes per call
	return dst;
}
static void tm_from_guest(struct tm *t, g32 src)
{
	int32_t *g = G2H(src);
	memset(t, 0, sizeof *t);
	t->tm_sec = g[0]; t->tm_min = g[1]; t->tm_hour = g[2]; t->tm_mday = g[3];
	t->tm_mon = g[4]; t->tm_year = g[5]; t->tm_wday = g[6]; t->tm_yday = g[7];
	t->tm_isdst = g[8]; t->tm_gmtoff = g[9];
}
static __thread g32 tm_static;
static g32 tmbuf(void) { if (!tm_static) tm_static = gcalloc(1, 44); return tm_static; }
static void t_gmtime(uint32_t *args, struct hres *r)
{
	time_t t = *(int32_t *)G2H(A(0));
	struct tm tm;
	RET(gmtime_r(&t, &tm) ? tm_to_guest(&tm, tmbuf()) : 0);
}
static void t_localtime(uint32_t *args, struct hres *r)
{
	time_t t = *(int32_t *)G2H(A(0));
	struct tm tm;
	RET(localtime_r(&t, &tm) ? tm_to_guest(&tm, tmbuf()) : 0);
}
static void t_gmtime_r(uint32_t *args, struct hres *r)
{
	time_t t = *(int32_t *)G2H(A(0));
	struct tm tm;
	RET(gmtime_r(&t, &tm) ? tm_to_guest(&tm, A(1)) : 0);
}
static void t_localtime_r(uint32_t *args, struct hres *r)
{
	time_t t = *(int32_t *)G2H(A(0));
	struct tm tm;
	RET(localtime_r(&t, &tm) ? tm_to_guest(&tm, A(1)) : 0);
}
static void t_mktime(uint32_t *args, struct hres *r)
{
	struct tm tm;
	tm_from_guest(&tm, A(0));
	time_t t = mktime(&tm);
	tm_to_guest(&tm, A(0));
	RET((int32_t)t);
}
static void t_timegm(uint32_t *args, struct hres *r)
{
	struct tm tm;
	tm_from_guest(&tm, A(0));
	RET((int32_t)timegm(&tm));
}
static __thread g32 asc_static;
static void t_asctime(uint32_t *args, struct hres *r)
{
	struct tm tm;
	char b[64];
	tm_from_guest(&tm, A(0));
	if (!asc_static) asc_static = gcalloc(1, 64);
	asctime_r(&tm, b);
	strcpy(G2H(asc_static), b);
	RET(asc_static);
}
static void t_strftime(uint32_t *args, struct hres *r)
{
	struct tm tm;
	tm_from_guest(&tm, A(3));
	RET(strftime(G2H(A(0)), A(1), G2H(A(2)), &tm));
}
static void t_nanosleep(uint32_t *args, struct hres *r)
{
	int32_t *g = G2H(A(0));
	struct timespec ts = { g[0], g[1] }, rem;
	int rc = nanosleep(&ts, &rem);
	if (A(1)) { int32_t *o = G2H(A(1)); o[0] = (int32_t)rem.tv_sec; o[1] = (int32_t)rem.tv_nsec; }
	RET(rc);
}

// ---- sysctl: size_t is 32 bits in the guest -------------------------------
static int fix_sysctl_out(const int *name, unsigned namelen, void *out, size_t *len)
{
	(void)out; (void)len;
	// values whose type is long / struct with longs need narrowing
	if (namelen == 2 && name[0] == CTL_KERN && name[1] == KERN_BOOTTIME) return 1;
	return 0;
}
static void t_sysctl(uint32_t *args, struct hres *r)
{
	uint32_t *glen = G2H(A(3));
	size_t len = glen ? *glen : 0;
	int *name = G2H(A(0));
	char tmp[1024];
	int narrow = fix_sysctl_out(name, A(1), NULL, NULL);
	void *out = A(2) ? (narrow ? tmp : G2H(A(2))) : NULL;
	if (narrow) len = sizeof tmp;
	int rc = sysctl(name, A(1), out, glen ? &len : NULL, A(4) ? G2H(A(4)) : NULL, A(5));
	if (rc == 0 && narrow && A(2)) {
		struct timeval *tv = (void *)tmp;
		int32_t *g = G2H(A(2));
		g[0] = (int32_t)tv->tv_sec; g[1] = tv->tv_usec;
		len = 8;
	}
	if (glen) *glen = (uint32_t)len;
	RET(rc);
}
static void t_sysctlbyname(uint32_t *args, struct hres *r)
{
	uint32_t *glen = G2H(A(2));
	size_t len = glen ? *glen : 0;
	const char *nm = G2H(A(0));
	int rc;
	// 64-bit-only values: hand back 32 bits when the guest asked for 4 bytes
	uint64_t big;
	size_t blen = sizeof big;
	if (A(1) && glen && *glen == 4 && sysctlbyname(nm, &big, &blen, NULL, 0) == 0 && blen == 8) {
		*(uint32_t *)G2H(A(1)) = big > 0xffffffffu ? 0xffffffffu : (uint32_t)big;
		*glen = 4;
		RET(0);
		return;
	}
	rc = sysctlbyname(nm, A(1) ? G2H(A(1)) : NULL, glen ? &len : NULL, A(3) ? G2H(A(3)) : NULL, A(4));
	if (glen) *glen = (uint32_t)len;
	RET(rc);
}

// ---- dl* -> our own loader -----------------------------------------------
struct image *macho_dlopen(const char *path);
g32 macho_dlsym(struct image *img, const char *name);
static struct image *handles[1024];
static uint32_t nhandles = 2;	// 1 (handle 16) is SYS_HANDLE
static char dlerr[512];
static g32 dlerr_g;
// handle 16 stands for "the host's system libraries": dlsym on it looks in
// the host thunk table (e.g. libtogl dlopens OpenGL's libGL.dylib by path)
#define SYS_HANDLE 16
static int is_system_path(const char *p)
{
	return p && (!strncmp(p, "/System/", 8) || !strncmp(p, "/usr/lib/", 9));
}
static void t_dlopen(uint32_t *args, struct hres *r)
{
	const char *p = A(0) ? G2H(A(0)) : NULL;
	if (is_system_path(p)) { LOG("dlopen(%s): host library\n", p); RET(SYS_HANDLE); return; }
	struct image *img = macho_dlopen(p);
	if (!img) { snprintf(dlerr, sizeof dlerr, "dlopen(%s): image not found", p ? p : "NULL"); RET(0); return; }
	for (uint32_t i = 1; i < nhandles; i++) if (handles[i] == img) { RET(i * 16); return; }
	handles[nhandles] = img;
	RET(nhandles++ * 16);
}
static void t_dlsym(uint32_t *args, struct hres *r)
{
	char nm[512];
	snprintf(nm, sizeof nm, "_%s", (char *)G2H(A(1)));
	g32 h = A(0), a;
	if (h == 0xfffffffe || h == 0xfffffffd || h == 0xfffffffb)	// RTLD_DEFAULT/NEXT/SELF
		a = macho_find_any(nm);
	else if (h == SYS_HANDLE)
		a = host_lookup(nm);
	else {
		if (!h || h % 16 || h / 16 >= nhandles) {
			snprintf(dlerr, sizeof dlerr, "dlsym: invalid handle %#x", h);
			RET(0);
			return;
		}
		a = macho_dlsym(handles[h / 16], nm);
	}
	if (!a) snprintf(dlerr, sizeof dlerr, "dlsym: symbol %s not found", nm + 1);
	RET(a);
}
static void t_dlclose(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_dlerror(uint32_t *args, struct hres *r)
{
	(void)args;
	if (!dlerr[0]) { RET(0); return; }
	if (!dlerr_g) dlerr_g = gcalloc(1, 512);
	strcpy(G2H(dlerr_g), dlerr);
	dlerr[0] = 0;
	RET(dlerr_g);
}

// ---- libgcc 64-bit helpers -------------------------------------------------
static uint64_t udivdi3(uint64_t a, uint64_t b) { return a / b; }
static uint64_t umoddi3(uint64_t a, uint64_t b) { return a % b; }
static int64_t divdi3(int64_t a, int64_t b) { return a / b; }
static int64_t moddi3(int64_t a, int64_t b) { return a % b; }
static uint64_t fixunsdfdi(double d) { return (uint64_t)d; }
static int64_t fixdfdi(double d) { return (int64_t)d; }
static uint64_t fixunssfdi(float f) { return (uint64_t)f; }
static double floatdidf(int64_t v) { return (double)v; }
static double floatundidf(uint64_t v) { return (double)v; }
static float floatdisf(int64_t v) { return (float)v; }

// small wrappers whose host type is wider than the guest's
static uint32_t w_malloc_size(uint32_t p) { return gmalloc_size(p); }
static int w_tolower(int c) { return tolower(c); }
static int w_toupper(int c) { return toupper(c); }
static int w_rand(void) { return rand(); }
static double w_atof(const char *s) { return atof(s); }
static int32_t w_strtol(const char *s, uint32_t endp, int base)
{
	char *e;
	long v = strtol(s, &e, base);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}
static uint32_t w_strtoul(const char *s, uint32_t endp, int base)
{
	char *e;
	unsigned long v = strtoul(s, &e, base);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}
static double w_strtod(const char *s, uint32_t endp)
{
	char *e;
	double v = strtod(s, &e);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v;
}
static int64_t w_strtoll(const char *s, uint32_t endp, int base)
{
	char *e;
	long long v = strtoll(s, &e, base);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v;
}
static uint64_t w_strtoull(const char *s, uint32_t endp, int base)
{
	char *e;
	unsigned long long v = strtoull(s, &e, base);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return v;
}
static uint32_t w_strtok_r(char *s, const char *sep, uint32_t last)
{
	char *l = *(g32 *)G2H(last) ? G2H(*(g32 *)G2H(last)) : NULL;
	char *t = strtok_r(s, sep, &l);
	*(g32 *)G2H(last) = l ? H2G(l) : 0;
	return t ? H2G(t) : 0;
}
static void t_qsort(uint32_t *args, struct hres *r);
static void t_bsearch(uint32_t *args, struct hres *r);

// qsort with a guest comparator: simple merge-free insertion+shell sort that
// calls back into 32-bit code for each compare.
static void t_qsort(uint32_t *args, struct hres *r)
{
	(void)r;
	uint8_t *base = G2H(A(0));
	uint32_t n = A(1), sz = A(2);
	g32 cmp = A(3);
	uint8_t *tmp = malloc(sz);
	static const uint32_t gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
	for (int gi = 0; gi < 8; gi++) {
		uint32_t gap = gaps[gi];
		for (uint32_t i = gap; i < n; i++) {
			memcpy(tmp, base + i * sz, sz);
			uint32_t j = i;
			// tmp must live in guest memory for the comparator
			g32 gt = gmalloc(sz);
			memcpy(G2H(gt), tmp, sz);
			while (j >= gap && (int32_t)gcall(cmp, 2, H2G(base + (j - gap) * sz), gt) > 0) {
				memcpy(base + j * sz, base + (j - gap) * sz, sz);
				j -= gap;
			}
			memcpy(base + j * sz, G2H(gt), sz);
			gfree(gt);
		}
	}
	free(tmp);
}
static void t_bsearch(uint32_t *args, struct hres *r)
{
	g32 key = A(0), base = A(1), cmp = A(4);
	uint32_t n = A(2), sz = A(3), lo = 0, hi = n;
	while (lo < hi) {
		uint32_t mid = (lo + hi) / 2;
		int32_t c = (int32_t)gcall(cmp, 2, key, base + mid * sz);
		if (c == 0) { RET(base + mid * sz); return; }
		if (c < 0) hi = mid; else lo = mid + 1;
	}
	RET(0);
}

static struct hfn libc[] = {
	// memory
	G("_malloc", gmalloc, "p:u"),
	G("_calloc", gcalloc, "p:uu"),
	G("_realloc", grealloc, "p:pu"),
	G("_free", gfree, "v:p"),
	G("_malloc_size", w_malloc_size, "u:p"),
	G("_valloc", gmalloc, "p:u"),
	// strings
	G("_memcpy", memcpy, "p:ppu"),
	G("_memmove", memmove, "p:ppu"),
	G("_memset", memset, "p:piu"),
	G("_memcmp", memcmp, "i:ppu"),
	G("_memchr", memchr, "p:piu"),
	G("_bzero", bzero, "v:pu"),
	G("___bzero", bzero, "v:pu"),
	G("_strlen", strlen, "u:p"),
	G("_strnlen", strnlen, "u:pu"),
	G("_strcmp", strcmp, "i:pp"),
	G("_strncmp", strncmp, "i:ppu"),
	G("_strcasecmp", strcasecmp, "i:pp"),
	G("_strncasecmp", strncasecmp, "i:ppu"),
	G("_strcpy", strcpy, "p:pp"),
	G("_strncpy", strncpy, "p:ppu"),
	G("_strlcpy", strlcpy, "u:ppu"),
	G("_strlcat", strlcat, "u:ppu"),
	G("_strcat", strcat, "p:pp"),
	G("_strncat", strncat, "p:ppu"),
	G("_strchr", strchr, "p:pi"),
	G("_strrchr", strrchr, "p:pi"),
	G("_strstr", strstr, "p:pp"),
	G("_strcasestr", strcasestr, "p:pp"),
	G("_strpbrk", strpbrk, "p:pp"),
	G("_strspn", strspn, "u:pp"),
	G("_strcspn", strcspn, "u:pp"),
	G("_strtok_r", w_strtok_r, "p:ppp"),
	G("_atoi", atoi, "i:p"),
	G("_atof", w_atof, "d:p"),
	G("_strtol", w_strtol, "i:ppi"),
	G("_strtoul", w_strtoul, "u:ppi"),
	G("_strtod", w_strtod, "d:pp"),
	G("_strtoll", w_strtoll, "l:ppi"),
	G("_strtoull", w_strtoull, "l:ppi"),
	G("_strtoq", w_strtoll, "l:ppi"),
	G("_strtouq", w_strtoull, "l:ppi"),
	G("___tolower", w_tolower, "i:i"),
	G("___toupper", w_toupper, "i:i"),
	G("_tolower", w_tolower, "i:i"),
	G("_toupper", w_toupper, "i:i"),
	G("_wcslen", wcslen, "u:p"),
	G("_wcscmp", wcscmp, "i:pp"),
	G("_wcsncpy", wcsncpy, "p:ppu"),
	G("_wcscpy", wcscpy, "p:pp"),
	// stdio
	R("_fopen", t_fopen), R("_fdopen", t_fdopen), R("_fclose", t_fclose),
	R("_fgetc", t_fgetc), R("_getc", t_fgetc), R("_fputc", t_fputc), R("_putc", t_fputc),
	R("_fputs", t_fputs), R("_fgets", t_fgets), R("_fread", t_fread), R("_fwrite", t_fwrite),
	R("_fflush", t_fflush), R("_fseek", t_fseek), R("_fseeko", t_fseeko), R("_ftell", t_ftell),
	R("_ftello", t_ftello), R("_feof", t_feof), R("_ferror", t_ferror), R("_fileno", t_fileno),
	R("_rewind", t_rewind), R("_setvbuf", t_setvbuf), R("___srget", t_srget), R("___swbuf", t_swbuf),
	R("_ungetc", t_ungetc),
	R("_printf", t_printf), R("_vprintf", t_vprintf), R("_fprintf", t_fprintf), R("_vfprintf", t_vfprintf),
	R("_sprintf", t_sprintf), R("_vsprintf", t_vsprintf), R("_snprintf", t_snprintf),
	R("_vsnprintf", t_vsnprintf), R("_asprintf", t_asprintf), R("_vasprintf", t_vasprintf),
	G("_puts", puts, "i:p"),
	G("_putchar", putchar, "i:i"),
	G("_perror", perror, "v:p"),
	D("___stdinp", 4, init_stdinp), D("___stdoutp", 4, init_stdoutp), D("___stderrp", 4, init_stderrp),
	// process
	R("_getenv", t_getenv),
	G("_setenv", setenv, "i:ppi"),
	G("_unsetenv", unsetenv, "i:p"),
	R("__NSGetExecutablePath", t_NSGetExecutablePath),
	R("__NSGetArgc", t_NSGetArgc), R("__NSGetArgv", t_NSGetArgv),
	R("__NSGetEnviron", t_NSGetEnviron), R("__NSGetProgname", t_NSGetProgname),
	D("_NXArgc", 4, init_NXArgc), D("_NXArgv", 4, init_NXArgv),
	D("_environ", 4, init_environ), D("___progname", 4, init_progname),
	R("___error", t_error),
	G("_getpid", getpid, "i:"),
	G("_getuid", getuid, "u:"),
	G("_raise", raise, "i:i"),
	G("_kill", kill, "i:ii"),
	G("_alarm", alarm, "u:u"),
	G("_usleep", usleep, "i:u"),
	G("_sleep", sleep, "u:u"),
	G("_unlink", unlink, "i:p"),
	G("_rand", w_rand, "i:"),
	G("_srand", srand, "v:u"),
	G("_random", random, "i:"),
	G("_srandom", srandom, "v:u"),
	G("_arc4random", arc4random, "u:"),
	G("_getpagesize", getpagesize, "i:"),
	R("_exit", t_exit), R("__exit", t__exit), R("_abort", t_abort),
	R("___stack_chk_fail", t_stack_chk_fail),
	D("___stack_chk_guard", 4, init_stack_guard),
	R("_qsort", t_qsort), R("_bsearch", t_bsearch),
	// time
	R("_time", t_time), R("_gettimeofday", t_gettimeofday),
	R("_gmtime", t_gmtime), R("_localtime", t_localtime),
	R("_gmtime_r", t_gmtime_r), R("_localtime_r", t_localtime_r),
	R("_mktime", t_mktime), R("_timegm", t_timegm), R("_asctime", t_asctime),
	R("_strftime", t_strftime), R("_nanosleep", t_nanosleep),
	G("_mach_absolute_time", mach_absolute_time, "l:"),
	G("_mach_timebase_info", mach_timebase_info, "i:p"),
	// sysctl
	R("_sysctl", t_sysctl), R("_sysctlbyname", t_sysctlbyname),
	// dl
	R("_dlopen", t_dlopen), R("_dlsym", t_dlsym), R("_dlclose", t_dlclose), R("_dlerror", t_dlerror),
	// libgcc
	G("___udivdi3", udivdi3, "l:ll"), G("___umoddi3", umoddi3, "l:ll"),
	G("___divdi3", divdi3, "l:ll"), G("___moddi3", moddi3, "l:ll"),
	G("___fixunsdfdi", fixunsdfdi, "l:d"), G("___fixdfdi", fixdfdi, "l:d"),
	G("___fixunssfdi", fixunssfdi, "l:f"),
	G("___floatdidf", floatdidf, "d:l"), G("___floatundidf", floatundidf, "d:l"),
	G("___floatdisf", floatdisf, "f:l"),
};

void thunks_libc_init(void)
{
	g_stdin = gfile_new(stdin);
	g_stdout = gfile_new(stdout);
	g_stderr = gfile_new(stderr);
	REGISTER(libc);
}
