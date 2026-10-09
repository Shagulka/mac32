// 32-bit self-test for the mode switches: guest -> host -> guest -> host.
int mac32_host_add1(int);
int mac32_host_callback(int (*f)(int), int x);

static int inner(int x) { return mac32_host_add1(x) * 2; }
int test_flat(int x) { return mac32_host_add1(x); }
int test_nested(int x) { return mac32_host_callback(inner, x) + 1; }
int test_deep(int x);
static int deeper(int x) { return x <= 0 ? mac32_host_add1(100) : test_deep(x - 1) + 1; }
int test_deep(int x) { return mac32_host_callback(deeper, x); }

// timing: n host calls in a row
int test_bench(int n) { int s = 0; for (int i = 0; i < n; i++) s += mac32_host_add1(i); return s; }
static int plain_add1(int x) { return x + 1; }
int test_bench_guest(int n) { int s = 0; int (*volatile f)(int) = plain_add1; for (int i = 0; i < n; i++) s += f(i); return s; }
