// 32-bit C++ self-test for exceptions and RTTI under mac32.
struct Base { virtual ~Base() {} int b = 1; };
struct Mid : Base { int m = 2; };
struct Other { virtual ~Other() {} int o = 3; };
struct Derived : Mid, Other { int d = 4; };
struct Err { int code; };

static int depth(int n) { if (n == 0) throw Err{42}; return depth(n - 1) + 1; }

extern "C" int test_throw_int() { try { throw 7; } catch (int v) { return v; } return -1; }
extern "C" int test_throw_deep() { try { return depth(10); } catch (const Err &e) { return e.code; } }
extern "C" int test_rethrow()
{
	try { try { throw Err{5}; } catch (...) { throw; } } catch (Err &e) { return e.code; }
	return -1;
}
extern "C" int test_dynamic_cast()
{
	Derived *d = new Derived;
	Base *b = d;
	Other *o = dynamic_cast<Other *>(b);		// cross cast
	Derived *back = dynamic_cast<Derived *>(o);	// down cast
	int ok = o && back == d && o->o == 3 && dynamic_cast<Mid *>(b) != nullptr;
	delete d;
	return ok;
}
