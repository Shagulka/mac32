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

// CoreGraphics types cross with float -> double conversion (thunks.c codes g P R A)
typedef struct { float x, y; } CGPoint;
typedef struct { float w, h; } CGSize;
typedef struct { CGPoint o; CGSize s; } CGRect;
typedef struct { float a, b, c, d, tx, ty; } CGAffineTransform;
CGRect CGRectIntegral(CGRect r);
float CGRectGetMaxX(CGRect r);
CGAffineTransform CGAffineTransformMakeScale(float sx, float sy);
CGPoint CGPointApplyAffineTransform(CGPoint p, CGAffineTransform t);
// 1 if every result is right
int test_cg(void)
{
	CGRect r = { { 1.25f, 2.5f }, { 3.5f, 4.25f } };
	CGRect i = CGRectIntegral(r);			// {1, 2, 4, 5}: origin floored, far edge ceiled
	float mx = CGRectGetMaxX(r);			// 4.75
	CGAffineTransform t = CGAffineTransformMakeScale(2, 3);
	CGPoint p = CGPointApplyAffineTransform((CGPoint){ 1.5f, -2 }, t);	// {3, -6}
	return i.o.x == 1 && i.o.y == 2 && i.s.w == 4 && i.s.h == 5 && mx == 4.75f &&
	       t.a == 2 && t.d == 3 && t.b == 0 && t.tx == 0 && p.x == 3 && p.y == -6;
}
