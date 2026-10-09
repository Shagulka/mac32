// More libSystem: scanf, files, stat, processes, iconv, ctype, math,
// signals (ignored for now), and Mach/launchd calls the game only uses for
// crash reporting.
#include "thunks.h"
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <iconv.h>
#include <wchar.h>
#include <ctype.h>
#include <runetype.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <dirent.h>
#include <limits.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>

// ---- scanf: rewrite the format for 32-bit sizes, pass guest pointers ------
static char *scan_fmt(const char *f)
{
	char *o = malloc(strlen(f) * 2 + 1), *p = o;
	while (*f) {
		if (*f != '%') { *p++ = *f++; continue; }
		*p++ = *f++;
		if (*f == '%') { *p++ = *f++; continue; }
		if (*f == '*') *p++ = *f++;
		while (*f >= '0' && *f <= '9') *p++ = *f++;
		// length: i386 long is 32 bits, so "l" on integers becomes nothing
		int l = 0, h = 0;
		while (*f == 'l' || *f == 'h' || *f == 'q' || *f == 'L' || *f == 'z' || *f == 't' || *f == 'j') {
			if (*f == 'l') l++;
			else if (*f == 'q' || *f == 'j') l = 2;
			else if (*f == 'L') l = 3;
			else if (*f == 'h') h++;
			f++;
		}
		char c = *f;
		if (strchr("diouxXn", c)) {
			if (l == 2) { *p++ = 'l'; *p++ = 'l'; }
			for (int i = 0; i < h; i++) *p++ = 'h';
		} else if (strchr("eEfgGaA", c)) {
			if (l == 1) *p++ = 'l';
			else if (l == 3) *p++ = 'l';	// long double: treat as double (rare)
		} else if (c == 'p') {
			c = 'x';
		} else if ((c == 's' || c == 'c' || c == '[') && l == 1) {
			*p++ = 'l';
		}
		if (c == '[') {
			*p++ = *f++;
			if (*f == '^') *p++ = *f++;
			if (*f == ']') *p++ = *f++;
			while (*f && *f != ']') *p++ = *f++;
			if (*f) *p++ = *f++;
			continue;
		}
		if (c) { *p++ = c; f++; }
	}
	*p = 0;
	return o;
}
#define P32(i) ((void *)(uintptr_t)ap[i])
static int do_sscanf(const char *s, const char *fmt, const uint32_t *ap)
{
	char *f = scan_fmt(fmt);
	int n = sscanf(s, f, P32(0), P32(1), P32(2), P32(3), P32(4), P32(5), P32(6), P32(7),
		       P32(8), P32(9), P32(10), P32(11), P32(12), P32(13), P32(14), P32(15),
		       P32(16), P32(17), P32(18), P32(19), P32(20), P32(21), P32(22), P32(23));
	free(f);
	return n;
}
int mac32_sscanf(const char *s, const char *fmt, const uint32_t *ap) { return do_sscanf(s, fmt, ap); }
static void t_sscanf(uint32_t *args, struct hres *r) { RET(do_sscanf(G2H(A(0)), G2H(A(1)), &args[2])); }
static void t_vsscanf(uint32_t *args, struct hres *r) { RET(do_sscanf(G2H(A(0)), G2H(A(1)), G2H(A(2)))); }
static void t_fscanf(uint32_t *args, struct hres *r)
{
	// read a line and scan it: good enough for config files
	char line[4096];
	if (!fgets(line, sizeof line, gfile_host(A(0)))) { RET(EOF); return; }
	RET(do_sscanf(line, G2H(A(1)), &args[2]));
}

