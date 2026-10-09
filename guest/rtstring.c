// GNU libstdc++ (GCC 4.x) copy-on-write std::string and std::wstring, the
// out-of-line members 32-bit game code calls. Written from the ABI: the
// object is one pointer to the characters, preceded by a 12-byte header
// {length, capacity, refcount}. refcount -1 = leaked (unshareable), 0 = one
// owner, n>0 = shared by n+1. The empty string is a static shared header.
// One implementation serves both widths: CS is the character size (1 or 4).

void mac32_guest_fatal(const char *what, const char *detail);
void *malloc(unsigned long);
void free(void *);
void *memcpy(void *, const void *, unsigned long);
void *memmove(void *, const void *, unsigned long);
int memcmp(const void *, const void *, unsigned long);
unsigned long strlen(const char *);

typedef unsigned long size_t;
#define NPOS ((size_t)-1)
typedef struct rep { size_t len, cap; int ref; } rep;

unsigned long _ZNSs4_Rep20_S_empty_rep_storageE[4];
unsigned long _ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE[4];
const char _ZNSs4_Rep11_S_terminalE = 0;
const int _ZNSbIwSt11char_traitsIwESaIwEE4_Rep11_S_terminalE = 0;

static rep *EMPTY(int cs)
{
	return (rep *)(cs == 1 ? _ZNSs4_Rep20_S_empty_rep_storageE : _ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE);
}
#define R(p) ((rep *)*(char **)(p) - 1)
#define D(p) (*(char **)(p))
static char *data_of(rep *r) { return (char *)(r + 1); }
static int shared(rep *r) { return r->ref > 0; }

static void memset_z(char *p, int cs) { for (int i = 0; i < cs; i++) p[i] = 0; }

static void set_len(rep *r, size_t n, int cs)
{
	if (r == EMPTY(cs)) return;
	r->ref = 0;
	r->len = n;
	memset_z(data_of(r) + n * cs, cs);
}

