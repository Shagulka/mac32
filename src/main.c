#include <pthread.h>
// mac32 — run 32-bit i386 Mac programs on Apple Silicon.
//   mac32 [-v] [-t] [-L dir]... program [args...]
//   mac32 [-v] -L dir --tier0-test dylib
#include "mac32.h"
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include <time.h>
#include <sys/mman.h>
#include <mach-o/dyld.h>
#include <CoreFoundation/CoreFoundation.h>

int verbose;
extern int trace_calls, unresolved_count;
g32 g_argv, g_envp, g_apple, g_progname;
int g_argc;
extern char **environ;
void set_exe_path(const char *p);

void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[mac32] fatal: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	_exit(1);
}

static g32 guest_strv(char **v, int n)
{
	g32 arr = gcalloc((uint32_t)n + 1, 4);
	for (int i = 0; i < n; i++) ((g32 *)G2H(arr))[i] = gstrdup(v[i]);
	return arr;
}

static void setup_process(int argc, char **argv)
{
	char rp[PATH_MAX];
	if (!realpath(argv[0], rp)) snprintf(rp, sizeof rp, "%s", argv[0]);
	set_exe_path(rp);
	int ne = 0;
	while (environ[ne]) ne++;
	g_argc = argc;
	g_argv = guest_strv(argv, argc);
	g_envp = guest_strv(environ, ne);
	char ex[PATH_MAX + 20];
	snprintf(ex, sizeof ex, "executable_path=%s", rp);
	char *ap[1] = { ex };
	g_apple = guest_strv(ap, 1);
	const char *leaf = strrchr(argv[0], '/');
	g_progname = gstrdup(leaf ? leaf + 1 : argv[0]);
}

static int tier0_test(const char *path)
{
	char *av[1] = { (char *)path };
	setup_process(1, av);
	struct image *img = macho_load(path);
	fprintf(stderr, "[mac32] loaded %s, %d unresolved imports\n", path, unresolved_count);
	macho_run_initializers(img);
	fprintf(stderr, "[mac32] initializers done\n");

	g32 ft = macho_sym(img, "_Plat_FloatTime");
	double t0 = gcall_fp(ft, 0);
	usleep(250000);
	double t1 = gcall_fp(ft, 0);
	fprintf(stderr, "[mac32] Plat_FloatTime: %.6f -> %.6f (slept 0.25 s, delta %.3f)\n", t0, t1, t1 - t0);

	g32 msg = macho_sym(img, "_Msg");
	g32 fmt = gstrdup("Msg from 32-bit tier0: %s %d %.2f\n");
	g32 s = gstrdup("hello");
	double pi = 3.14159;
	uint32_t pw[2];
	memcpy(pw, &pi, 8);
	uint32_t a[5] = { fmt, s, 42, pw[0], pw[1] };
	gcallv(msg, 5, a, NULL);
	fflush(stdout);
	return 0;
}

// Foo.app -> Foo.app/Contents/MacOS/<CFBundleExecutable>
static int bundle_executable(const char *path, char *out, size_t n)
{
	size_t l = strlen(path);
	while (l && path[l - 1] == '/') l--;
	if (l < 4 || strncmp(path + l - 4, ".app", 4)) return 0;
	char plist[PATH_MAX];
	snprintf(plist, sizeof plist, "%.*s/Contents/Info.plist", (int)l, path);
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)plist, (CFIndex)strlen(plist), false);
	CFReadStreamRef st = CFReadStreamCreateWithFile(NULL, url);
	CFRelease(url);
	if (!st || !CFReadStreamOpen(st)) die("%s: cannot read Contents/Info.plist", path);
	CFPropertyListRef pl = CFPropertyListCreateWithStream(NULL, st, 0, kCFPropertyListImmutable, NULL, NULL);
	CFReadStreamClose(st);
	CFRelease(st);
	CFStringRef name = pl ? CFDictionaryGetValue(pl, CFSTR("CFBundleExecutable")) : NULL;
	char nm[256];
	if (!name || !CFStringGetCString(name, nm, sizeof nm, kCFStringEncodingUTF8)) die("%s: no CFBundleExecutable", path);
	snprintf(out, n, "%.*s/Contents/MacOS/%s", (int)l, path, nm);
	CFRelease(pl);
	return 1;
}