// ---- stat: old i386 layout (32-bit ino), 96 bytes --------------------------
static void stat_to_guest(const struct stat *s, g32 g)
{
	uint8_t *p = G2H(g);
	memset(p, 0, 96);
	*(int32_t *)(p + 0) = s->st_dev;
	*(uint32_t *)(p + 4) = (uint32_t)s->st_ino;
	*(uint16_t *)(p + 8) = s->st_mode;
	*(uint16_t *)(p + 10) = s->st_nlink;
	*(uint32_t *)(p + 12) = s->st_uid;
	*(uint32_t *)(p + 16) = s->st_gid;
	*(int32_t *)(p + 20) = s->st_rdev;
	*(int32_t *)(p + 24) = (int32_t)s->st_atimespec.tv_sec; *(int32_t *)(p + 28) = (int32_t)s->st_atimespec.tv_nsec;
	*(int32_t *)(p + 32) = (int32_t)s->st_mtimespec.tv_sec; *(int32_t *)(p + 36) = (int32_t)s->st_mtimespec.tv_nsec;
	*(int32_t *)(p + 40) = (int32_t)s->st_ctimespec.tv_sec; *(int32_t *)(p + 44) = (int32_t)s->st_ctimespec.tv_nsec;
	memcpy(p + 48, &s->st_size, 8);
	memcpy(p + 56, &s->st_blocks, 8);
	*(int32_t *)(p + 64) = s->st_blksize;
	*(uint32_t *)(p + 68) = s->st_flags;
	*(uint32_t *)(p + 72) = s->st_gen;
}
// $INODE64 layout: 144 bytes
static void stat64_to_guest(const struct stat *s, g32 g)
{
	uint8_t *p = G2H(g);
	memset(p, 0, 144);
	*(int32_t *)(p + 0) = s->st_dev;
	*(uint16_t *)(p + 4) = s->st_mode;
	*(uint16_t *)(p + 6) = s->st_nlink;
	memcpy(p + 8, &s->st_ino, 8);
	*(uint32_t *)(p + 16) = s->st_uid;
	*(uint32_t *)(p + 20) = s->st_gid;
	*(int32_t *)(p + 24) = s->st_rdev;
	const struct timespec *ts[4] = { &s->st_atimespec, &s->st_mtimespec, &s->st_ctimespec, &s->st_birthtimespec };
	for (int i = 0; i < 4; i++) {
		*(int32_t *)(p + 28 + i * 8) = (int32_t)ts[i]->tv_sec;
		*(int32_t *)(p + 32 + i * 8) = (int32_t)ts[i]->tv_nsec;
	}
	memcpy(p + 60, &s->st_size, 8);
	memcpy(p + 68, &s->st_blocks, 8);
	*(int32_t *)(p + 76) = s->st_blksize;
	*(uint32_t *)(p + 80) = s->st_flags;
	*(uint32_t *)(p + 84) = s->st_gen;
}
static void t_stat(uint32_t *args, struct hres *r) { struct stat s; int rc = stat(G2H(A(0)), &s); if (!rc) stat_to_guest(&s, A(1)); RET(rc); }
static void t_lstat(uint32_t *args, struct hres *r) { struct stat s; int rc = lstat(G2H(A(0)), &s); if (!rc) stat_to_guest(&s, A(1)); RET(rc); }
static void t_fstat(uint32_t *args, struct hres *r) { struct stat s; int rc = fstat((int)A(0), &s); if (!rc) stat_to_guest(&s, A(1)); RET(rc); }
static void t_stat64(uint32_t *args, struct hres *r) { struct stat s; int rc = stat(G2H(A(0)), &s); if (!rc) stat64_to_guest(&s, A(1)); RET(rc); }
static void t_lstat64(uint32_t *args, struct hres *r) { struct stat s; int rc = lstat(G2H(A(0)), &s); if (!rc) stat64_to_guest(&s, A(1)); RET(rc); }
static void t_fstat64(uint32_t *args, struct hres *r) { struct stat s; int rc = fstat((int)A(0), &s); if (!rc) stat64_to_guest(&s, A(1)); RET(rc); }

// lseek: off_t is 64-bit on i386 Darwin too
static int64_t w_lseek(int fd, int64_t off, int wh) { return lseek(fd, off, wh); }
static int32_t w_read(int fd, void *b, uint32_t n) { return (int32_t)read(fd, b, n); }
static int32_t w_write(int fd, const void *b, uint32_t n) { return (int32_t)write(fd, b, n); }
static int32_t w_pread(int fd, void *b, uint32_t n, int64_t off) { return (int32_t)pread(fd, b, n, off); }
static int32_t w_pwrite(int fd, const void *b, uint32_t n, int64_t off) { return (int32_t)pwrite(fd, b, n, off); }
static int w_open(const char *p, int fl, int mode) { return open(p, fl, mode); }
static int w_ftruncate(int fd, int64_t len) { return ftruncate(fd, len); }