static rep *create(size_t cap, size_t old, int cs)
{
	size_t max = ((NPOS - sizeof(rep)) / cs - 1) / 4;
	if (cap > max) mac32_guest_fatal("std::length_error", "basic_string::_S_create");
	if (cap > old && cap < 2 * old) cap = 2 * old;
	size_t size = (cap + 1) * cs + sizeof(rep);
	if (size + 16 > 4096 && cap > old) {
		cap += (4096 - (size + 16) % 4096) / cs;
		if (cap > max) cap = max;
		size = (cap + 1) * cs + sizeof(rep);
	}
	rep *r = malloc(size);
	r->cap = cap;
	r->ref = 0;
	r->len = 0;
	return r;
}
static void dispose(rep *r, int cs)
{
	if (r != EMPTY(cs) && __sync_fetch_and_add(&r->ref, -1) <= 0) free(r);
}
// share if possible, else copy (for copy-construct / assign)
static char *grab(rep *r, int cs)
{
	if (r->ref >= 0) {
		if (r != EMPTY(cs)) __sync_fetch_and_add(&r->ref, 1);
		return data_of(r);
	}
	rep *n = create(r->len, 0, cs);
	memcpy(data_of(n), data_of(r), r->len * cs);
	set_len(n, r->len, cs);
	return data_of(n);
}
// replace [pos, pos+len1) with len2 uninitialized characters
static void mutate(void *s, size_t pos, size_t len1, size_t len2, int cs)
{
	rep *r = R(s);
	size_t old = r->len, nsz = old + len2 - len1, tail = old - pos - len1;
	if (nsz > r->cap || shared(r)) {
		rep *n = create(nsz, r->cap, cs);
		if (pos) memcpy(data_of(n), D(s), pos * cs);
		if (tail) memcpy(data_of(n) + (pos + len2) * cs, D(s) + (pos + len1) * cs, tail * cs);
		dispose(r, cs);
		D(s) = data_of(n);
	} else if (tail && len1 != len2) {
		memmove(D(s) + (pos + len2) * cs, D(s) + (pos + len1) * cs, tail * cs);
	}
	set_len(R(s), nsz, cs);
}
static void reserve(void *s, size_t res, int cs)
{
	rep *r = R(s);
	if (res != r->cap || shared(r)) {
		if (res < r->len) res = r->len;
		rep *n = create(res, r->cap, cs);
		memcpy(data_of(n), D(s), r->len * cs);
		set_len(n, r->len, cs);
		dispose(r, cs);
		D(s) = data_of(n);
	}
}
// replace [pos, pos+n1) with n2 chars from src (src may point into s)
static void *replace(void *s, size_t pos, size_t n1, const void *src, size_t n2, int cs)
{
	size_t sz = R(s)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::replace");
	if (n1 > sz - pos) n1 = sz - pos;
	char tmp[512], *t = tmp;
	if (n2 * cs > sizeof tmp) t = malloc(n2 * cs);
	memcpy(t, src, n2 * cs);
	mutate(s, pos, n1, n2, cs);
	memcpy(D(s) + pos * cs, t, n2 * cs);
	if (t != tmp) free(t);
	return s;
}
static void fill(char *p, size_t n, unsigned c, int cs)
{
	for (size_t i = 0; i < n; i++) {
		if (cs == 1) p[i] = (char)c;
		else ((unsigned *)p)[i] = c;
	}
}
static size_t clen(const void *p, int cs)
{
	if (cs == 1) return strlen(p);
	size_t n = 0;
	while (((const unsigned *)p)[n]) n++;
	return n;
}
static unsigned at(const void *p, size_t i, int cs) { return cs == 1 ? ((const unsigned char *)p)[i] : ((const unsigned *)p)[i]; }
static int cmp(const void *a, const void *b, size_t n, int cs)
{
	if (cs == 1) return memcmp(a, b, n);
	for (size_t i = 0; i < n; i++) {
		unsigned x = at(a, i, cs), y = at(b, i, cs);
		if (x != y) return x < y ? -1 : 1;
	}
	return 0;
}
static int cmp_len(int r, size_t a, size_t b)
{
	if (r) return r;
	long d = (long)(a - b);
	return d;
}
static void construct(void *s, const void *p, size_t n, int cs)
{
	if (!n) { D(s) = data_of(EMPTY(cs)); return; }
	rep *r = create(n, 0, cs);
	memcpy(data_of(r), p, n * cs);
	set_len(r, n, cs);
	D(s) = data_of(r);
}
static size_t find(const void *s, const void *p, size_t pos, size_t n, int cs)
{
	size_t sz = R(s)->len;
	if (!n) return pos <= sz ? pos : NPOS;
	for (; pos + n <= sz; pos++)
		if (!cmp(D(s) + pos * cs, p, n, cs)) return pos;
	return NPOS;
}
static size_t find_c(const void *s, unsigned c, size_t pos, int cs)
{
	for (size_t sz = R(s)->len; pos < sz; pos++) if (at(D(s), pos, cs) == c) return pos;
	return NPOS;
}
static int in_set(unsigned c, const void *set, size_t n, int cs)
{
	for (size_t i = 0; i < n; i++) if (at(set, i, cs) == c) return 1;
	return 0;
}