// Our 32-bit runtime library (guest/libmac32rt.dylib) fills in what the old
// libstdc++/libgcc provided as 32-bit code. It lives next to the mac32 binary.
static void load_runtime(void)
{
	char self[PATH_MAX], exe[PATH_MAX], rt[PATH_MAX];
	uint32_t n = sizeof self;
	if (_NSGetExecutablePath(self, &n)) die("cannot find own path");
	if (!realpath(self, exe)) snprintf(exe, sizeof exe, "%s", self);	// wrappers run a symlink to us
	char *slash = strrchr(exe, '/');
	*slash = 0;
	snprintf(rt, sizeof rt, "%s/guest/libmac32rt.dylib", exe);
	if (!macho_load(rt)) die("cannot load %s (run make)", rt);
	snprintf(rt, sizeof rt, "%s/guest/libmac32cxx.dylib", exe);	// C++ exceptions + RTTI
	if (!macho_load(rt)) die("cannot load %s (run make)", rt);
}

// Enter an LC_UNIXTHREAD executable at "start" with the stack the kernel
// would build: esp -> argc, argv[], 0, envp[], 0, apple[], 0. A 3-byte shim
// drops the return address gcall pushes, then jumps to start. start never
// returns; it calls exit().
static void run_entry(g32 entry)
{
	uint32_t na = 0, ne = 0, np = 0;
	while (((g32 *)G2H(g_argv))[na]) na++;
	while (((g32 *)G2H(g_envp))[ne]) ne++;
	while (((g32 *)G2H(g_apple))[np]) np++;
	uint32_t n = 1 + na + 1 + ne + 1 + np + 1;
	uint32_t *v = calloc(n, 4), k = 0;
	v[k++] = na;
	for (uint32_t j = 0; j < na; j++) v[k++] = ((g32 *)G2H(g_argv))[j];
	k++;
	for (uint32_t j = 0; j < ne; j++) v[k++] = ((g32 *)G2H(g_envp))[j];
	k++;
	for (uint32_t j = 0; j < np; j++) v[k++] = ((g32 *)G2H(g_apple))[j];
	k++;
	g32 shim = low_pages(16, PROT_READ | PROT_WRITE | PROT_EXEC);
	uint8_t *p = G2H(shim);
	int32_t rel = (int32_t)(entry - (shim + 8));
	p[0] = 0x83; p[1] = 0xc4; p[2] = 0x04;		// add $4, %esp
	p[3] = 0xe9; memcpy(p + 4, &rel, 4);		// jmp start
	LOG("entering %#x with %u args\n", entry, na);
	gcallv(shim, (int)n, v, NULL);
	die("start returned");
}

static void *churn_thread(void *fn) { gcall((g32)(uintptr_t)fn, 1, 1); return NULL; }