static void t_getcwd(uint32_t *args, struct hres *r)
{
	if (!A(0)) {
		char b[PATH_MAX];
		RET(getcwd(b, sizeof b) ? gstrdup(b) : 0);
		return;
	}
	RET(getcwd(G2H(A(0)), A(1)) ? A(0) : 0);
}
static void t_realpath(uint32_t *args, struct hres *r)
{
	char b[PATH_MAX];
	if (!realpath(G2H(A(0)), b)) { RET(0); return; }
	if (A(1)) { strcpy(G2H(A(1)), b); RET(A(1)); }
	else RET(gstrdup(b));
}

// argv/envp arrays of 32-bit pointers -> host
static char **host_strv(g32 v)
{
	if (!v) return NULL;
	g32 *g = G2H(v);
	int n = 0;
	while (g[n]) n++;
	char **h = calloc((size_t)n + 1, sizeof *h);
	for (int i = 0; i < n; i++) h[i] = G2H(g[i]);
	return h;
}
static void t_execvp(uint32_t *args, struct hres *r) { char **v = host_strv(A(1)); RET(execvp(G2H(A(0)), v)); free(v); }
static void t_execv(uint32_t *args, struct hres *r) { char **v = host_strv(A(1)); RET(execv(G2H(A(0)), v)); free(v); }
static void t_execve(uint32_t *args, struct hres *r)
{
	char **v = host_strv(A(1)), **e = host_strv(A(2));
	RET(execve(G2H(A(0)), v, e));
	free(v); free(e);
}
static void t_popen(uint32_t *args, struct hres *r) { RET(gfile_new(popen(G2H(A(0)), G2H(A(1))))); }
static void t_pclose(uint32_t *args, struct hres *r) { RET(pclose(gfile_host(A(0)))); }
static int w_wait(uint32_t st) { int s = 0; int rc = wait(&s); if (st) *(int32_t *)G2H(st) = s; return rc; }
static int w_waitpid(int pid, uint32_t st, int opt) { int s = 0; int rc = waitpid(pid, &s, opt); if (st) *(int32_t *)G2H(st) = s; return rc; }

// opendir/readdir: DIR is a host object; struct dirent (old i386, 32-bit ino):
// d_ino u32, d_reclen u16, d_type u8, d_namlen u8, d_name[256]  = 264 bytes
static void t_opendir(uint32_t *args, struct hres *r);
static void t_readdir(uint32_t *args, struct hres *r);
static void t_closedir(uint32_t *args, struct hres *r);
static DIR *dirs[1024];
static void t_opendir(uint32_t *args, struct hres *r)
{
	DIR *d = opendir(G2H(A(0)));
	if (!d) { RET(0); return; }
	for (int i = 1; i < 1024; i++)
		if (!dirs[i]) {
			dirs[i] = d;
			g32 g = gcalloc(1, 264 + 8);
			*(uint32_t *)G2H(g + 264) = (uint32_t)i;
			RET(g);
			return;
		}
	closedir(d);
	RET(0);
}
static void t_readdir(uint32_t *args, struct hres *r)
{
	DIR *d = dirs[*(uint32_t *)G2H(A(0) + 264)];
	struct dirent *e = readdir(d);
	if (!e) { RET(0); return; }
	uint8_t *g = G2H(A(0));
	*(uint32_t *)g = (uint32_t)e->d_ino;
	*(uint16_t *)(g + 4) = 264;
	g[6] = e->d_type;
	g[7] = (uint8_t)(e->d_namlen > 255 ? 255 : e->d_namlen);
	memcpy(g + 8, e->d_name, g[7]);
	g[8 + g[7]] = 0;
	RET(A(0));
}
static void t_closedir(uint32_t *args, struct hres *r)
{
	uint32_t i = *(uint32_t *)G2H(A(0) + 264);
	int rc = closedir(dirs[i]);
	dirs[i] = NULL;
	gfree(A(0));
	RET(rc);
}