// ---- std::string (Ss) ----------------------------------------------------------
#define S1 1
void _ZNSs4_Rep10_M_destroyERKSaIcE(rep *r, const void *a) { (void)a; free(r); }
void _ZNSs4_Rep10_M_disposeERKSaIcE(rep *r, const void *a) { (void)a; dispose(r, S1); }
rep *_ZNSs4_Rep9_S_createEmmRKSaIcE(size_t cap, size_t old, const void *a) { (void)a; return create(cap, old, S1); }
void _ZNSs12_M_leak_hardEv(void *s)
{
	if (R(s) == EMPTY(S1)) return;
	if (shared(R(s))) mutate(s, 0, 0, 0, S1);
	R(s)->ref = -1;
}
void _ZNSs9_M_mutateEmmm(void *s, size_t pos, size_t l1, size_t l2) { mutate(s, pos, l1, l2, S1); }
void _ZNSsC1Ev(void *s) { D(s) = data_of(EMPTY(S1)); }
void _ZNSsC2Ev(void *s) { D(s) = data_of(EMPTY(S1)); }
void _ZNSsC1EPKcRKSaIcE(void *s, const char *p, const void *a) { (void)a; construct(s, p, strlen(p), S1); }
void _ZNSsC2EPKcRKSaIcE(void *s, const char *p, const void *a) { (void)a; construct(s, p, strlen(p), S1); }
void _ZNSsC1EPKcmRKSaIcE(void *s, const char *p, size_t n, const void *a) { (void)a; construct(s, p, n, S1); }
void _ZNSsC1ERKSs(void *s, const void *o) { D(s) = grab(R(o), S1); }
void _ZNSsC2ERKSs(void *s, const void *o) { D(s) = grab(R(o), S1); }
void _ZNSsC1ERKSsmm(void *s, const void *o, size_t pos, size_t n)
{
	size_t sz = R(o)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::basic_string");
	if (n > sz - pos) n = sz - pos;
	construct(s, D(o) + pos, n, S1);
}
void _ZNSsC1EmcRKSaIcE(void *s, size_t n, char c, const void *a)
{
	(void)a;
	if (!n) { D(s) = data_of(EMPTY(S1)); return; }
	rep *r = create(n, 0, S1);
	fill(data_of(r), n, (unsigned char)c, S1);
	set_len(r, n, S1);
	D(s) = data_of(r);
}
void _ZNSsD1Ev(void *s) { dispose(R(s), S1); }
void _ZNSsD2Ev(void *s) { dispose(R(s), S1); }
void *_ZNSs6assignEPKcm(void *s, const char *p, size_t n) { return replace(s, 0, R(s)->len, p, n, S1); }
void *_ZNSs6assignEPKc(void *s, const char *p) { return replace(s, 0, R(s)->len, p, strlen(p), S1); }
void *_ZNSs6assignERKSs(void *s, const void *o)
{
	if (R(s) != R(o)) {
		char *d = grab(R(o), S1);
		dispose(R(s), S1);
		D(s) = d;
	}
	return s;
}
void *_ZNSsaSERKSs(void *s, const void *o) { return _ZNSs6assignERKSs(s, o); }
void *_ZNSsaSEPKc(void *s, const char *p) { return _ZNSs6assignEPKc(s, p); }
void *_ZNSsaSEc(void *s, char c) { return replace(s, 0, R(s)->len, &c, 1, S1); }
void *_ZNSs6appendEPKcm(void *s, const char *p, size_t n) { return n ? replace(s, R(s)->len, 0, p, n, S1) : s; }
void *_ZNSs6appendEPKc(void *s, const char *p) { return _ZNSs6appendEPKcm(s, p, strlen(p)); }
void *_ZNSs6appendERKSs(void *s, const void *o) { return _ZNSs6appendEPKcm(s, D(o), R(o)->len); }
void *_ZNSs6appendERKSsmm(void *s, const void *o, size_t pos, size_t n)
{
	size_t sz = R(o)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::append");
	if (n > sz - pos) n = sz - pos;
	return _ZNSs6appendEPKcm(s, D(o) + pos, n);
}
void *_ZNSs6appendEmc(void *s, size_t n, char c)
{
	if (!n) return s;
	size_t len = R(s)->len;
	mutate(s, len, 0, n, S1);
	fill(D(s) + len, n, (unsigned char)c, S1);
	return s;
}
void *_ZNSspLEPKc(void *s, const char *p) { return _ZNSs6appendEPKc(s, p); }
void *_ZNSspLERKSs(void *s, const void *o) { return _ZNSs6appendERKSs(s, o); }
void *_ZNSspLEc(void *s, char c) { return _ZNSs6appendEmc(s, 1, c); }
void _ZNSs9push_backEc(void *s, char c) { _ZNSs6appendEmc(s, 1, c); }
void _ZNSs6resizeEmc(void *s, size_t n, char c)
{
	size_t sz = R(s)->len;
	if (n > sz) _ZNSs6appendEmc(s, n - sz, c);
	else if (n < sz) mutate(s, n, sz - n, 0, S1);
}
void _ZNSs6resizeEm(void *s, size_t n) { _ZNSs6resizeEmc(s, n, 0); }
void _ZNSs7reserveEm(void *s, size_t n) { reserve(s, n, S1); }
void *_ZNSs7replaceEmmPKcm(void *s, size_t pos, size_t n1, const char *p, size_t n2) { return replace(s, pos, n1, p, n2, S1); }
void *_ZNSs7replaceEmmPKc(void *s, size_t pos, size_t n1, const char *p) { return replace(s, pos, n1, p, strlen(p), S1); }
void *_ZNSs7replaceEmmRKSs(void *s, size_t pos, size_t n1, const void *o) { return replace(s, pos, n1, D(o), R(o)->len, S1); }
void *_ZNSs14_M_replace_auxEmmmc(void *s, size_t pos, size_t n1, size_t n2, char c)
{
	size_t sz = R(s)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::_M_replace_aux");
	if (n1 > sz - pos) n1 = sz - pos;
	mutate(s, pos, n1, n2, S1);
	fill(D(s) + pos, n2, (unsigned char)c, S1);
	return s;
}
void *_ZNSs7replaceEmmmc(void *s, size_t pos, size_t n1, size_t n2, char c) { return _ZNSs14_M_replace_auxEmmmc(s, pos, n1, n2, c); }
void *_ZNSs6insertEmPKcm(void *s, size_t pos, const char *p, size_t n) { return replace(s, pos, 0, p, n, S1); }
void *_ZNSs5eraseEmm(void *s, size_t pos, size_t n)
{
	size_t sz = R(s)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::erase");
	if (n > sz - pos) n = sz - pos;
	mutate(s, pos, n, 0, S1);
	return s;
}
void _ZNSs5clearEv(void *s) { mutate(s, 0, R(s)->len, 0, S1); }
void _ZNSs4swapERSs(void *s, void *o) { char *t = D(s); D(s) = D(o); D(o) = t; }
size_t _ZNKSs4findEPKcmm(const void *s, const char *p, size_t pos, size_t n) { return find(s, p, pos, n, S1); }
size_t _ZNKSs4findEPKcm(const void *s, const char *p, size_t pos) { return find(s, p, pos, strlen(p), S1); }
size_t _ZNKSs4findERKSsm(const void *s, const void *o, size_t pos) { return find(s, D(o), pos, R(o)->len, S1); }
size_t _ZNKSs4findEcm(const void *s, char c, size_t pos) { return find_c(s, (unsigned char)c, pos, S1); }
size_t _ZNKSs5rfindEcm(const void *s, char c, size_t pos)
{
	size_t sz = R(s)->len;
	if (!sz) return NPOS;
	if (pos > sz - 1) pos = sz - 1;
	for (size_t i = pos + 1; i-- > 0;) if (D(s)[i] == c) return i;
	return NPOS;
}
size_t _ZNKSs5rfindEPKcmm(const void *s, const char *p, size_t pos, size_t n)
{
	size_t sz = R(s)->len;
	if (n > sz) return NPOS;
	if (pos > sz - n) pos = sz - n;
	for (size_t i = pos + 1; i-- > 0;) if (!memcmp(D(s) + i, p, n)) return i;
	return NPOS;
}
size_t _ZNKSs13find_first_ofEPKcmm(const void *s, const char *p, size_t pos, size_t n)
{
	for (size_t sz = R(s)->len; pos < sz; pos++) if (in_set((unsigned char)D(s)[pos], p, n, S1)) return pos;
	return NPOS;
}
size_t _ZNKSs13find_first_ofEcm(const void *s, char c, size_t pos) { return find_c(s, (unsigned char)c, pos, S1); }
size_t _ZNKSs17find_first_not_ofEPKcmm(const void *s, const char *p, size_t pos, size_t n)
{
	for (size_t sz = R(s)->len; pos < sz; pos++) if (!in_set((unsigned char)D(s)[pos], p, n, S1)) return pos;
	return NPOS;
}
size_t _ZNKSs17find_first_not_ofEPKcm(const void *s, const char *p, size_t pos) { return _ZNKSs17find_first_not_ofEPKcmm(s, p, pos, strlen(p)); }
size_t _ZNKSs17find_first_not_ofEcm(const void *s, char c, size_t pos) { return _ZNKSs17find_first_not_ofEPKcmm(s, &c, pos, 1); }
size_t _ZNKSs12find_last_ofEPKcmm(const void *s, const char *p, size_t pos, size_t n)
{
	size_t sz = R(s)->len;
	if (!sz || !n) return NPOS;
	if (pos > sz - 1) pos = sz - 1;
	for (size_t i = pos + 1; i-- > 0;) if (in_set((unsigned char)D(s)[i], p, n, S1)) return i;
	return NPOS;
}
size_t _ZNKSs12find_last_ofEPKcm(const void *s, const char *p, size_t pos) { return _ZNKSs12find_last_ofEPKcmm(s, p, pos, strlen(p)); }
size_t _ZNKSs12find_last_ofEcm(const void *s, char c, size_t pos) { return _ZNKSs12find_last_ofEPKcmm(s, &c, pos, 1); }
size_t _ZNKSs16find_last_not_ofEPKcmm(const void *s, const char *p, size_t pos, size_t n)
{
	size_t sz = R(s)->len;
	if (!sz) return NPOS;
	if (pos > sz - 1) pos = sz - 1;
	for (size_t i = pos + 1; i-- > 0;) if (!in_set((unsigned char)D(s)[i], p, n, S1)) return i;
	return NPOS;
}
int _ZNKSs7compareEPKc(const void *s, const char *p)
{
	size_t a = R(s)->len, b = strlen(p);
	return cmp_len(memcmp(D(s), p, a < b ? a : b), a, b);
}
int _ZNKSs7compareERKSs(const void *s, const void *o)
{
	size_t a = R(s)->len, b = R(o)->len;
	return cmp_len(memcmp(D(s), D(o), a < b ? a : b), a, b);
}
int _ZNKSs7compareEmmRKSs(const void *s, size_t pos, size_t n, const void *o)
{
	size_t sz = R(s)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::compare");
	if (n > sz - pos) n = sz - pos;
	size_t b = R(o)->len;
	return cmp_len(memcmp(D(s) + pos, D(o), n < b ? n : b), n, b);
}
int _ZNKSs7compareEmmPKc(const void *s, size_t pos, size_t n, const char *p)
{
	size_t sz = R(s)->len, b = strlen(p);
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::compare");
	if (n > sz - pos) n = sz - pos;
	return cmp_len(memcmp(D(s) + pos, p, n < b ? n : b), n, b);
}
int _ZNKSs7compareEmmPKcm(const void *s, size_t pos, size_t n, const char *p, size_t b)
{
	size_t sz = R(s)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::compare");
	if (n > sz - pos) n = sz - pos;
	return cmp_len(memcmp(D(s) + pos, p, n < b ? n : b), n, b);
}
size_t _ZNKSs4copyEPcmm(const void *s, char *out, size_t n, size_t pos)
{
	size_t sz = R(s)->len;
	if (pos > sz) mac32_guest_fatal("std::out_of_range", "basic_string::copy");
	if (n > sz - pos) n = sz - pos;
	memcpy(out, D(s) + pos, n);
	return n;
}
// substr returns by value: hidden result pointer first
void *_ZNKSs6substrEmm(void *ret, const void *s, size_t pos, size_t n) { _ZNSsC1ERKSsmm(ret, s, pos, n); return ret; }
size_t _ZNKSs4sizeEv(const void *s) { return R(s)->len; }
size_t _ZNKSs6lengthEv(const void *s) { return R(s)->len; }
int _ZNKSs5emptyEv(const void *s) { return R(s)->len == 0; }
const char *_ZNKSs5c_strEv(const void *s) { return D(s); }
const char *_ZNKSs4dataEv(const void *s) { return D(s); }
const char *_ZNKSsixEm(const void *s, size_t i) { return D(s) + i; }
char *_ZNSsixEm(void *s, size_t i) { _ZNSs12_M_leak_hardEv(s); return D(s) + i; }
const char *_ZNKSs2atEm(const void *s, size_t i)
{
	if (i >= R(s)->len) mac32_guest_fatal("std::out_of_range", "basic_string::at");
	return D(s) + i;
}
char *_ZNSs2atEm(void *s, size_t i)
{
	if (i >= R(s)->len) mac32_guest_fatal("std::out_of_range", "basic_string::at");
	_ZNSs12_M_leak_hardEv(s);
	return D(s) + i;
}
char *_ZNSs5beginEv(void *s) { _ZNSs12_M_leak_hardEv(s); return D(s); }
char *_ZNSs3endEv(void *s) { _ZNSs12_M_leak_hardEv(s); return D(s) + R(s)->len; }

