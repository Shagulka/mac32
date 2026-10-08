// mac32 — run 32-bit i386 Mac programs on Apple Silicon.
//   mac32 [-v] [-t] [-L dir]... program [args...]
//   mac32 [-v] -L dir --tier0-test dylib
#include "mac32.h"
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>

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

int main(int argc, char **argv)
{
	int i = 1;
	const char *t0 = NULL;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-v")) verbose = 1;
		else if (!strcmp(argv[i], "-t")) trace_calls = 1;
		else if (!strcmp(argv[i], "-L") && i + 1 < argc) macho_add_search_dir(argv[++i]);
		else if (!strcmp(argv[i], "--tier0-test") && i + 1 < argc) t0 = argv[++i];
		else die("unknown option %s", argv[i]);
	}
	low_init();
	cpu_init();
	thunks_init();
	if (t0) return tier0_test(t0);
	if (i >= argc) die("usage: mac32 [-v] [-t] [-L dir] program [args...]");

	setup_process(argc - i, argv + i);
	struct image *img = macho_load(argv[i]);
	macho_run_initializers(img);
	g32 entry = macho_entry(img);
	if (!entry) die("%s has no entry point", argv[i]);
	die("running executables from LC_UNIXTHREAD is next (entry %#x)", entry);
}