// ---- iconv: iconv_t is a host pointer, size_t is 32 bits ------------------
static iconv_t icv[256];
static void t_iconv_open(uint32_t *args, struct hres *r)
{
	iconv_t c = iconv_open(G2H(A(0)), G2H(A(1)));
	if (c == (iconv_t)-1) { RET(-1); return; }
	for (uint32_t i = 1; i < 256; i++) if (!icv[i]) { icv[i] = c; RET(i); return; }
	iconv_close(c);
	RET(-1);
}
static void t_iconv_close(uint32_t *args, struct hres *r) { RET(iconv_close(icv[A(0)])); icv[A(0)] = NULL; }
static void t_iconv(uint32_t *args, struct hres *r)
{
	g32 *gin = A(1) ? G2H(A(1)) : NULL, *gout = A(3) ? G2H(A(3)) : NULL;
	uint32_t *ginl = A(2) ? G2H(A(2)) : NULL, *goutl = A(4) ? G2H(A(4)) : NULL;
	char *in = gin && *gin ? G2H(*gin) : NULL, *out = gout && *gout ? G2H(*gout) : NULL;
	size_t inl = ginl ? *ginl : 0, outl = goutl ? *goutl : 0;
	size_t rc = iconv(icv[A(0)], gin ? &in : NULL, ginl ? &inl : NULL, gout ? &out : NULL, goutl ? &outl : NULL);
	if (gin && in) *gin = H2G(in);
	if (gout && out) *gout = H2G(out);
	if (ginl) *ginl = (uint32_t)inl;
	if (goutl) *goutl = (uint32_t)outl;
	RET((uint32_t)rc);
}

// ---- ctype: i386 _RuneLocale header is 52 bytes, then runetype/maplower/mapupper
static void init_runelocale(g32 g)
{
	uint8_t *p = G2H(g);
	memcpy(p, _DefaultRuneLocale.__magic, 8);
	memcpy(p + 8, _DefaultRuneLocale.__encoding, 32);
	*(int32_t *)(p + 48) = _DefaultRuneLocale.__invalid_rune;
	memcpy(p + 52, _DefaultRuneLocale.__runetype, 256 * 4);
	memcpy(p + 52 + 1024, _DefaultRuneLocale.__maplower, 256 * 4);
	memcpy(p + 52 + 2048, _DefaultRuneLocale.__mapupper, 256 * 4);
}
static g32 rune_ptr;
static void init_currentrune(g32 g) { *(g32 *)G2H(g) = rune_ptr; }
static uint32_t w_maskrune(int32_t c, uint32_t m) { return (uint32_t)__maskrune(c, m); }
static int32_t w_toupper_l(int32_t c) { return __toupper(c); }
static int32_t w_tolower_l(int32_t c) { return __tolower(c); }

// ---- misc --------------------------------------------------------------
static void t_strerror_r(uint32_t *args, struct hres *r) { RET(strerror_r((int)A(0), G2H(A(1)), A(2))); }
static g32 errstr[200];
static void t_strerror(uint32_t *args, struct hres *r)
{
	int e = (int)A(0);
	if (e >= 0 && e < 200) { if (!errstr[e]) errstr[e] = gstrdup(strerror(e)); RET(errstr[e]); }
	else RET(gstrdup(strerror(e)));
}
static int32_t w_wcstol(const wchar_t *s, uint32_t endp, int base)
{
	wchar_t *e;
	long v = wcstol(s, &e, base);
	if (endp) *(g32 *)G2H(endp) = H2G(e);
	return (int32_t)v;
}
static void w_memset_pattern16(void *b, const void *pat, uint32_t n) { memset_pattern16(b, pat, n); }
static void w_memset_pattern4(void *b, const void *pat, uint32_t n) { memset_pattern4(b, pat, n); }
static void w_memset_pattern8(void *b, const void *pat, uint32_t n) { memset_pattern8(b, pat, n); }
static void t_ignore0(uint32_t *args, struct hres *r) { (void)args; RET(0); }
static void t_debugger(uint32_t *args, struct hres *r) { (void)args; (void)r; fprintf(stderr, "[mac32] guest called Debugger()\n"); }