int main(int argc, char **argv)
{
	int i = 1;
	const char *t0 = NULL, *deps = NULL, *selftest = NULL;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else if (!strcmp(argv[i], "-t")) trace_calls = 1;
		else if (!strcmp(argv[i], "-L") && i + 1 < argc) macho_add_search_dir(argv[++i]);
		else if (!strcmp(argv[i], "--tier0-test") && i + 1 < argc) t0 = argv[++i];
		else if (!strcmp(argv[i], "--deps") && i + 1 < argc) deps = argv[++i];
		else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) selftest = argv[++i];
		else if (!strncmp(argv[i], "-psn_", 5)) break;
		else die("unknown option %s", argv[i]);
	}
	// Finder starts apps with no arguments (older macOS added -psn_...): pick one
	if (i < argc && !strncmp(argv[i], "-psn_", 5)) i++;
	if (i >= argc && !t0 && !deps && !selftest) {
		extern void launcher_main(void);
		launcher_main();	// returns only if nothing was chosen
		return 0;
	}
	low_init();
	cpu_init();
	thunks_init();
	load_runtime();
	if (t0) return tier0_test(t0);
	if (selftest) {
		char *av[1] = { (char *)selftest };
		setup_process(1, av);
		struct image *img = macho_load(selftest);
		int f = (int)gcall(macho_sym(img, "_test_flat"), 1, 5);
		fprintf(stderr, "test_flat(5) = %d (want 6)\n", f);
		int n = (int)gcall(macho_sym(img, "_test_nested"), 1, 5);
		fprintf(stderr, "test_nested(5) = %d (want 13)\n", n);
		int d = (int)gcall(macho_sym(img, "_test_deep"), 1, 3);
		fprintf(stderr, "test_deep(3) = %d (want 104)\n", d);
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		gcall(macho_sym(img, "_test_bench"), 1, 1000000);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double host_ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e6;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		gcall(macho_sym(img, "_test_bench_guest"), 1, 1000000);
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double guest_ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e6;
		fprintf(stderr, "guest->host call: %.0f ns each; guest->guest call: %.1f ns each\n", host_ns, guest_ns);
		// address space reuse: 40 GB of big blocks, a churning mixed heap,
		// and 2000 short threads with 8 MB guest stacks each
		for (int k = 0; k < 40000; k++) gfree(gmalloc(1 << 20));
		static g32 keep[1024];
		for (int k = 0; k < 1000000; k++) {
			int j = (int)(arc4random() % 1024);
			gfree(keep[j]);
			uint32_t sz = arc4random() % 4 ? arc4random() % 4096 : arc4random() % (4 << 20);
			keep[j] = gmalloc(sz);
			*(uint8_t *)G2H(keep[j]) = 1;
		}
		for (int k = 0; k < 1024; k++) { gfree(keep[k]); keep[k] = 0; }
		g32 flat = macho_sym(img, "_test_flat");
		for (int k = 0; k < 2000; k++) {
			pthread_t t;
			pthread_create(&t, NULL, churn_thread, (void *)(uintptr_t)flat);
			pthread_join(t, NULL);
		}
		fprintf(stderr, "allocator churn: ok\n");
		char ex[PATH_MAX];
		snprintf(ex, sizeof ex, "%s", selftest);
		strcpy(strrchr(ex, '/') + 1, "extest.dylib");
		struct image *xi = macho_load(ex);
		int e1 = (int)gcall(macho_sym(xi, "_test_throw_int"), 0);
		int e2 = (int)gcall(macho_sym(xi, "_test_throw_deep"), 0);
		int e3 = (int)gcall(macho_sym(xi, "_test_rethrow"), 0);
		int e4 = (int)gcall(macho_sym(xi, "_test_dynamic_cast"), 0);
		fprintf(stderr, "exceptions: int %d (7), deep %d (42), rethrow %d (5), dynamic_cast %d (1)\n", e1, e2, e3, e4);
		if (!(e1 == 7 && e2 == 42 && e3 == 5 && e4 == 1)) return 1;
		return !(f == 6 && n == 13 && d == 104);
	}
	if (deps) {	// load the tree, report what is missing, run nothing
		char *av[1] = { (char *)deps };
		setup_process(1, av);
		macho_load(deps);
		fprintf(stderr, "[mac32] %d unresolved imports\n", unresolved_count);
		return 0;
	}
	if (i >= argc) die("usage: mac32 [-v] [-t] [-L dir] program-or-App.app [args...]");

	// an .app bundle: run its CFBundleExecutable, look for libraries in its
	// Frameworks folder, and make it the guest's main bundle
	static char exe[PATH_MAX];
	if (bundle_executable(argv[i], exe, sizeof exe)) {
		set_guest_bundle(argv[i]);
		argv[i] = exe;
	}
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", argv[i]);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		char fw[PATH_MAX + 16];
		snprintf(fw, sizeof fw, "%s/../Frameworks", dir);
		macho_add_search_dir(fw);
		macho_add_search_dir(dir);
	}

	setup_process(argc - i, argv + i);
	struct image *img = macho_load(argv[i]);
	low_exe_placed(0);	// a position-independent program: nothing is fixed below EXE_ZONE
	g32 entry = macho_entry(img), mainfn = macho_main(img);
	if (!entry && !mainfn) die("%s has no entry point", argv[i]);
	// dyld fills in the crt1 globals an executable defines
	g32 a;
	if ((a = macho_sym(img, "_NXArgc"))) *(int32_t *)G2H(a) = g_argc;
	if ((a = macho_sym(img, "_NXArgv"))) *(g32 *)G2H(a) = g_argv;
	if ((a = macho_sym(img, "_environ"))) *(g32 *)G2H(a) = g_envp;
	if ((a = macho_sym(img, "___progname"))) *(g32 *)G2H(a) = g_progname;
	macho_run_initializers(img);
	if (mainfn) {		// LC_MAIN: dyld calls main(argc, argv, envp, apple) and exits with its result
		LOG("calling main at %#x\n", mainfn);
		int rc = (int)gcall(mainfn, 4, (uint32_t)g_argc, g_argv, g_envp, g_apple);
		extern void run_guest_atexit(void);
		run_guest_atexit();
		fflush(NULL);
		exit(rc);
	}
	run_entry(entry);
}