// std::allocator<char/wchar_t>: stateless
void _ZNSaIcEC1Ev(void *a) { (void)a; }
void _ZNSaIcEC2Ev(void *a) { (void)a; }
void _ZNSaIcEC1ERKS_(void *a, const void *b) { (void)a; (void)b; }
void _ZNSaIcEC2ERKS_(void *a, const void *b) { (void)a; (void)b; }
void _ZNSaIcED1Ev(void *a) { (void)a; }
void _ZNSaIcED2Ev(void *a) { (void)a; }
void _ZNSaIwEC1Ev(void *a) { (void)a; }
void _ZNSaIwED1Ev(void *a) { (void)a; }

// __gnu_cxx atomics used by inline string code
int _ZN9__gnu_cxx18__exchange_and_addEPVii(volatile int *p, int v) { return __sync_fetch_and_add(p, v); }
void _ZN9__gnu_cxx12__atomic_addEPVii(volatile int *p, int v) { __sync_fetch_and_add(p, v); }

// ---- std::wstring (SbIw...) ---------------------------------------------------
#define W 4
#define WS(name) _ZNSbIwSt11char_traitsIwESaIwEE##name
#define WK(name) _ZNKSbIwSt11char_traitsIwESaIwEE##name
void WS(4_Rep10_M_destroyERKS1_)(rep *r, const void *a) { (void)a; free(r); }
void WS(4_Rep10_M_disposeERKS1_)(rep *r, const void *a) { (void)a; dispose(r, W); }
rep *WS(4_Rep9_S_createEmmRKS1_)(size_t cap, size_t old, const void *a) { (void)a; return create(cap, old, W); }
void WS(9_M_mutateEmmm)(void *s, size_t pos, size_t l1, size_t l2) { mutate(s, pos, l1, l2, W); }
void WS(12_M_leak_hardEv)(void *s)
{
	if (R(s) == EMPTY(W)) return;
	if (shared(R(s))) mutate(s, 0, 0, 0, W);
	R(s)->ref = -1;
}
void WS(C1EPKwRKS1_)(void *s, const unsigned *p, const void *a) { (void)a; construct(s, p, clen(p, W), W); }
void WS(C1EPKwmRKS1_)(void *s, const unsigned *p, size_t n, const void *a) { (void)a; construct(s, p, n, W); }
void WS(C1ERKS2_)(void *s, const void *o) { D(s) = grab(R(o), W); }
void WS(D1Ev)(void *s) { dispose(R(s), W); }
void *WS(6assignEPKwm)(void *s, const unsigned *p, size_t n) { return replace(s, 0, R(s)->len, p, n, W); }
void *WS(6assignERKS2_)(void *s, const void *o)
{
	if (R(s) != R(o)) {
		char *d = grab(R(o), W);
		dispose(R(s), W);
		D(s) = d;
	}
	return s;
}
void *WS(6appendEPKwm)(void *s, const unsigned *p, size_t n) { return n ? replace(s, R(s)->len, 0, p, n, W) : s; }
void *WS(6appendERKS2_)(void *s, const void *o) { return WS(6appendEPKwm)(s, (const unsigned *)D(o), R(o)->len); }
void WS(7reserveEm)(void *s, size_t n) { reserve(s, n, W); }
size_t WK(4findEPKwmm)(const void *s, const unsigned *p, size_t pos, size_t n) { return find(s, p, pos, n, W); }
size_t WK(4findEwm)(const void *s, unsigned c, size_t pos) { return find_c(s, c, pos, W); }
int WK(7compareEPKw)(const void *s, const unsigned *p)
{
	size_t a = R(s)->len, b = clen(p, W);
	return cmp_len(cmp(D(s), p, a < b ? a : b, W), a, b);
}
void WS(C1Ev)(void *s) { D(s) = data_of(EMPTY(W)); }
void WS(C2Ev)(void *s) { D(s) = data_of(EMPTY(W)); }
void WS(D2Ev)(void *s) { dispose(R(s), W); }
void *WS(aSERKS2_)(void *s, const void *o) { return WS(6assignERKS2_)(s, o); }
void *WS(pLERKS2_)(void *s, const void *o) { return WS(6appendERKS2_)(s, o); }
void *WS(pLEPKw)(void *s, const unsigned *p) { return WS(6appendEPKwm)(s, p, clen(p, W)); }
void *WS(pLEw)(void *s, unsigned c) { return WS(6appendEPKwm)(s, &c, 1); }
void WS(9push_backEw)(void *s, unsigned c) { WS(6appendEPKwm)(s, &c, 1); }
void WS(6resizeEmw)(void *s, size_t n, unsigned c)
{
	size_t len = R(s)->len;
	if (n > len) {
		mutate(s, len, 0, n - len, W);
		fill(D(s) + len * W, n - len, c, W);
	} else if (n < len) mutate(s, n, len - n, 0, W);
}
void WS(6resizeEm)(void *s, size_t n) { WS(6resizeEmw)(s, n, 0); }
unsigned *WS(ixEm)(void *s, size_t i) { WS(12_M_leak_hardEv)(s); return (unsigned *)(D(s) + i * W); }
unsigned *WS(5beginEv)(void *s) { WS(12_M_leak_hardEv)(s); return (unsigned *)D(s); }
unsigned *WS(3endEv)(void *s) { WS(12_M_leak_hardEv)(s); return (unsigned *)(D(s) + R(s)->len * W); }
// erase(iterator first, iterator last): iterators are plain pointers; returns the new first
unsigned *WS(5eraseEN9__gnu_cxx17__normal_iteratorIPwS2_EES6_)(void *s, unsigned *first, unsigned *last)
{
	size_t pos = (size_t)(first - (unsigned *)D(s)), n = (size_t)(last - first);
	mutate(s, pos, n, 0, W);
	return (unsigned *)D(s) + pos;
}
void WS(4_Rep26_M_set_length_and_sharableEm)(rep *r, size_t n)
{
	if (r == EMPTY(W)) return;
	r->ref = 0;
	r->len = n;
	memset_z(data_of(r) + n * W, W);
}
unsigned *WS(4_Rep10_M_refdataEv)(rep *r) { return (unsigned *)data_of(r); }
rep *WS(12_S_empty_repEv)(void) { return EMPTY(W); }
void WS(12_Alloc_hiderC1EPwRKS1_)(void *h, unsigned *p, const void *a) { (void)a; D(h) = (char *)p; }
void WS(13_S_copy_charsEPwN9__gnu_cxx17__normal_iteratorIPKwS2_EES8_)(unsigned *p, const unsigned *k1, const unsigned *k2)
{
	memcpy(p, k1, (size_t)(k2 - k1) * W);
}
size_t WK(4sizeEv)(const void *s) { return R(s)->len; }
size_t WK(6lengthEv)(const void *s) { return R(s)->len; }
size_t WK(4findERKS2_m)(const void *s, const void *o, size_t pos) { return find(s, D(o), pos, R(o)->len, W); }
size_t WK(5rfindERKS2_m)(const void *s, const void *o, size_t pos)
{
	size_t len = R(s)->len, n = R(o)->len;
	if (n > len) return NPOS;
	size_t i = len - n < pos ? len - n : pos;
	for (;; i--) {
		if (!cmp(D(s) + i * W, D(o), n, W)) return i;
		if (!i) return NPOS;
	}
}
int WK(7compareERKS2_)(const void *s, const void *o)
{
	size_t a = R(s)->len, b = R(o)->len;
	return cmp_len(cmp(D(s), D(o), a < b ? a : b, W), a, b);
}
void *WK(6substrEmm)(void *ret, const void *s, size_t pos, size_t n)
{
	size_t len = R(s)->len;
	if (pos > len) mac32_guest_fatal("std::out_of_range", "wstring::substr");
	if (n > len - pos) n = len - pos;
	construct(ret, D(s) + pos * W, n, W);
	return ret;
}
const char *_ZNKSs5beginEv(const void *s) { return D(s); }
const char *_ZNKSs3endEv(const void *s) { return D(s) + R(s)->len; }