// signals: record but do not deliver for now
static void t_sigaction(uint32_t *args, struct hres *r)
{
	if (A(2)) memset(G2H(A(2)), 0, 12);	// i386 struct sigaction: handler, mask, flags
	LOG("guest sigaction(%u) ignored\n", A(0));
	RET(0);
}
static void t_signal(uint32_t *args, struct hres *r) { LOG("guest signal(%u) ignored\n", A(0)); RET(0); }
static void t_sigprocmask(uint32_t *args, struct hres *r) { if (A(2)) *(uint32_t *)G2H(A(2)) = 0; RET(0); }

// mmap from the guest must land below 4 GB
static void t_mmap(uint32_t *args, struct hres *r)
{
	uint32_t len = A(1);
	int prot = (int)A(2), flags = (int)A(3), fd = (int)A(4);
	off_t off = (off_t)(A(5) | (uint64_t)A(6) << 32);
	g32 at = A(0) && (flags & MAP_FIXED) ? A(0) : low_pages(len, PROT_READ | PROT_WRITE);
	void *p = mmap(G2H(at), len, prot, flags | MAP_FIXED, fd, off);
	RET(p == MAP_FAILED ? 0xffffffffu : H2G(p));
}
static int w_munmap(uint32_t a, uint32_t len)
{
	// keep the range reserved so the address space stays ours
	return mmap(G2H(a), len, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED ? -1 : 0;
}
static int w_mprotect(uint32_t a, uint32_t len, int prot) { return mprotect(G2H(a & ~0xfffu), len + (a & 0xfff), prot); }
static void t_getrlimit(uint32_t *args, struct hres *r)
{
	struct rlimit rl;
	int rc = getrlimit((int)A(0), &rl);
	if (!rc) { uint64_t *g = G2H(A(1)); g[0] = rl.rlim_cur; g[1] = rl.rlim_max; }
	RET(rc);
}
static void t_setrlimit(uint32_t *args, struct hres *r)
{
	uint64_t *g = G2H(A(1));
	struct rlimit rl = { g[0], g[1] };
	RET(setrlimit((int)A(0), &rl));
}

static void init_task_self(g32 g) { *(uint32_t *)G2H(g) = mach_task_self(); }
static void init_bootstrap(g32 g) { *(uint32_t *)G2H(g) = bootstrap_port; }

static double w_ldexp(double x, int e) { return ldexp(x, e); }
static double w_frexp(double x, uint32_t e) { int ee; double v = frexp(x, &ee); *(int32_t *)G2H(e) = ee; return v; }
static double w_modf(double x, uint32_t ip) { double i; double v = modf(x, &i); memcpy(G2H(ip), &i, 8); return v; }
static float w_modff(float x, uint32_t ip) { float i; float v = modff(x, &i); memcpy(G2H(ip), &i, 4); return v; }
static int w_abs(int x) { return abs(x); }
static int32_t w_labs(int32_t x) { return x < 0 ? -x : x; }
static void w_sincos(double x, uint32_t s, uint32_t c) { double a = sin(x), b = cos(x); memcpy(G2H(s), &a, 8); memcpy(G2H(c), &b, 8); }
static void w_sincosf(float x, uint32_t s, uint32_t c) { float a = sinf(x), b = cosf(x); memcpy(G2H(s), &a, 4); memcpy(G2H(c), &b, 4); }

static struct hfn libc2[] = {
	R("_sscanf", t_sscanf), R("_vsscanf", t_vsscanf), R("_fscanf", t_fscanf),
	R("_stat", t_stat), R("_lstat", t_lstat), R("_fstat", t_fstat),
	R("_stat$INODE64", t_stat64), R("_lstat$INODE64", t_lstat64), R("_fstat$INODE64", t_fstat64),
	G("_open", w_open, "i:pii"), G("_close", close, "i:i"),
	G("_read", w_read, "i:ipu"), G("_write", w_write, "i:ipu"),
	G("_pread", w_pread, "i:ipul"), G("_pwrite", w_pwrite, "i:ipul"),
	G("_lseek", w_lseek, "l:ili"), G("_ftruncate", w_ftruncate, "i:il"),
	G("_access", access, "i:pi"), G("_chdir", chdir, "i:p"), G("_mkdir", mkdir, "i:pi"),
	G("_rmdir", rmdir, "i:p"), G("_rename", rename, "i:pp"), G("_remove", remove, "i:p"),
	G("_dup", dup, "i:i"), G("_dup2", dup2, "i:ii"), G("_pipe", pipe, "i:p"),
	G("_fchmod", fchmod, "i:ii"), G("_chmod", chmod, "i:pi"), G("_ioctl", ioctl, "i:iup"),
	G("_fcntl", fcntl, "i:iii"), G("_isatty", isatty, "i:i"), G("_fsync", fsync, "i:i"),
	G("_umask", umask, "i:i"), G("_symlink", symlink, "i:pp"), G("_readlink", readlink, "i:ppu"),
	R("_getcwd", t_getcwd), R("_realpath", t_realpath),
	R("_opendir", t_opendir), R("_readdir", t_readdir), R("_closedir", t_closedir),
	R("_opendir$INODE64", t_opendir), R("_closedir$INODE64", t_closedir),
	G("_fork", fork, "i:"), G("_getppid", getppid, "i:"), G("_setsid", setsid, "i:"),
	R("_execvp", t_execvp), R("_execv", t_execv), R("_execve", t_execve),
	G("_system", system, "i:p"), R("_popen", t_popen), R("_pclose", t_pclose),
	G("_wait", w_wait, "i:p"), G("_waitpid", w_waitpid, "i:ipi"),
	R("_iconv_open", t_iconv_open), R("_iconv_close", t_iconv_close), R("_iconv", t_iconv),
	R("_libiconv_open", t_iconv_open), R("_libiconv_close", t_iconv_close), R("_libiconv", t_iconv),
	D("__DefaultRuneLocale", 52 + 3 * 1024 + 64, init_runelocale),
	D("__CurrentRuneLocale", 4, init_currentrune),
	G("___maskrune", w_maskrune, "u:iu"), G("___toupper_l", w_toupper_l, "i:i"), G("___tolower_l", w_tolower_l, "i:i"),
	G("_isalpha", isalpha, "i:i"), G("_isdigit", isdigit, "i:i"), G("_isspace", isspace, "i:i"),
	G("_isalnum", isalnum, "i:i"), G("_isupper", isupper, "i:i"), G("_islower", islower, "i:i"),
	G("_isprint", isprint, "i:i"), G("_ispunct", ispunct, "i:i"), G("_isxdigit", isxdigit, "i:i"),
	G("_iswspace", iswspace, "i:i"), G("_towlower", towlower, "i:i"), G("_towupper", towupper, "i:i"),
	G("_rindex", rindex, "p:pi"), G("_index", index, "p:pi"),
	R("_strerror_r", t_strerror_r), R("_strerror", t_strerror),
	G("_strdup", gstrdup, "p:p"),
	G("_wcstol", w_wcstol, "i:ppi"), G("_wcscat", wcscat, "p:pp"), G("_wcsncmp", wcsncmp, "i:ppu"),
	G("_wcschr", wcschr, "p:pi"), G("_wcsrchr", wcsrchr, "p:pi"), G("_wcsstr", wcsstr, "p:pp"),
	G("_wcsncat", wcsncat, "p:ppu"), G("_wcscasecmp", wcscasecmp, "i:pp"), G("_wcsncasecmp", wcsncasecmp, "i:ppu"),
	G("_memset_pattern16", w_memset_pattern16, "v:ppu"), G("_memset_pattern4", w_memset_pattern4, "v:ppu"),
	G("_memset_pattern8", w_memset_pattern8, "v:ppu"),
	R("_Debugger", t_debugger),
	R("_sigaction", t_sigaction), R("_signal", t_signal), R("_sigprocmask", t_sigprocmask),
	R("_pthread_sigmask", t_sigprocmask), R("_sigaltstack", t_ignore0),
	R("_mmap", t_mmap), G("_munmap", w_munmap, "i:uu"), G("_mprotect", w_mprotect, "i:uui"),
	R("_getrlimit", t_getrlimit), R("_setrlimit", t_setrlimit),
	// Mach / launchd: the game uses these for crash reporting and Steam IPC
	D("_mach_task_self_", 4, init_task_self), D("_bootstrap_port", 4, init_bootstrap),
	// Mach IPC: port names and message headers are 32-bit in every process,
	// so these pass straight through (libsteam_api finds Steam this way)
	G("_bootstrap_look_up", bootstrap_look_up, "i:upp"),
	G("_mach_msg", mach_msg, "i:piuuuuu"),
	G("_mach_port_allocate", mach_port_allocate, "i:uup"),
	G("_mach_port_deallocate", mach_port_deallocate, "i:uu"),
	G("_mach_port_mod_refs", mach_port_mod_refs, "i:uuui"),
	G("_mach_port_type", mach_port_type, "i:uup"),
	R("_launch_data_alloc", t_ignore0), R("_launch_data_dict_insert", t_ignore0), R("_launch_data_free", t_ignore0),
	R("_launch_data_new_string", t_ignore0), R("_launch_msg", t_ignore0),
	// math
	G("_sin", sin, "d:d"), G("_cos", cos, "d:d"), G("_tan", tan, "d:d"), G("_asin", asin, "d:d"),
	G("_acos", acos, "d:d"), G("_atan", atan, "d:d"), G("_atan2", atan2, "d:dd"), G("_sqrt", sqrt, "d:d"),
	G("_pow", pow, "d:dd"), G("_exp", exp, "d:d"), G("_log", log, "d:d"), G("_log10", log10, "d:d"),
	G("_log2", log2, "d:d"), G("_floor", floor, "d:d"), G("_ceil", ceil, "d:d"), G("_fmod", fmod, "d:dd"),
	G("_fabs", fabs, "d:d"), G("_round", round, "d:d"), G("_trunc", trunc, "d:d"), G("_hypot", hypot, "d:dd"),
	G("_exp2", exp2, "d:d"), G("_sinh", sinh, "d:d"), G("_cosh", cosh, "d:d"), G("_tanh", tanh, "d:d"),
	G("_ldexp", w_ldexp, "d:di"), G("_frexp", w_frexp, "d:dp"), G("_modf", w_modf, "d:dp"),
	G("_sinf", sinf, "f:f"), G("_cosf", cosf, "f:f"), G("_tanf", tanf, "f:f"), G("_asinf", asinf, "f:f"),
	G("_acosf", acosf, "f:f"), G("_atanf", atanf, "f:f"), G("_atan2f", atan2f, "f:ff"), G("_sqrtf", sqrtf, "f:f"),
	G("_powf", powf, "f:ff"), G("_expf", expf, "f:f"), G("_logf", logf, "f:f"), G("_log10f", log10f, "f:f"),
	G("_floorf", floorf, "f:f"), G("_ceilf", ceilf, "f:f"), G("_fmodf", fmodf, "f:ff"), G("_fabsf", fabsf, "f:f"),
	G("_roundf", roundf, "f:f"), G("_modff", w_modff, "f:fp"), G("_exp2f", exp2f, "f:f"), G("_log2f", log2f, "f:f"),
	G("_lround", lround, "i:d"), G("_lroundf", lroundf, "i:f"), G("_llround", llround, "l:d"),
	G("_abs", w_abs, "i:i"), G("_labs", w_labs, "i:i"),
	G("___sincos", w_sincos, "v:dpp"), G("___sincosf", w_sincosf, "v:fpp"),
};

void thunks_libc2_init(void)
{
	REGISTER(libc2);
	rune_ptr = host_lookup("__DefaultRuneLocale");
}
