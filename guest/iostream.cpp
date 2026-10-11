// GNU libstdc++ (GCC 4.0-4.2) iostreams for char, as 32-bit guest code.
//
// Old Mac programs inline parts of these classes (constructors, rdstate(),
// sgetc() ...), so the classes below declare the same bases, data members and
// virtual functions in the same order as GCC's headers. The compiler then lays
// out objects, vtables and VTTs the way GCC 4 did, and mangles every name the
// same way. The code behind them is our own: stdio for files, the GNU
// copy-on-write string (rtstring.c) for string streams, printf/strtod for
// numbers. Only char streams; no wide streams and no real locales.

typedef unsigned long size_t;
typedef int ptrdiff_t;
typedef union { char __mbstate8[128]; long long _mbstateL; } __mbstate_t;
typedef struct __sFILE FILE;

extern "C" {
void *malloc(size_t);
void free(void *);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
size_t strlen(const char *);
int snprintf(char *, size_t, const char *, ...);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
double strtod(const char *, char **);
FILE *fopen(const char *, const char *);
FILE *fdopen(int, const char *);
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fflush(FILE *);
int fseeko(FILE *, long long, int);
long long ftello(FILE *);
int getc(FILE *);
int ungetc(int, FILE *);
int putc(int, FILE *);
int fileno(FILE *);
extern FILE *__stdinp, *__stdoutp, *__stderrp;
void mac32_guest_fatal(const char *what, const char *detail);
}

inline void *operator new(size_t, void *p) noexcept { return p; }

namespace std {

typedef ptrdiff_t streamsize;
typedef long long streamoff;

template<class T> class allocator { };
template<class C> struct char_traits;
template<> struct char_traits<char> {
	typedef char char_type;
	typedef int int_type;
	static int eof() { return -1; }
	static int to_int(char c) { return (unsigned char)c; }
	static int not_eof(int c) { return c == -1 ? 0 : c; }
};

// rtstring.c implements these members (the copy-on-write GNU string)
template<class C, class T, class A> class basic_string {
public:
	C *_M_p;
	basic_string();
	basic_string(const C *, size_t, const A & = A());
	basic_string(const basic_string &);
	~basic_string();
	basic_string &operator=(const basic_string &);
	basic_string &assign(const C *, size_t);
	basic_string &append(const C *, size_t);
	void push_back(C);
	void reserve(size_t);
	void swap(basic_string &);
	void clear();
	size_t size() const { return ((const size_t *)_M_p)[-3]; }
	size_t capacity() const { return ((const size_t *)_M_p)[-2]; }
	const C *data() const { return _M_p; }
};
typedef basic_string<char, char_traits<char>, allocator<char> > string;

template<class S> class fpos {
public:
	streamoff _M_off;
	S _M_state;
	fpos() : _M_off(0), _M_state() { }
	fpos(streamoff o) : _M_off(o), _M_state() { }
	operator streamoff() const { return _M_off; }
};
typedef fpos<__mbstate_t> streampos;

enum _Ios_Fmtflags { _S_ios_fmtflags_end = 1 << 16 };
enum _Ios_Openmode { _S_ios_openmode_end = 1 << 16 };
enum _Ios_Iostate { _S_ios_iostate_end = 1 << 16 };
enum _Ios_Seekdir { _S_ios_seekdir_end = 1 << 16 };

class locale {
public:
	class facet;
	class _Impl;
	_Impl *_M_impl;
	locale() throw();
	locale(const locale &) throw();
	~locale() throw();
	const locale &operator=(const locale &) throw();
	static const locale &classic();
};

static locale::_Impl *const the_locale = (locale::_Impl *)"C";
locale::locale() throw() : _M_impl(the_locale) { }
locale::locale(const locale &o) throw() : _M_impl(o._M_impl) { }
locale::~locale() throw() { }
const locale &locale::operator=(const locale &o) throw() { _M_impl = o._M_impl; return *this; }
const locale &locale::classic() { static locale c; return c; }

class ios_base {
public:
	typedef _Ios_Fmtflags fmtflags;
	typedef _Ios_Iostate iostate;
	typedef _Ios_Openmode openmode;
	typedef _Ios_Seekdir seekdir;
	enum { boolalpha = 1, dec = 2, fixed = 4, hex = 8, internal = 16, left = 32, oct = 64, right = 128,
	       scientific = 256, showbase = 512, showpoint = 1024, showpos = 2048, skipws = 4096,
	       unitbuf = 8192, uppercase = 16384, adjustfield = 176, basefield = 74, floatfield = 260 };
	enum { badbit = 1, eofbit = 2, failbit = 4, goodbit = 0 };
	enum { app = 1, ate = 2, binary = 4, in = 8, out = 16, trunc = 32 };
	enum { beg = 0, cur = 1, end = 2 };
	enum event { erase_event, imbue_event, copyfmt_event };
	typedef void (*event_callback)(event, ios_base &, int);

	class Init {
	public:
		Init();
		~Init();
		static int _S_refcount;
		static bool _S_synced_with_stdio;
	};

	void register_callback(event_callback fn, int index);
	static int xalloc() throw();
	static bool sync_with_stdio(bool sync = true);
	locale imbue(const locale &loc);
	virtual ~ios_base();

	fmtflags flags() const { return _M_flags; }
	streamsize width() const { return _M_width; }
	streamsize precision() const { return _M_precision; }
	iostate rdstate() const { return _M_streambuf_state; }
protected:
	ios_base();

	streamsize _M_precision;
	streamsize _M_width;
	fmtflags _M_flags;
	iostate _M_exception;
	iostate _M_streambuf_state;
	struct _Callback_list {
		_Callback_list *_M_next;
		event_callback _M_fn;
		int _M_index;
		int _M_refcount;
	};
	_Callback_list *_M_callbacks;
	struct _Words {
		void *_M_pword;
		long _M_iword;
	};
	_Words _M_word_zero;
	enum { _S_local_word_size = 8 };
	_Words _M_local_word[_S_local_word_size];
	int _M_word_size;
	_Words *_M_word;
	locale _M_ios_locale;

	void _M_call_callbacks(event ev) throw();
	void _M_dispose_callbacks();
	_Words &_M_grow_words(int index, bool iword);
	void _M_init();
};

int ios_base::Init::_S_refcount;
bool ios_base::Init::_S_synced_with_stdio = true;

ios_base::ios_base() : _M_callbacks(0), _M_word_zero(), _M_word_size(_S_local_word_size), _M_word(_M_local_word) { }

ios_base::~ios_base()
{
	_M_call_callbacks(erase_event);
	_M_dispose_callbacks();
	if (_M_word != _M_local_word) free(_M_word);
	_M_word = 0;
}

void ios_base::_M_init()
{
	_M_precision = 6;
	_M_width = 0;
	_M_flags = (fmtflags)(skipws | dec);
	_M_ios_locale = locale();
}

void ios_base::register_callback(event_callback fn, int index)
{
	_Callback_list *c = (_Callback_list *)malloc(sizeof *c);
	c->_M_next = _M_callbacks;
	c->_M_fn = fn;
	c->_M_index = index;
	c->_M_refcount = 0;
	_M_callbacks = c;
}

void ios_base::_M_call_callbacks(event ev) throw()
{
	for (_Callback_list *c = _M_callbacks; c; c = c->_M_next) c->_M_fn(ev, *this, c->_M_index);
}

void ios_base::_M_dispose_callbacks()
{
	while (_M_callbacks) {
		_Callback_list *n = _M_callbacks->_M_next;
		free(_M_callbacks);
		_M_callbacks = n;
	}
}

int ios_base::xalloc() throw() { static int top = 4; return top++; }
bool ios_base::sync_with_stdio(bool sync) { bool was = Init::_S_synced_with_stdio; Init::_S_synced_with_stdio = sync; return was; }
locale ios_base::imbue(const locale &loc) { locale old = _M_ios_locale; _M_ios_locale = loc; _M_call_callbacks(imbue_event); return old; }

ios_base::_Words &ios_base::_M_grow_words(int ix, bool iword)
{
	(void)iword;
	if (ix < 0) { _M_streambuf_state = (iostate)(_M_streambuf_state | badbit); _M_word_zero = _Words(); return _M_word_zero; }
	if (ix >= _M_word_size) {
		int n = ix + 1 > 2 * _M_word_size ? ix + 1 : 2 * _M_word_size;
		_Words *w = (_Words *)malloc(n * sizeof *w);
		memset(w, 0, n * sizeof *w);
		memcpy(w, _M_word, _M_word_size * sizeof *w);
		if (_M_word != _M_local_word) free(_M_word);
		_M_word = w;
		_M_word_size = n;
	}
	return _M_word[ix];
}

template<class C, class T> class basic_streambuf;
template<class C, class T> class basic_ostream;

// The facet pointers in basic_ios. GCC code inlines ctype<char>::widen and
// narrow, which read these tables directly when the _ok byte is set; num_put
// and num_get are only ever handed back to our own library.
struct ctype_char_shape {
	virtual ~ctype_char_shape() { }
	int _M_refcount;
	void *_M_c_locale_ctype;
	bool _M_del;
	void *_M_toupper, *_M_tolower;
	const void *_M_table;
	char _M_widen_ok;
	char _M_widen[256];
	char _M_narrow[256];
	char _M_narrow_ok;
	ctype_char_shape()
	{
		_M_refcount = 1;
		_M_c_locale_ctype = 0;
		_M_del = false;
		_M_toupper = _M_tolower = 0;
		_M_table = 0;
		for (int i = 0; i < 256; i++) _M_widen[i] = _M_narrow[i] = (char)i;
		_M_widen_ok = _M_narrow_ok = 1;
	}
};
static ctype_char_shape the_ctype;
static void *the_num_facet[4];

template<class C, class T> class basic_ios : public ios_base {
public:
	typedef C char_type;
	typedef int int_type;
	typedef T traits_type;
	operator void *() const { return fail() ? 0 : (void *)this; }
	bool operator!() const { return fail(); }
	iostate rdstate() const { return _M_streambuf_state; }
	void clear(iostate state = (iostate)goodbit);
	void setstate(iostate s) { clear((iostate)(rdstate() | s)); }
	void _M_setstate(iostate s) { _M_streambuf_state = (iostate)(_M_streambuf_state | s); }
	bool good() const { return rdstate() == 0; }
	bool eof() const { return (rdstate() & eofbit) != 0; }
	bool fail() const { return (rdstate() & (badbit | failbit)) != 0; }
	bool bad() const { return (rdstate() & badbit) != 0; }
	iostate exceptions() const { return _M_exception; }
	void exceptions(iostate e) { _M_exception = e; clear(_M_streambuf_state); }
	explicit basic_ios(basic_streambuf<C, T> *sb) : ios_base() { init(sb); }
	virtual ~basic_ios() { }
	basic_ostream<C, T> *tie() const { return _M_tie; }
	basic_ostream<C, T> *tie(basic_ostream<C, T> *o) { basic_ostream<C, T> *was = _M_tie; _M_tie = o; return was; }
	basic_streambuf<C, T> *rdbuf() const { return _M_streambuf; }
	basic_streambuf<C, T> *rdbuf(basic_streambuf<C, T> *sb);
	basic_ios &copyfmt(const basic_ios &o);
	char_type fill() const
	{
		if (!_M_fill_init) { _M_fill = widen(' '); _M_fill_init = true; }
		return _M_fill;
	}
	char_type fill(char_type ch) { char_type was = fill(); _M_fill = ch; return was; }
	locale imbue(const locale &loc);
	char narrow(char_type c, char dfault) const;
	char_type widen(char c) const;
protected:
	basic_ios() : ios_base(), _M_tie(0), _M_fill(), _M_fill_init(false), _M_streambuf(0),
		      _M_ctype(0), _M_num_put(0), _M_num_get(0) { }
	void init(basic_streambuf<C, T> *sb);
	void _M_cache_locale(const locale &loc);

	basic_ostream<C, T> *_M_tie;
	mutable char_type _M_fill;
	mutable bool _M_fill_init;
	basic_streambuf<C, T> *_M_streambuf;
	const void *_M_ctype;
	const void *_M_num_put;
	const void *_M_num_get;
};

template<class C, class T> void basic_ios<C, T>::clear(iostate state)
{
	_M_streambuf_state = rdbuf() ? state : (iostate)(state | badbit);
	if (exceptions() & rdstate()) mac32_guest_fatal("std::ios_base::failure", "basic_ios::clear");
}

template<class C, class T> basic_streambuf<C, T> *basic_ios<C, T>::rdbuf(basic_streambuf<C, T> *sb)
{
	basic_streambuf<C, T> *was = _M_streambuf;
	_M_streambuf = sb;
	clear();
	return was;
}

template<class C, class T> basic_ios<C, T> &basic_ios<C, T>::copyfmt(const basic_ios &o)
{
	if (this == &o) return *this;
	_M_call_callbacks(erase_event);
	_M_precision = o._M_precision;
	_M_width = o._M_width;
	_M_flags = o._M_flags;
	_M_tie = o._M_tie;
	_M_fill = o.fill();
	_M_fill_init = true;
	_M_ios_locale = o._M_ios_locale;
	_M_call_callbacks(copyfmt_event);
	exceptions(o.exceptions());
	return *this;
}

template<class C, class T> locale basic_ios<C, T>::imbue(const locale &loc)
{
	locale old = ios_base::imbue(loc);
	_M_cache_locale(loc);
	return old;
}

template<class C, class T> char basic_ios<C, T>::narrow(char_type c, char) const { return c; }
template<class C, class T> C basic_ios<C, T>::widen(char c) const { return c; }

template<class C, class T> void basic_ios<C, T>::init(basic_streambuf<C, T> *sb)
{
	ios_base::_M_init();
	_M_cache_locale(_M_ios_locale);
	_M_fill = char_type();
	_M_fill_init = false;
	_M_tie = 0;
	_M_exception = (iostate)goodbit;
	_M_streambuf = sb;
	_M_streambuf_state = sb ? (iostate)goodbit : (iostate)badbit;
}

template<class C, class T> void basic_ios<C, T>::_M_cache_locale(const locale &)
{
	_M_ctype = &the_ctype;
	_M_num_put = &the_num_facet[0];
	_M_num_get = &the_num_facet[2];
}

template<class C, class T> class basic_streambuf {
public:
	typedef C char_type;
	typedef T traits_type;
	typedef int int_type;
	typedef streampos pos_type;
	typedef streamoff off_type;
	virtual ~basic_streambuf() { }

	locale pubimbue(const locale &loc) { locale old = _M_buf_locale; imbue(loc); _M_buf_locale = loc; return old; }
	locale getloc() const { return _M_buf_locale; }
	basic_streambuf *pubsetbuf(char_type *s, streamsize n) { return setbuf(s, n); }
	pos_type pubseekoff(off_type off, ios_base::seekdir way, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out))
	{ return seekoff(off, way, mode); }
	pos_type pubseekpos(pos_type sp, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out))
	{ return seekpos(sp, mode); }
	int pubsync() { return sync(); }
	streamsize in_avail()
	{
		streamsize n = egptr() - gptr();
		return n ? n : showmanyc();
	}
	int_type snextc() { return sbumpc() == T::eof() ? T::eof() : sgetc(); }
	int_type sbumpc()
	{
		if (gptr() < egptr()) return T::to_int(*_M_in_cur++);
		return uflow();
	}
	int_type sgetc()
	{
		if (gptr() < egptr()) return T::to_int(*gptr());
		return underflow();
	}
	streamsize sgetn(char_type *s, streamsize n) { return xsgetn(s, n); }
	int_type sputbackc(char_type c)
	{
		if (eback() < gptr() && gptr()[-1] == c) return T::to_int(*--_M_in_cur);
		return pbackfail(T::to_int(c));
	}
	int_type sungetc()
	{
		if (eback() < gptr()) return T::to_int(*--_M_in_cur);
		return pbackfail();
	}
	int_type sputc(char_type c)
	{
		if (pptr() < epptr()) { *_M_out_cur++ = c; return T::to_int(c); }
		return overflow(T::to_int(c));
	}
	streamsize sputn(const char_type *s, streamsize n) { return xsputn(s, n); }

protected:
	basic_streambuf() : _M_in_beg(0), _M_in_cur(0), _M_in_end(0), _M_out_beg(0), _M_out_cur(0), _M_out_end(0), _M_buf_locale() { }

	char_type *eback() const { return _M_in_beg; }
	char_type *gptr() const { return _M_in_cur; }
	char_type *egptr() const { return _M_in_end; }
	void gbump(int n) { _M_in_cur += n; }
	void setg(char_type *b, char_type *n, char_type *e) { _M_in_beg = b; _M_in_cur = n; _M_in_end = e; }
	char_type *pbase() const { return _M_out_beg; }
	char_type *pptr() const { return _M_out_cur; }
	char_type *epptr() const { return _M_out_end; }
	void pbump(int n) { _M_out_cur += n; }
	void setp(char_type *b, char_type *e) { _M_out_beg = _M_out_cur = b; _M_out_end = e; }

	virtual void imbue(const locale &) { }
	virtual basic_streambuf *setbuf(char_type *, streamsize) { return this; }
	virtual pos_type seekoff(off_type, ios_base::seekdir, ios_base::openmode = (ios_base::openmode)(ios_base::in | ios_base::out))
	{ return pos_type(off_type(-1)); }
	virtual pos_type seekpos(pos_type, ios_base::openmode = (ios_base::openmode)(ios_base::in | ios_base::out))
	{ return pos_type(off_type(-1)); }
	virtual int sync() { return 0; }
	virtual streamsize showmanyc() { return 0; }
	virtual streamsize xsgetn(char_type *s, streamsize n);
	virtual int_type underflow() { return T::eof(); }
	virtual int_type uflow()
	{
		int_type c = T::eof();
		if (underflow() != T::eof()) c = T::to_int(*_M_in_cur++);
		return c;
	}
	virtual int_type pbackfail(int_type = T::eof()) { return T::eof(); }
	virtual streamsize xsputn(const char_type *s, streamsize n);
	virtual int_type overflow(int_type = T::eof()) { return T::eof(); }

	char_type *_M_in_beg, *_M_in_cur, *_M_in_end;
	char_type *_M_out_beg, *_M_out_cur, *_M_out_end;
	locale _M_buf_locale;

	template<class, class> friend class basic_istream;
	template<class, class> friend class basic_ostream;
	friend class stdio_buf;
};

template<class C, class T> streamsize basic_streambuf<C, T>::xsgetn(char_type *s, streamsize n)
{
	streamsize done = 0;
	while (done < n) {
		streamsize avail = egptr() - gptr();
		if (avail) {
			streamsize k = avail < n - done ? avail : n - done;
			memcpy(s + done, gptr(), k);
			done += k;
			_M_in_cur += k;
		} else {
			int_type c = uflow();
			if (c == T::eof()) break;
			s[done++] = (char_type)c;
		}
	}
	return done;
}

template<class C, class T> streamsize basic_streambuf<C, T>::xsputn(const char_type *s, streamsize n)
{
	streamsize done = 0;
	while (done < n) {
		streamsize room = epptr() - pptr();
		if (room) {
			streamsize k = room < n - done ? room : n - done;
			memcpy(pptr(), s + done, k);
			done += k;
			_M_out_cur += k;
		} else {
			if (overflow(T::to_int(s[done])) == T::eof()) break;
			done++;
		}
	}
	return done;
}

template<class C, class T> class basic_istream : virtual public basic_ios<C, T> {
public:
	typedef C char_type;
	typedef int int_type;
	typedef T traits_type;
	typedef streampos pos_type;
	typedef streamoff off_type;
	typedef basic_streambuf<C, T> __streambuf_type;
	typedef ios_base::iostate iostate;

	explicit basic_istream(__streambuf_type *sb) : _M_gcount(0) { this->init(sb); }
	virtual ~basic_istream() { _M_gcount = 0; }

	class sentry {
	public:
		explicit sentry(basic_istream &is, bool noskipws = false);
		operator bool() const { return _M_ok; }
	private:
		bool _M_ok;
	};

	basic_istream &operator>>(basic_istream &(*pf)(basic_istream &)) { return pf(*this); }
	basic_istream &operator>>(bool &n);
	basic_istream &operator>>(short &n);
	basic_istream &operator>>(unsigned short &n);
	basic_istream &operator>>(int &n);
	basic_istream &operator>>(unsigned int &n);
	basic_istream &operator>>(long &n);
	basic_istream &operator>>(unsigned long &n);
	basic_istream &operator>>(long long &n);
	basic_istream &operator>>(unsigned long long &n);
	basic_istream &operator>>(float &f);
	basic_istream &operator>>(double &f);
	basic_istream &operator>>(long double &f);
	basic_istream &operator>>(void *&p);
	basic_istream &operator>>(__streambuf_type *sb);

	streamsize gcount() const { return _M_gcount; }
	int_type get();
	basic_istream &get(char_type &c);
	basic_istream &get(char_type *s, streamsize n, char_type delim);
	basic_istream &get(char_type *s, streamsize n);
	basic_istream &get(__streambuf_type &sb, char_type delim);
	basic_istream &get(__streambuf_type &sb);
	basic_istream &getline(char_type *s, streamsize n, char_type delim);
	basic_istream &getline(char_type *s, streamsize n);
	basic_istream &ignore();
	basic_istream &ignore(streamsize n);
	basic_istream &ignore(streamsize n, int_type delim);
	int_type peek();
	basic_istream &read(char_type *s, streamsize n);
	streamsize readsome(char_type *s, streamsize n);
	basic_istream &putback(char_type c);
	basic_istream &unget();
	int sync();
	pos_type tellg();
	basic_istream &seekg(pos_type pos);
	basic_istream &seekg(off_type off, ios_base::seekdir dir);

	// the number readers below; GCC 4.2 headers call this out of line
	template<class V> basic_istream &_M_extract(V &v);
protected:
	basic_istream() : _M_gcount(0) { this->init(0); }
	streamsize _M_gcount;
private:
	int read_number(char *buf, int n, bool fp);
};

template<class C, class T> class basic_ostream : virtual public basic_ios<C, T> {
public:
	typedef C char_type;
	typedef int int_type;
	typedef T traits_type;
	typedef streampos pos_type;
	typedef streamoff off_type;
	typedef basic_streambuf<C, T> __streambuf_type;
	typedef ios_base::iostate iostate;

	explicit basic_ostream(__streambuf_type *sb) { this->init(sb); }
	virtual ~basic_ostream() { }

	class sentry {
	public:
		explicit sentry(basic_ostream &os);
		~sentry()
		{
			if ((_M_os.flags() & ios_base::unitbuf) && _M_os.good() && _M_os.rdbuf() && _M_os.rdbuf()->pubsync() == -1)
				_M_os.setstate((iostate)ios_base::badbit);
		}
		operator bool() const { return _M_ok; }
	private:
		bool _M_ok;
		basic_ostream &_M_os;
	};

	basic_ostream &operator<<(basic_ostream &(*pf)(basic_ostream &)) { return pf(*this); }
	basic_ostream &operator<<(basic_ios<C, T> &(*pf)(basic_ios<C, T> &)) { pf(*this); return *this; }
	basic_ostream &operator<<(ios_base &(*pf)(ios_base &)) { pf(*this); return *this; }
	basic_ostream &operator<<(long n);
	basic_ostream &operator<<(unsigned long n);
	basic_ostream &operator<<(bool n);
	basic_ostream &operator<<(short n);
	basic_ostream &operator<<(unsigned short n);
	basic_ostream &operator<<(int n);
	basic_ostream &operator<<(unsigned int n);
	basic_ostream &operator<<(long long n);
	basic_ostream &operator<<(unsigned long long n);
	basic_ostream &operator<<(double f);
	basic_ostream &operator<<(float f);
	basic_ostream &operator<<(long double f);
	basic_ostream &operator<<(const void *p);
	basic_ostream &operator<<(__streambuf_type *sb);

	basic_ostream &put(char_type c);
	basic_ostream &write(const char_type *s, streamsize n);
	basic_ostream &flush();
	pos_type tellp();
	basic_ostream &seekp(pos_type pos);
	basic_ostream &seekp(off_type off, ios_base::seekdir dir);

	template<class V> basic_ostream &_M_insert(V v);
	void _M_write(const char_type *s, streamsize n);	// raw, sets badbit on a short write
	void _M_pad_write(const char *s, int n, int prefix);	// honours width/fill/adjust
protected:
	basic_ostream() { this->init(0); }
};

template<class C, class T> class basic_iostream : public basic_istream<C, T>, public basic_ostream<C, T> {
public:
	explicit basic_iostream(basic_streambuf<C, T> *sb) : basic_istream<C, T>(sb), basic_ostream<C, T>(sb) { }
	virtual ~basic_iostream() { }
protected:
	basic_iostream() : basic_istream<C, T>(), basic_ostream<C, T>() { }
};

typedef basic_istream<char, char_traits<char> > istream;
typedef basic_ostream<char, char_traits<char> > ostream;
typedef basic_streambuf<char, char_traits<char> > streambuf;
typedef basic_ios<char, char_traits<char> > ios;

// ---- istream ---------------------------------------------------------------

template<class C, class T> basic_istream<C, T>::sentry::sentry(basic_istream &is, bool noskipws) : _M_ok(false)
{
	iostate err = (iostate)ios_base::goodbit;
	if (is.good()) {
		if (is.tie()) is.tie()->flush();
		if (!noskipws && (is.flags() & ios_base::skipws)) {
			__streambuf_type *sb = is.rdbuf();
			int c = sb->sgetc();
			while (c != T::eof() && (c == ' ' || (c >= '\t' && c <= '\r'))) c = sb->snextc();
			if (c == T::eof()) err = (iostate)(err | ios_base::eofbit);
		}
	}
	if (is.good() && err == ios_base::goodbit) _M_ok = true;
	else is.setstate((iostate)(err | ios_base::failbit));
}

static bool is_digit_in(int c, int base)
{
	if (base == 16) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	if (base == 8) return c >= '0' && c <= '7';
	return c >= '0' && c <= '9';
}

// Copy the characters of one number into buf (sign, base prefix, digits, and
// for floats the point and exponent). Returns the base, or 0 if no digits.
template<class C, class T> int basic_istream<C, T>::read_number(char *buf, int n, bool fp)
{
	__streambuf_type *sb = this->rdbuf();
	int k = 0, digits = 0;
	int base = fp ? 10 : (this->flags() & ios_base::hex) ? 16 : (this->flags() & ios_base::oct) ? 8 :
		   (this->flags() & ios_base::basefield) ? 10 : 0;
	int c = sb->sgetc();
	if (c == '+' || c == '-') { buf[k++] = (char)c; c = sb->snextc(); }
	if (!fp && (base == 0 || base == 16) && c == '0') {
		buf[k++] = '0'; digits++;
		c = sb->snextc();
		if (c == 'x' || c == 'X') { buf[k++] = (char)c; c = sb->snextc(); base = 16; digits = 0; }
		else if (base == 0) base = 8;
	}
	if (base == 0) base = 10;
	while (c != T::eof() && is_digit_in(c, base) && k < n - 1) { buf[k++] = (char)c; digits++; c = sb->snextc(); }
	if (fp) {
		if (c == '.' && k < n - 1) {
			buf[k++] = '.';
			c = sb->snextc();
			while (c != T::eof() && is_digit_in(c, 10) && k < n - 1) { buf[k++] = (char)c; digits++; c = sb->snextc(); }
		}
		if (digits && (c == 'e' || c == 'E') && k < n - 3) {
			buf[k++] = (char)c;
			c = sb->snextc();
			if (c == '+' || c == '-') { buf[k++] = (char)c; c = sb->snextc(); }
			while (c != T::eof() && is_digit_in(c, 10) && k < n - 1) { buf[k++] = (char)c; c = sb->snextc(); }
		}
	}
	buf[k] = 0;
	if (c == T::eof()) this->_M_setstate((iostate)ios_base::eofbit);
	return digits ? base : 0;
}

template<class V> struct num_limits;
#define LIM(V, lo, hi, sgn) template<> struct num_limits<V> { static const bool is_signed = sgn; \
	static long long min() { return (long long)(lo); } static unsigned long long max() { return (unsigned long long)(hi); } };
LIM(short, -32768, 32767, true)
LIM(unsigned short, 0, 65535, false)
LIM(int, -2147483647 - 1, 2147483647, true)
LIM(unsigned int, 0, 4294967295u, false)
LIM(long, -2147483647L - 1, 2147483647L, true)
LIM(unsigned long, 0, 4294967295ul, false)
LIM(long long, -9223372036854775807LL - 1, 9223372036854775807LL, true)
LIM(unsigned long long, 0, 18446744073709551615ull, false)
#undef LIM

template<class C, class T> template<class V> basic_istream<C, T> &basic_istream<C, T>::_M_extract(V &v)
{
	sentry ok(*this, false);
	if (!ok) return *this;
	char buf[128];
	int base = read_number(buf, sizeof buf, false);
	if (!base) { this->setstate((iostate)(this->rdstate() | ios_base::failbit)); return *this; }
	char *end;
	if (num_limits<V>::is_signed) {
		long long x = strtoll(buf, &end, base);
		if (x < num_limits<V>::min() || (x > 0 && (unsigned long long)x > num_limits<V>::max()))
			this->setstate((iostate)(this->rdstate() | ios_base::failbit));
		else v = (V)x;
	} else {
		unsigned long long x = strtoull(buf, &end, base);
		if (x > num_limits<V>::max() && buf[0] != '-') this->setstate((iostate)(this->rdstate() | ios_base::failbit));
		else v = (V)x;
	}
	return *this;
}

template<> template<> istream &istream::_M_extract(double &v)
{
	sentry ok(*this, false);
	if (!ok) return *this;
	char buf[256];
	if (!read_number(buf, sizeof buf, true)) { setstate((iostate)(rdstate() | ios_base::failbit)); return *this; }
	v = strtod(buf, 0);
	return *this;
}
template<> template<> istream &istream::_M_extract(float &v) { double d = 0; _M_extract(d); if (!fail()) v = (float)d; return *this; }
template<> template<> istream &istream::_M_extract(long double &v) { double d = 0; _M_extract(d); if (!fail()) v = d; return *this; }
template<> template<> istream &istream::_M_extract(void *&v)
{
	unsigned long x = 0;
	_Ios_Fmtflags was = _M_flags;
	_M_flags = (fmtflags)((_M_flags & ~ios_base::basefield) | ios_base::hex);
	_M_extract(x);
	_M_flags = was;
	if (!fail()) v = (void *)x;
	return *this;
}
template<> template<> istream &istream::_M_extract(bool &v)
{
	if (!(flags() & ios_base::boolalpha)) {
		long x = 0;
		_M_extract(x);
		if (!fail()) { if (x == 0 || x == 1) v = x; else setstate((iostate)(rdstate() | ios_base::failbit)); }
		return *this;
	}
	sentry ok(*this, false);
	if (!ok) return *this;
	static const char *const word[2] = { "false", "true" };
	streambuf *sb = rdbuf();
	int c = sb->sgetc(), which = c == 't' ? 1 : c == 'f' ? 0 : -1;
	if (which < 0) { setstate((iostate)(rdstate() | ios_base::failbit)); return *this; }
	for (const char *p = word[which]; *p; p++, c = sb->snextc())
		if (c != *p) { setstate((iostate)(rdstate() | ios_base::failbit | (c == -1 ? ios_base::eofbit : 0))); return *this; }
	v = which;
	return *this;
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(bool &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(short &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(unsigned short &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(int &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(unsigned int &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(long &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(unsigned long &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(long long &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(unsigned long long &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(float &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(double &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(long double &n) { return _M_extract(n); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(void *&n) { return _M_extract(n); }

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::operator>>(__streambuf_type *out)
{
	_M_gcount = 0;
	sentry ok(*this, false);
	if (!ok) return *this;
	if (!out) { this->setstate((iostate)ios_base::badbit); return *this; }
	__streambuf_type *sb = this->rdbuf();
	int c;
	while ((c = sb->sgetc()) != T::eof()) {
		if (out->sputc((C)c) == T::eof()) break;
		sb->sbumpc();
		_M_gcount++;
	}
	iostate err = (iostate)(c == T::eof() ? ios_base::eofbit : 0);
	if (!_M_gcount) err = (iostate)(err | ios_base::failbit);
	if (err) this->setstate(err);
	return *this;
}

template<class C, class T> int basic_istream<C, T>::get()
{
	_M_gcount = 0;
	int c = T::eof();
	sentry ok(*this, true);
	if (ok) {
		c = this->rdbuf()->sbumpc();
		if (c != T::eof()) _M_gcount = 1;
	}
	if (!_M_gcount) this->setstate((iostate)(ios_base::eofbit | ios_base::failbit));
	return c;
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::get(char_type &ch)
{
	int c = get();
	if (c != T::eof()) ch = (C)c;
	return *this;
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::get(char_type *s, streamsize n, char_type delim)
{
	_M_gcount = 0;
	sentry ok(*this, true);
	iostate err = (iostate)0;
	if (ok) {
		__streambuf_type *sb = this->rdbuf();
		int c = sb->sgetc();
		while (_M_gcount + 1 < n && c != T::eof() && c != T::to_int(delim)) {
			*s++ = (C)c;
			_M_gcount++;
			c = sb->snextc();
		}
		if (c == T::eof()) err = (iostate)(err | ios_base::eofbit);
	}
	if (n > 0) *s = C();
	if (!_M_gcount) err = (iostate)(err | ios_base::failbit);
	if (err) this->setstate(err);
	return *this;
}
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::get(char_type *s, streamsize n) { return get(s, n, this->widen('\n')); }

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::get(__streambuf_type &out, char_type delim)
{
	_M_gcount = 0;
	sentry ok(*this, true);
	iostate err = (iostate)0;
	if (ok) {
		__streambuf_type *sb = this->rdbuf();
		int c = sb->sgetc();
		while (c != T::eof() && c != T::to_int(delim) && out.sputc((C)c) != T::eof()) {
			_M_gcount++;
			c = sb->snextc();
		}
		if (c == T::eof()) err = (iostate)(err | ios_base::eofbit);
	}
	if (!_M_gcount) err = (iostate)(err | ios_base::failbit);
	if (err) this->setstate(err);
	return *this;
}
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::get(__streambuf_type &out) { return get(out, this->widen('\n')); }

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::getline(char_type *s, streamsize n, char_type delim)
{
	_M_gcount = 0;
	sentry ok(*this, true);
	iostate err = (iostate)0;
	if (ok) {
		__streambuf_type *sb = this->rdbuf();
		int c = sb->sgetc();
		while (_M_gcount + 1 < n && c != T::eof() && c != T::to_int(delim)) {
			*s++ = (C)c;
			_M_gcount++;
			c = sb->snextc();
		}
		if (c == T::eof()) err = (iostate)(err | ios_base::eofbit);
		else if (c == T::to_int(delim)) { _M_gcount++; sb->sbumpc(); }
		else err = (iostate)(err | ios_base::failbit);	// buffer full
	}
	if (n > 0) *s = C();
	if (!_M_gcount) err = (iostate)(err | ios_base::failbit);
	if (err) this->setstate(err);
	return *this;
}
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::getline(char_type *s, streamsize n) { return getline(s, n, this->widen('\n')); }

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::ignore(streamsize n, int_type delim)
{
	_M_gcount = 0;
	sentry ok(*this, true);
	if (ok && n > 0) {
		__streambuf_type *sb = this->rdbuf();
		bool forever = n == 0x7fffffff;
		int c = sb->sgetc();
		while ((forever || _M_gcount < n) && c != T::eof() && c != delim) {
			_M_gcount++;
			c = sb->snextc();
		}
		if (c == T::eof()) this->setstate((iostate)ios_base::eofbit);
		else if (c == delim && (forever || _M_gcount < n)) { _M_gcount++; sb->sbumpc(); }
	}
	return *this;
}
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::ignore(streamsize n) { return ignore(n, T::eof()); }
template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::ignore() { return ignore(1, T::eof()); }

template<class C, class T> int basic_istream<C, T>::peek()
{
	_M_gcount = 0;
	int c = T::eof();
	sentry ok(*this, true);
	if (ok) {
		c = this->rdbuf()->sgetc();
		if (c == T::eof()) this->setstate((iostate)ios_base::eofbit);
	}
	return c;
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::read(char_type *s, streamsize n)
{
	_M_gcount = 0;
	sentry ok(*this, true);
	if (ok) {
		_M_gcount = this->rdbuf()->sgetn(s, n);
		if (_M_gcount != n) this->setstate((iostate)(ios_base::eofbit | ios_base::failbit));
	}
	return *this;
}

template<class C, class T> streamsize basic_istream<C, T>::readsome(char_type *s, streamsize n)
{
	_M_gcount = 0;
	sentry ok(*this, true);
	if (ok) {
		streamsize avail = this->rdbuf()->in_avail();
		if (avail > 0) _M_gcount = this->rdbuf()->sgetn(s, avail < n ? avail : n);
		else if (avail == -1) this->setstate((iostate)ios_base::eofbit);
	}
	return _M_gcount;
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::putback(char_type c)
{
	_M_gcount = 0;
	this->clear((iostate)(this->rdstate() & ~ios_base::eofbit));
	sentry ok(*this, true);
	if (ok && (!this->rdbuf() || this->rdbuf()->sputbackc(c) == T::eof())) this->setstate((iostate)ios_base::badbit);
	return *this;
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::unget()
{
	_M_gcount = 0;
	this->clear((iostate)(this->rdstate() & ~ios_base::eofbit));
	sentry ok(*this, true);
	if (ok && (!this->rdbuf() || this->rdbuf()->sungetc() == T::eof())) this->setstate((iostate)ios_base::badbit);
	return *this;
}

template<class C, class T> int basic_istream<C, T>::sync()
{
	__streambuf_type *sb = this->rdbuf();
	if (!sb) return -1;
	if (sb->pubsync() == -1) { this->setstate((iostate)ios_base::badbit); return -1; }
	return 0;
}

template<class C, class T> streampos basic_istream<C, T>::tellg()
{
	if (this->fail()) return pos_type(off_type(-1));
	return this->rdbuf()->pubseekoff(0, (ios_base::seekdir)ios_base::cur, (ios_base::openmode)ios_base::in);
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::seekg(pos_type pos)
{
	this->clear((iostate)(this->rdstate() & ~ios_base::eofbit));
	if (!this->fail() && this->rdbuf()->pubseekpos(pos, (ios_base::openmode)ios_base::in)._M_off == -1)
		this->setstate((iostate)ios_base::failbit);
	return *this;
}

template<class C, class T> basic_istream<C, T> &basic_istream<C, T>::seekg(off_type off, ios_base::seekdir dir)
{
	this->clear((iostate)(this->rdstate() & ~ios_base::eofbit));
	if (!this->fail() && this->rdbuf()->pubseekoff(off, dir, (ios_base::openmode)ios_base::in)._M_off == -1)
		this->setstate((iostate)ios_base::failbit);
	return *this;
}

// free input functions
template<class C, class T> basic_istream<C, T> &operator>>(basic_istream<C, T> &in, C &c)
{
	typename basic_istream<C, T>::sentry ok(in, false);
	if (ok) {
		int x = in.rdbuf()->sbumpc();
		if (x == T::eof()) in.setstate((ios_base::iostate)(ios_base::eofbit | ios_base::failbit));
		else c = (C)x;
	}
	return in;
}
template<class T> basic_istream<char, T> &operator>>(basic_istream<char, T> &in, unsigned char &c) { return in >> (char &)c; }
template<class T> basic_istream<char, T> &operator>>(basic_istream<char, T> &in, signed char &c) { return in >> (char &)c; }

template<class C, class T> basic_istream<C, T> &operator>>(basic_istream<C, T> &in, C *s)
{
	typename basic_istream<C, T>::sentry ok(in, false);
	int n = 0;
	if (ok) {
		streamsize w = in.width() > 0 ? in.width() : 0x7fffffff;
		basic_streambuf<C, T> *sb = in.rdbuf();
		int c = sb->sgetc();
		while (n < w - 1 && c != T::eof() && !(c == ' ' || (c >= '\t' && c <= '\r'))) {
			s[n++] = (C)c;
			c = sb->snextc();
		}
		if (c == T::eof()) in.setstate((ios_base::iostate)ios_base::eofbit);
		s[n] = C();
		ios_base &b = in;
		struct W : ios_base { static void zero(ios_base &x) { ((W &)x)._M_width = 0; } };
		W::zero(b);
	}
	if (!n) in.setstate((ios_base::iostate)ios_base::failbit);
	return in;
}
template<class T> basic_istream<char, T> &operator>>(basic_istream<char, T> &in, unsigned char *s) { return in >> (char *)s; }
template<class T> basic_istream<char, T> &operator>>(basic_istream<char, T> &in, signed char *s) { return in >> (char *)s; }

template<class C, class T> basic_istream<C, T> &ws(basic_istream<C, T> &in)
{
	basic_streambuf<C, T> *sb = in.rdbuf();
	int c = sb->sgetc();
	while (c != T::eof() && (c == ' ' || (c >= '\t' && c <= '\r'))) c = sb->snextc();
	if (c == T::eof()) in.setstate((ios_base::iostate)ios_base::eofbit);
	return in;
}

template<class C, class T, class A> basic_istream<C, T> &operator>>(basic_istream<C, T> &in, basic_string<C, T, A> &str)
{
	typename basic_istream<C, T>::sentry ok(in, false);
	size_t n = 0;
	if (ok) {
		str.clear();
		streamsize w = in.width() > 0 ? in.width() : 0x7fffffff;
		basic_streambuf<C, T> *sb = in.rdbuf();
		C buf[128];
		int k = 0;
		int c = sb->sgetc();
		while ((streamsize)n < w && c != T::eof() && !(c == ' ' || (c >= '\t' && c <= '\r'))) {
			buf[k++] = (C)c;
			n++;
			if (k == 128) { str.append(buf, k); k = 0; }
			c = sb->snextc();
		}
		str.append(buf, k);
		if (c == T::eof()) in.setstate((ios_base::iostate)ios_base::eofbit);
		struct W : ios_base { static void zero(ios_base &x) { ((W &)x)._M_width = 0; } };
		W::zero(in);
	}
	if (!n) in.setstate((ios_base::iostate)ios_base::failbit);
	return in;
}

template<class C, class T, class A> basic_istream<C, T> &getline(basic_istream<C, T> &in, basic_string<C, T, A> &str, C delim)
{
	typename basic_istream<C, T>::sentry ok(in, true);
	size_t n = 0;
	ios_base::iostate err = (ios_base::iostate)0;
	if (ok) {
		str.clear();
		basic_streambuf<C, T> *sb = in.rdbuf();
		C buf[128];
		int k = 0;
		int c = sb->sgetc();
		while (c != T::eof() && c != T::to_int(delim)) {
			buf[k++] = (C)c;
			n++;
			if (k == 128) { str.append(buf, k); k = 0; }
			c = sb->snextc();
		}
		str.append(buf, k);
		if (c == T::eof()) err = (ios_base::iostate)(err | ios_base::eofbit);
		else { n++; sb->sbumpc(); }
	}
	if (!n) err = (ios_base::iostate)(err | ios_base::failbit);
	if (err) in.setstate(err);
	return in;
}
template<class C, class T, class A> basic_istream<C, T> &getline(basic_istream<C, T> &in, basic_string<C, T, A> &str)
{
	return getline(in, str, in.widen('\n'));
}

// ---- ostream ---------------------------------------------------------------

template<class C, class T> basic_ostream<C, T>::sentry::sentry(basic_ostream &os) : _M_ok(false), _M_os(os)
{
	if (os.tie() && os.good()) os.tie()->flush();
	if (os.good()) _M_ok = true;
	else os.setstate((iostate)ios_base::failbit);
}

template<class C, class T> void basic_ostream<C, T>::_M_write(const char_type *s, streamsize n)
{
	if (this->rdbuf()->sputn(s, n) != n) this->setstate((iostate)ios_base::badbit);
}

// Write a formatted field. prefix = how many leading chars (sign, 0x) go
// before the fill when adjustfield is internal.
template<class C, class T> void basic_ostream<C, T>::_M_pad_write(const char *s, int n, int prefix)
{
	streamsize w = this->width();
	struct W : ios_base { static void zero(ios_base &x) { ((W &)x)._M_width = 0; } };
	W::zero(*this);
	if (w <= n) { _M_write(s, n); return; }
	int pad = w - n, adj = this->flags() & ios_base::adjustfield;
	C f = this->fill();
	C fills[64];
	for (int i = 0; i < 64; i++) fills[i] = f;
	if (adj == ios_base::left) {
		_M_write(s, n);
		for (; pad > 0 && !this->bad(); pad -= 64) _M_write(fills, pad < 64 ? pad : 64);
	} else {
		int head = adj == ios_base::internal ? prefix : 0;
		_M_write(s, head);
		for (; pad > 0 && !this->bad(); pad -= 64) _M_write(fills, pad < 64 ? pad : 64);
		_M_write(s + head, n - head);
	}
}

static int int_prefix(const char *s)
{
	int k = 0;
	if (s[k] == '-' || s[k] == '+') k++;
	if (s[k] == '0' && (s[k + 1] == 'x' || s[k + 1] == 'X')) k += 2;
	return k;
}

template<class C, class T> template<class V> basic_ostream<C, T> &basic_ostream<C, T>::_M_insert(V v)
{
	sentry ok(*this);
	if (!ok) return *this;
	char buf[96];
	int f = this->flags(), base = f & ios_base::basefield;
	char fmt[16], *p = fmt;
	*p++ = '%';
	if (base == ios_base::hex || base == ios_base::oct) {
		if (f & ios_base::showbase) *p++ = '#';
		*p++ = 'l'; *p++ = 'l';
		*p++ = base == ios_base::oct ? 'o' : (f & ios_base::uppercase) ? 'X' : 'x';
		*p = 0;
		unsigned long long u = sizeof(V) == 8 ? (unsigned long long)v : (unsigned long long)(unsigned long)v;
		snprintf(buf, sizeof buf, fmt, u);
	} else {
		if ((f & ios_base::showpos) && num_limits<V>::is_signed) *p++ = '+';
		*p++ = 'l'; *p++ = 'l';
		*p++ = num_limits<V>::is_signed ? 'd' : 'u';
		*p = 0;
		if (num_limits<V>::is_signed) snprintf(buf, sizeof buf, fmt, (long long)v);
		else snprintf(buf, sizeof buf, fmt, (unsigned long long)v);
	}
	_M_pad_write(buf, strlen(buf), int_prefix(buf));
	return *this;
}

template<> template<> ostream &ostream::_M_insert(double v)
{
	sentry ok(*this);
	if (!ok) return *this;
	char buf[400];
	int f = flags(), ff = f & ios_base::floatfield;
	char fmt[16], *p = fmt;
	*p++ = '%';
	if (f & ios_base::showpos) *p++ = '+';
	if (f & ios_base::showpoint) *p++ = '#';
	*p++ = '.'; *p++ = '*';
	bool up = f & ios_base::uppercase;
	*p++ = ff == ios_base::fixed ? 'f' : ff == ios_base::scientific ? (up ? 'E' : 'e') : (up ? 'G' : 'g');
	*p = 0;
	int prec = precision() < 0 ? 6 : precision();
	if (prec > 300) prec = 300;
	snprintf(buf, sizeof buf, fmt, prec, v);
	_M_pad_write(buf, strlen(buf), buf[0] == '-' || buf[0] == '+');
	return *this;
}
template<> template<> ostream &ostream::_M_insert(long double v) { return _M_insert((double)v); }
template<> template<> ostream &ostream::_M_insert(bool v)
{
	if (!(flags() & ios_base::boolalpha)) return _M_insert((long)v);
	sentry ok(*this);
	if (ok) _M_pad_write(v ? "true" : "false", v ? 4 : 5, 0);
	return *this;
}
template<> template<> ostream &ostream::_M_insert(const void *v)
{
	sentry ok(*this);
	if (!ok) return *this;
	char buf[32];
	snprintf(buf, sizeof buf, "0x%lx", (unsigned long)v);
	_M_pad_write(buf, strlen(buf), 2);
	return *this;
}

template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(long n) { return _M_insert(n); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(unsigned long n) { return _M_insert(n); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(bool n) { return _M_insert(n); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(short n)
{
	if ((this->flags() & ios_base::basefield) == ios_base::hex || (this->flags() & ios_base::basefield) == ios_base::oct)
		return _M_insert((unsigned long)(unsigned short)n);
	return _M_insert((long)n);
}
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(unsigned short n) { return _M_insert((unsigned long)n); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(int n)
{
	if ((this->flags() & ios_base::basefield) == ios_base::hex || (this->flags() & ios_base::basefield) == ios_base::oct)
		return _M_insert((unsigned long)(unsigned int)n);
	return _M_insert((long)n);
}
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(unsigned int n) { return _M_insert((unsigned long)n); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(long long n) { return _M_insert(n); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(unsigned long long n) { return _M_insert(n); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(double f) { return _M_insert(f); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(float f) { return _M_insert((double)f); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(long double f) { return _M_insert(f); }
template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(const void *p) { return _M_insert(p); }

template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::operator<<(__streambuf_type *in)
{
	sentry ok(*this);
	if (!ok) return *this;
	if (!in) { this->setstate((iostate)ios_base::badbit); return *this; }
	int c, n = 0;
	while ((c = in->sgetc()) != T::eof()) {
		if (this->rdbuf()->sputc((C)c) == T::eof()) break;
		in->sbumpc();
		n++;
	}
	if (!n) this->setstate((iostate)ios_base::failbit);
	return *this;
}

template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::put(char_type c)
{
	sentry ok(*this);
	if (ok && this->rdbuf()->sputc(c) == T::eof()) this->setstate((iostate)ios_base::badbit);
	return *this;
}

template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::write(const char_type *s, streamsize n)
{
	sentry ok(*this);
	if (ok) _M_write(s, n);
	return *this;
}

template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::flush()
{
	if (this->rdbuf() && this->rdbuf()->pubsync() == -1) this->setstate((iostate)ios_base::badbit);
	return *this;
}

template<class C, class T> streampos basic_ostream<C, T>::tellp()
{
	if (this->fail()) return pos_type(off_type(-1));
	return this->rdbuf()->pubseekoff(0, (ios_base::seekdir)ios_base::cur, (ios_base::openmode)ios_base::out);
}

template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::seekp(pos_type pos)
{
	if (!this->fail() && this->rdbuf()->pubseekpos(pos, (ios_base::openmode)ios_base::out)._M_off == -1)
		this->setstate((iostate)ios_base::failbit);
	return *this;
}

template<class C, class T> basic_ostream<C, T> &basic_ostream<C, T>::seekp(off_type off, ios_base::seekdir dir)
{
	if (!this->fail() && this->rdbuf()->pubseekoff(off, dir, (ios_base::openmode)ios_base::out)._M_off == -1)
		this->setstate((iostate)ios_base::failbit);
	return *this;
}

// free output functions
template<class C, class T> basic_ostream<C, T> &__ostream_insert(basic_ostream<C, T> &out, const C *s, streamsize n)
{
	typename basic_ostream<C, T>::sentry ok(out);
	if (ok) out._M_pad_write(s, n, 0);
	return out;
}
template<class C, class T> basic_ostream<C, T> &operator<<(basic_ostream<C, T> &out, C c) { return __ostream_insert(out, &c, 1); }
template<class T> basic_ostream<char, T> &operator<<(basic_ostream<char, T> &out, char c) { return __ostream_insert(out, &c, 1); }
template<class T> basic_ostream<char, T> &operator<<(basic_ostream<char, T> &out, signed char c) { return out << (char)c; }
template<class T> basic_ostream<char, T> &operator<<(basic_ostream<char, T> &out, unsigned char c) { return out << (char)c; }
template<class C, class T> basic_ostream<C, T> &operator<<(basic_ostream<C, T> &out, const C *s)
{
	if (!s) out.setstate((ios_base::iostate)ios_base::badbit);
	else __ostream_insert(out, s, strlen(s));
	return out;
}
template<class T> basic_ostream<char, T> &operator<<(basic_ostream<char, T> &out, const char *s)
{
	if (!s) out.setstate((ios_base::iostate)ios_base::badbit);
	else __ostream_insert(out, s, strlen(s));
	return out;
}
template<class T> basic_ostream<char, T> &operator<<(basic_ostream<char, T> &out, const signed char *s) { return out << (const char *)s; }
template<class T> basic_ostream<char, T> &operator<<(basic_ostream<char, T> &out, const unsigned char *s) { return out << (const char *)s; }
template<class C, class T, class A> basic_ostream<C, T> &operator<<(basic_ostream<C, T> &out, const basic_string<C, T, A> &s)
{
	return __ostream_insert(out, s.data(), s.size());
}
template<class C, class T> basic_ostream<C, T> &endl(basic_ostream<C, T> &out) { out.put(out.widen('\n')); return out.flush(); }
template<class C, class T> basic_ostream<C, T> &ends(basic_ostream<C, T> &out) { return out.put(C()); }
template<class C, class T> basic_ostream<C, T> &flush(basic_ostream<C, T> &out) { return out.flush(); }

// ---- string streams ----------------------------------------------------------

template<class C, class T, class A> class basic_stringbuf : public basic_streambuf<C, T> {
public:
	typedef C char_type;
	typedef int int_type;
	typedef streampos pos_type;
	typedef streamoff off_type;
	typedef basic_streambuf<C, T> __streambuf_type;
	typedef basic_string<C, T, A> __string_type;
	typedef size_t __size_type;
protected:
	ios_base::openmode _M_mode;
	__string_type _M_string;
public:
	explicit basic_stringbuf(ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out))
		: __streambuf_type(), _M_mode(mode), _M_string() { }
	explicit basic_stringbuf(const __string_type &s, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out))
		: __streambuf_type(), _M_mode(), _M_string(s.data(), s.size()) { _M_stringbuf_init(mode); }
	__string_type str() const
	{
		if (this->pptr()) {
			C *e = this->pptr() > this->egptr() ? this->pptr() : this->egptr();
			return __string_type(this->pbase(), e - this->pbase());
		}
		return _M_string;
	}
	void str(const __string_type &s)
	{
		_M_string.assign(s.data(), s.size());
		_M_stringbuf_init(_M_mode);
	}
protected:
	void _M_stringbuf_init(ios_base::openmode mode)
	{
		_M_mode = mode;
		__size_type len = 0;
		if (_M_mode & (ios_base::ate | ios_base::app)) len = _M_string.size();
		_M_sync(const_cast<C *>(_M_string.data()), 0, len);
	}
	virtual streamsize showmanyc()
	{
		if (!(_M_mode & ios_base::in)) return -1;
		_M_update_egptr();
		return this->egptr() - this->gptr();
	}
	virtual int_type underflow();
	virtual int_type pbackfail(int_type c = T::eof());
	virtual int_type overflow(int_type c = T::eof());
	virtual __streambuf_type *setbuf(char_type *s, streamsize n)
	{
		if (s && n >= 0) {
			_M_string.clear();
			_M_sync(s, n, 0);
		}
		return this;
	}
	virtual pos_type seekoff(off_type off, ios_base::seekdir way, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out));
	virtual pos_type seekpos(pos_type sp, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out));
	void _M_sync(char_type *base, __size_type i, __size_type o);
	void _M_update_egptr()
	{
		if (this->pptr() && this->pptr() > this->egptr()) {
			if (_M_mode & ios_base::in) this->setg(this->eback(), this->gptr(), this->pptr());
			else this->setg(this->pptr(), this->pptr(), this->pptr());
		}
	}
};

// The get and put areas point into _M_string's own buffer; the string's
// length is only brought up to date when the buffer has to grow.
template<class C, class T, class A> void basic_stringbuf<C, T, A>::_M_sync(char_type *base, __size_type i, __size_type o)
{
	bool in = _M_mode & ios_base::in, out = _M_mode & ios_base::out;
	C *endg = base + _M_string.size(), *endp = base + _M_string.capacity();
	if (base != _M_string.data()) {	// setbuf: a buffer of the caller's
		endg += i;
		i = 0;
		endp = endg;
	}
	if (in) this->setg(base, base + i, endg);
	if (out) {
		this->setp(base, endp);
		this->pbump(o);
		if (!in) this->setg(endg, endg, endg);
	}
}

template<class C, class T, class A> int basic_stringbuf<C, T, A>::underflow()
{
	if (!(_M_mode & ios_base::in)) return T::eof();
	_M_update_egptr();
	return this->gptr() < this->egptr() ? T::to_int(*this->gptr()) : T::eof();
}

template<class C, class T, class A> int basic_stringbuf<C, T, A>::pbackfail(int_type c)
{
	if (this->eback() >= this->gptr()) return T::eof();
	if (c == T::eof()) { this->gbump(-1); return T::not_eof(c); }
	bool same = this->gptr()[-1] == (C)c;
	if (!same && !(_M_mode & ios_base::out)) return T::eof();
	this->gbump(-1);
	if (!same) *this->gptr() = (C)c;
	return c;
}

template<class C, class T, class A> int basic_stringbuf<C, T, A>::overflow(int_type c)
{
	if (!(_M_mode & ios_base::out)) return T::eof();
	if (c == T::eof()) return T::not_eof(c);
	if (this->pptr() < this->epptr()) {
		*this->pptr() = (C)c;
	} else {
		__size_type cap = _M_string.capacity(), len = 2 * cap > 512 ? 2 * cap : 512;
		__string_type tmp;
		tmp.reserve(len);
		if (this->pbase()) tmp.assign(this->pbase(), this->epptr() - this->pbase());
		tmp.push_back((C)c);
		_M_string.swap(tmp);
		_M_sync(const_cast<C *>(_M_string.data()), this->gptr() - this->eback(), this->pptr() - this->pbase());
	}
	this->pbump(1);
	return c;
}

template<class C, class T, class A> streampos basic_stringbuf<C, T, A>::seekoff(off_type off, ios_base::seekdir way, ios_base::openmode mode)
{
	pos_type ret = pos_type(off_type(-1));
	bool in = (ios_base::in & _M_mode & mode) != 0, out = (ios_base::out & _M_mode & mode) != 0;
	bool both = in && out && way != ios_base::cur;
	in &= !(mode & ios_base::out);
	out &= !(mode & ios_base::in);
	const C *beg = in ? this->eback() : this->pbase();
	if ((beg || !off) && (in || out || both)) {
		_M_update_egptr();
		off_type oi = off, oo = off;
		if (way == ios_base::cur) {
			oi += this->gptr() - beg;
			oo += this->pptr() - beg;
		} else if (way == ios_base::end) {
			oo = oi += this->egptr() - beg;
		}
		if ((in || both) && oi >= 0 && this->egptr() - beg >= oi) {
			this->gbump((beg + oi) - this->gptr());
			ret = pos_type(oi);
		}
		if ((out || both) && oo >= 0 && this->egptr() - beg >= oo) {
			this->pbump((beg + oo) - this->pptr());
			ret = pos_type(oo);
		}
	}
	return ret;
}

template<class C, class T, class A> streampos basic_stringbuf<C, T, A>::seekpos(pos_type sp, ios_base::openmode mode)
{
	pos_type ret = pos_type(off_type(-1));
	bool in = (ios_base::in & _M_mode & mode) != 0, out = (ios_base::out & _M_mode & mode) != 0;
	const C *beg = in ? this->eback() : this->pbase();
	if ((beg || !sp._M_off) && (in || out)) {
		_M_update_egptr();
		off_type pos = sp._M_off;
		if (0 <= pos && pos <= this->egptr() - beg) {
			if (in) this->gbump((beg + pos) - this->gptr());
			if (out) this->pbump((beg + pos) - this->pptr());
			ret = sp;
		}
	}
	return ret;
}

template<class C, class T, class A> class basic_istringstream : public basic_istream<C, T> {
public:
	typedef basic_string<C, T, A> __string_type;
	typedef basic_stringbuf<C, T, A> __stringbuf_type;
	explicit basic_istringstream(ios_base::openmode mode = (ios_base::openmode)ios_base::in)
		: basic_istream<C, T>(), _M_stringbuf((ios_base::openmode)(mode | ios_base::in)) { this->init(&_M_stringbuf); }
	explicit basic_istringstream(const __string_type &s, ios_base::openmode mode = (ios_base::openmode)ios_base::in)
		: basic_istream<C, T>(), _M_stringbuf(s, (ios_base::openmode)(mode | ios_base::in)) { this->init(&_M_stringbuf); }
	~basic_istringstream() { }
	__stringbuf_type *rdbuf() const { return const_cast<__stringbuf_type *>(&_M_stringbuf); }
	__string_type str() const { return _M_stringbuf.str(); }
	void str(const __string_type &s) { _M_stringbuf.str(s); }
private:
	__stringbuf_type _M_stringbuf;
};

template<class C, class T, class A> class basic_ostringstream : public basic_ostream<C, T> {
public:
	typedef basic_string<C, T, A> __string_type;
	typedef basic_stringbuf<C, T, A> __stringbuf_type;
	explicit basic_ostringstream(ios_base::openmode mode = (ios_base::openmode)ios_base::out)
		: basic_ostream<C, T>(), _M_stringbuf((ios_base::openmode)(mode | ios_base::out)) { this->init(&_M_stringbuf); }
	explicit basic_ostringstream(const __string_type &s, ios_base::openmode mode = (ios_base::openmode)ios_base::out)
		: basic_ostream<C, T>(), _M_stringbuf(s, (ios_base::openmode)(mode | ios_base::out)) { this->init(&_M_stringbuf); }
	~basic_ostringstream() { }
	__stringbuf_type *rdbuf() const { return const_cast<__stringbuf_type *>(&_M_stringbuf); }
	__string_type str() const { return _M_stringbuf.str(); }
	void str(const __string_type &s) { _M_stringbuf.str(s); }
private:
	__stringbuf_type _M_stringbuf;
};

template<class C, class T, class A> class basic_stringstream : public basic_iostream<C, T> {
public:
	typedef basic_string<C, T, A> __string_type;
	typedef basic_stringbuf<C, T, A> __stringbuf_type;
	explicit basic_stringstream(ios_base::openmode m = (ios_base::openmode)(ios_base::out | ios_base::in))
		: basic_iostream<C, T>(), _M_stringbuf(m) { this->init(&_M_stringbuf); }
	explicit basic_stringstream(const __string_type &s, ios_base::openmode m = (ios_base::openmode)(ios_base::out | ios_base::in))
		: basic_iostream<C, T>(), _M_stringbuf(s, m) { this->init(&_M_stringbuf); }
	~basic_stringstream() { }
	__stringbuf_type *rdbuf() const { return const_cast<__stringbuf_type *>(&_M_stringbuf); }
	__string_type str() const { return _M_stringbuf.str(); }
	void str(const __string_type &s) { _M_stringbuf.str(s); }
private:
	__stringbuf_type _M_stringbuf;
};

// ---- file streams ------------------------------------------------------------

template<class C> class __basic_file;
template<> class __basic_file<char> {
	FILE *_M_cfile;
	bool _M_cfile_created;
public:
	__basic_file(void *lock = 0);
	__basic_file *open(const char *name, ios_base::openmode mode, int prot = 0664);
	__basic_file *sys_open(FILE *f, ios_base::openmode);
	__basic_file *sys_open(int fd, ios_base::openmode mode);
	__basic_file *close();
	bool is_open() const;
	int fd();
	FILE *file();
	~__basic_file();
	streamsize xsputn(const char *s, streamsize n);
	streamsize xsgetn(char *s, streamsize n);
	streamoff seekoff(streamoff off, ios_base::seekdir way);
	int sync();
	streamsize showmanyc();
};

static const char *fopen_mode(ios_base::openmode m)
{
	int k = m & (ios_base::in | ios_base::out | ios_base::trunc | ios_base::app);
	bool b = m & ios_base::binary;
	switch (k) {
	case ios_base::out:
	case ios_base::out | ios_base::trunc: return b ? "wb" : "w";
	case ios_base::out | ios_base::app:
	case ios_base::app: return b ? "ab" : "a";
	case ios_base::in: return b ? "rb" : "r";
	case ios_base::in | ios_base::out: return b ? "r+b" : "r+";
	case ios_base::in | ios_base::out | ios_base::trunc: return b ? "w+b" : "w+";
	case ios_base::in | ios_base::out | ios_base::app:
	case ios_base::in | ios_base::app: return b ? "a+b" : "a+";
	}
	return 0;
}

__basic_file<char>::__basic_file(void *) : _M_cfile(0), _M_cfile_created(false) { }
__basic_file<char>::~__basic_file() { close(); }
bool __basic_file<char>::is_open() const { return _M_cfile != 0; }
int __basic_file<char>::fd() { return _M_cfile ? fileno(_M_cfile) : -1; }
FILE *__basic_file<char>::file() { return _M_cfile; }

__basic_file<char> *__basic_file<char>::open(const char *name, ios_base::openmode mode, int)
{
	const char *m = fopen_mode(mode);
	if (is_open() || !m) return 0;
	_M_cfile = fopen(name, m);
	if (!_M_cfile) return 0;
	_M_cfile_created = true;
	return this;
}

__basic_file<char> *__basic_file<char>::sys_open(FILE *f, ios_base::openmode)
{
	if (is_open() || !f) return 0;
	_M_cfile = f;
	_M_cfile_created = false;
	return this;
}

__basic_file<char> *__basic_file<char>::sys_open(int fd, ios_base::openmode mode)
{
	const char *m = fopen_mode(mode);
	if (is_open() || !m) return 0;
	_M_cfile = fdopen(fd, m);
	if (!_M_cfile) return 0;
	_M_cfile_created = true;
	return this;
}

__basic_file<char> *__basic_file<char>::close()
{
	if (!_M_cfile) return 0;
	int rc = _M_cfile_created ? fclose(_M_cfile) : fflush(_M_cfile);
	_M_cfile = 0;
	return rc ? 0 : this;
}

streamsize __basic_file<char>::xsputn(const char *s, streamsize n) { return fwrite(s, 1, n, _M_cfile); }
streamsize __basic_file<char>::xsgetn(char *s, streamsize n) { return fread(s, 1, n, _M_cfile); }
streamoff __basic_file<char>::seekoff(streamoff off, ios_base::seekdir way)
{
	if (fseeko(_M_cfile, off, way == ios_base::beg ? 0 : way == ios_base::cur ? 1 : 2)) return -1;
	return ftello(_M_cfile);
}
int __basic_file<char>::sync() { return fflush(_M_cfile); }
streamsize __basic_file<char>::showmanyc() { return 0; }

struct mutex_shape { long sig; char opaque[40]; };	// pthread_mutex_t, i386

// A plain buffered file: the get area holds what was read and not yet
// consumed, the put area what was written and not yet handed to stdio.
template<class C, class T> class basic_filebuf : public basic_streambuf<C, T> {
public:
	typedef C char_type;
	typedef int int_type;
	typedef streampos pos_type;
	typedef streamoff off_type;
	typedef basic_streambuf<C, T> __streambuf_type;
	typedef __basic_file<char> __file_type;
protected:
	mutex_shape _M_lock;
	__file_type _M_file;
	ios_base::openmode _M_mode;
	__mbstate_t _M_state_beg, _M_state_cur, _M_state_last;
	char_type *_M_buf;
	size_t _M_buf_size;
	bool _M_buf_allocated;
	bool _M_reading;
	bool _M_writing;
	char_type _M_pback;
	char_type *_M_pback_cur_save;
	char_type *_M_pback_end_save;
	bool _M_pback_init;
	const void *_M_codecvt;
	char *_M_ext_buf;
	streamsize _M_ext_buf_size;
	const char *_M_ext_next;
	char *_M_ext_end;
public:
	basic_filebuf();
	virtual ~basic_filebuf() { close(); }
	bool is_open() const { return _M_file.is_open(); }
	basic_filebuf *open(const char *name, ios_base::openmode mode);
	basic_filebuf *close();
protected:
	void _M_allocate_internal_buffer();
	void _M_destroy_internal_buffer();
	virtual streamsize showmanyc();
	virtual int_type underflow();
	virtual int_type pbackfail(int_type c = T::eof());
	virtual int_type overflow(int_type c = T::eof());
	virtual __streambuf_type *setbuf(char_type *s, streamsize n);
	virtual pos_type seekoff(off_type off, ios_base::seekdir way, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out));
	virtual pos_type seekpos(pos_type pos, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out));
	virtual int sync();
	virtual void imbue(const locale &) { }
	virtual streamsize xsgetn(char_type *s, streamsize n);
	virtual streamsize xsputn(const char_type *s, streamsize n);
	bool flush_out();
	void drop_in();
};

template<class C, class T> basic_filebuf<C, T>::basic_filebuf()
	: __streambuf_type(), _M_lock(), _M_file(&_M_lock), _M_mode((ios_base::openmode)0), _M_state_beg(), _M_state_cur(), _M_state_last(),
	  _M_buf(0), _M_buf_size(8192), _M_buf_allocated(false), _M_reading(false), _M_writing(false), _M_pback(),
	  _M_pback_cur_save(0), _M_pback_end_save(0), _M_pback_init(false), _M_codecvt(0), _M_ext_buf(0),
	  _M_ext_buf_size(0), _M_ext_next(0), _M_ext_end(0) { }

template<class C, class T> void basic_filebuf<C, T>::_M_allocate_internal_buffer()
{
	if (!_M_buf_allocated && !_M_buf) {
		_M_buf = (C *)malloc(_M_buf_size);
		_M_buf_allocated = true;
	}
}

template<class C, class T> void basic_filebuf<C, T>::_M_destroy_internal_buffer()
{
	if (_M_buf_allocated) {
		free(_M_buf);
		_M_buf = 0;
		_M_buf_allocated = false;
	}
}

template<class C, class T> basic_filebuf<C, T> *basic_filebuf<C, T>::open(const char *name, ios_base::openmode mode)
{
	if (is_open() || !_M_file.open(name, mode)) return 0;
	_M_allocate_internal_buffer();
	_M_mode = mode;
	_M_reading = _M_writing = false;
	this->setg(_M_buf, _M_buf, _M_buf);
	this->setp(0, 0);
	if ((mode & ios_base::ate) && seekoff(0, (ios_base::seekdir)ios_base::end, mode)._M_off == -1) {
		close();
		return 0;
	}
	return this;
}

template<class C, class T> basic_filebuf<C, T> *basic_filebuf<C, T>::close()
{
	if (!is_open()) return 0;
	bool ok = flush_out();
	_M_mode = (ios_base::openmode)0;
	_M_reading = _M_writing = false;
	_M_destroy_internal_buffer();
	this->setg(0, 0, 0);
	this->setp(0, 0);
	if (!_M_file.close()) ok = false;
	return ok ? this : 0;
}

// hand buffered output to stdio
template<class C, class T> bool basic_filebuf<C, T>::flush_out()
{
	if (!_M_writing) return true;
	streamsize n = this->pptr() - this->pbase();
	bool ok = !n || _M_file.xsputn(this->pbase(), n) == n;
	this->setp(_M_buf, _M_buf + _M_buf_size);
	return ok;
}

// forget read-ahead, moving the file back to the reader's position
template<class C, class T> void basic_filebuf<C, T>::drop_in()
{
	if (!_M_reading) return;
	streamsize unread = this->egptr() - this->gptr();
	if (unread) _M_file.seekoff(-unread, (ios_base::seekdir)ios_base::cur);
	this->setg(_M_buf, _M_buf, _M_buf);
	_M_reading = false;
}

template<class C, class T> streamsize basic_filebuf<C, T>::showmanyc()
{
	if (!(_M_mode & ios_base::in) || !is_open()) return -1;
	return this->egptr() - this->gptr();
}

template<class C, class T> int basic_filebuf<C, T>::underflow()
{
	if (!(_M_mode & ios_base::in) || !is_open()) return T::eof();
	if (_M_writing) {
		if (!flush_out()) return T::eof();
		_M_writing = false;
		this->setp(0, 0);
	}
	if (this->gptr() < this->egptr()) return T::to_int(*this->gptr());
	streamsize n = _M_file.xsgetn(_M_buf, _M_buf_size);
	this->setg(_M_buf, _M_buf, _M_buf + (n > 0 ? n : 0));
	_M_reading = true;
	return n > 0 ? T::to_int(*this->gptr()) : T::eof();
}

template<class C, class T> int basic_filebuf<C, T>::pbackfail(int_type c)
{
	if (!(_M_mode & ios_base::in) || this->eback() >= this->gptr()) return T::eof();
	this->gbump(-1);
	if (c == T::eof()) return T::not_eof(c);
	*this->gptr() = (C)c;
	return c;
}

template<class C, class T> int basic_filebuf<C, T>::overflow(int_type c)
{
	if (!(_M_mode & (ios_base::out | ios_base::app)) || !is_open()) return T::eof();
	if (_M_reading) drop_in();
	if (!_M_writing) {
		this->setp(_M_buf, _M_buf + _M_buf_size);
		this->setg(_M_buf, _M_buf, _M_buf);
		_M_writing = true;
	} else if (!flush_out()) {
		return T::eof();
	}
	if (c == T::eof()) return T::not_eof(c);
	*this->pptr() = (C)c;
	this->pbump(1);
	return c;
}

template<class C, class T> basic_streambuf<C, T> *basic_filebuf<C, T>::setbuf(char_type *s, streamsize n)
{
	if (is_open()) return this;
	_M_destroy_internal_buffer();
	if (s && n > 0) { _M_buf = s; _M_buf_size = n; }
	else _M_buf_size = 8192;
	return this;
}

template<class C, class T> streampos basic_filebuf<C, T>::seekoff(off_type off, ios_base::seekdir way, ios_base::openmode)
{
	if (!is_open()) return pos_type(off_type(-1));
	if (!flush_out()) return pos_type(off_type(-1));
	if (_M_writing) { _M_writing = false; this->setp(0, 0); }
	drop_in();
	return pos_type(_M_file.seekoff(off, way));
}

template<class C, class T> streampos basic_filebuf<C, T>::seekpos(pos_type pos, ios_base::openmode mode)
{
	return seekoff(pos._M_off, (ios_base::seekdir)ios_base::beg, mode);
}

template<class C, class T> int basic_filebuf<C, T>::sync()
{
	if (!is_open()) return 0;
	int rc = 0;
	if (_M_writing) {
		if (!flush_out()) rc = -1;
		if (_M_file.sync()) rc = -1;
	}
	return rc;
}

template<class C, class T> streamsize basic_filebuf<C, T>::xsgetn(char_type *s, streamsize n)
{
	streamsize done = 0, avail = this->egptr() - this->gptr();
	if (avail > 0) {
		done = avail < n ? avail : n;
		memcpy(s, this->gptr(), done);
		this->gbump(done);
	}
	if (done < n && n - done >= (streamsize)_M_buf_size && (_M_mode & ios_base::in) && is_open() && !_M_writing) {
		streamsize k = _M_file.xsgetn(s + done, n - done);	// big reads skip the buffer
		if (k > 0) done += k;
		this->setg(_M_buf, _M_buf, _M_buf);
		_M_reading = true;
		return done;
	}
	return done + __streambuf_type::xsgetn(s + done, n - done);
}

template<class C, class T> streamsize basic_filebuf<C, T>::xsputn(const char_type *s, streamsize n)
{
	if (n >= (streamsize)_M_buf_size && (_M_mode & (ios_base::out | ios_base::app)) && is_open()) {
		if (_M_reading) drop_in();
		if (!flush_out()) return 0;
		if (!_M_writing) { this->setp(_M_buf, _M_buf + _M_buf_size); _M_writing = true; }
		return _M_file.xsputn(s, n);
	}
	return __streambuf_type::xsputn(s, n);
}

template<class C, class T> class basic_ifstream : public basic_istream<C, T> {
public:
	typedef basic_filebuf<C, T> __filebuf_type;
	basic_ifstream() : basic_istream<C, T>(), _M_filebuf() { this->init(&_M_filebuf); }
	explicit basic_ifstream(const char *name, ios_base::openmode mode = (ios_base::openmode)ios_base::in)
		: basic_istream<C, T>(), _M_filebuf()
	{
		this->init(&_M_filebuf);
		open(name, mode);
	}
	~basic_ifstream() { }
	__filebuf_type *rdbuf() const { return const_cast<__filebuf_type *>(&_M_filebuf); }
	bool is_open() { return _M_filebuf.is_open(); }
	bool is_open() const { return _M_filebuf.is_open(); }
	void open(const char *name, ios_base::openmode mode = (ios_base::openmode)ios_base::in)
	{
		if (!_M_filebuf.open(name, (ios_base::openmode)(mode | ios_base::in))) this->setstate((ios_base::iostate)ios_base::failbit);
		else this->clear();
	}
	void close() { if (!_M_filebuf.close()) this->setstate((ios_base::iostate)ios_base::failbit); }
private:
	__filebuf_type _M_filebuf;
};

template<class C, class T> class basic_ofstream : public basic_ostream<C, T> {
public:
	typedef basic_filebuf<C, T> __filebuf_type;
	basic_ofstream() : basic_ostream<C, T>(), _M_filebuf() { this->init(&_M_filebuf); }
	explicit basic_ofstream(const char *name, ios_base::openmode mode = (ios_base::openmode)(ios_base::out | ios_base::trunc))
		: basic_ostream<C, T>(), _M_filebuf()
	{
		this->init(&_M_filebuf);
		open(name, mode);
	}
	~basic_ofstream() { }
	__filebuf_type *rdbuf() const { return const_cast<__filebuf_type *>(&_M_filebuf); }
	bool is_open() { return _M_filebuf.is_open(); }
	bool is_open() const { return _M_filebuf.is_open(); }
	void open(const char *name, ios_base::openmode mode = (ios_base::openmode)(ios_base::out | ios_base::trunc))
	{
		if (!_M_filebuf.open(name, (ios_base::openmode)(mode | ios_base::out))) this->setstate((ios_base::iostate)ios_base::failbit);
		else this->clear();
	}
	void close() { if (!_M_filebuf.close()) this->setstate((ios_base::iostate)ios_base::failbit); }
private:
	__filebuf_type _M_filebuf;
};

template<class C, class T> class basic_fstream : public basic_iostream<C, T> {
public:
	typedef basic_filebuf<C, T> __filebuf_type;
	basic_fstream() : basic_iostream<C, T>(), _M_filebuf() { this->init(&_M_filebuf); }
	explicit basic_fstream(const char *name, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out))
		: basic_iostream<C, T>(0), _M_filebuf()
	{
		this->init(&_M_filebuf);
		open(name, mode);
	}
	~basic_fstream() { }
	__filebuf_type *rdbuf() const { return const_cast<__filebuf_type *>(&_M_filebuf); }
	bool is_open() { return _M_filebuf.is_open(); }
	bool is_open() const { return _M_filebuf.is_open(); }
	void open(const char *name, ios_base::openmode mode = (ios_base::openmode)(ios_base::in | ios_base::out))
	{
		if (!_M_filebuf.open(name, mode)) this->setstate((ios_base::iostate)ios_base::failbit);
		else this->clear();
	}
	void close() { if (!_M_filebuf.close()) this->setstate((ios_base::iostate)ios_base::failbit); }
private:
	__filebuf_type _M_filebuf;
};

// ---- emit everything GCC's library exported ---------------------------------

template class basic_ios<char, char_traits<char> >;
template class basic_streambuf<char, char_traits<char> >;
template class basic_istream<char, char_traits<char> >;
template class basic_ostream<char, char_traits<char> >;
template class basic_iostream<char, char_traits<char> >;
template class basic_stringbuf<char, char_traits<char>, allocator<char> >;
template class basic_istringstream<char, char_traits<char>, allocator<char> >;
template class basic_ostringstream<char, char_traits<char>, allocator<char> >;
template class basic_stringstream<char, char_traits<char>, allocator<char> >;
template class basic_filebuf<char, char_traits<char> >;
template class basic_ifstream<char, char_traits<char> >;
template class basic_ofstream<char, char_traits<char> >;
template class basic_fstream<char, char_traits<char> >;

template istream &istream::_M_extract(short &);
template istream &istream::_M_extract(unsigned short &);
template istream &istream::_M_extract(int &);
template istream &istream::_M_extract(unsigned int &);
template istream &istream::_M_extract(long &);
template istream &istream::_M_extract(unsigned long &);
template istream &istream::_M_extract(long long &);
template istream &istream::_M_extract(unsigned long long &);
template ostream &ostream::_M_insert(long);
template ostream &ostream::_M_insert(unsigned long);
template ostream &ostream::_M_insert(long long);
template ostream &ostream::_M_insert(unsigned long long);

typedef char_traits<char> TR;
template istream &operator>>(istream &, char &);
template istream &operator>>(istream &, unsigned char &);
template istream &operator>>(istream &, signed char &);
template istream &operator>>(istream &, char *);
template istream &operator>>(istream &, unsigned char *);
template istream &operator>>(istream &, signed char *);
template istream &operator>>(istream &, string &);
template istream &ws(istream &);
template istream &getline(istream &, string &, char);
template istream &getline(istream &, string &);
template ostream &operator<< <TR>(ostream &, char);
template ostream &operator<< <TR>(ostream &, signed char);
template ostream &operator<< <TR>(ostream &, unsigned char);
template ostream &operator<< <TR>(ostream &, const char *);
template ostream &operator<< <TR>(ostream &, const signed char *);
template ostream &operator<< <TR>(ostream &, const unsigned char *);
template ostream &operator<< <char, TR>(ostream &, const char *);
template ostream &operator<<(ostream &, const string &);
template ostream &__ostream_insert(ostream &, const char *, streamsize);
template ostream &endl(ostream &);
template ostream &ends(ostream &);
template ostream &flush(ostream &);

// sizes GCC 4 gave these classes on i386
static_assert(sizeof(ios_base) == 112, "ios_base");
static_assert(sizeof(ios) == 136, "basic_ios");
static_assert(sizeof(streambuf) == 32, "streambuf");
static_assert(sizeof(istream) == 144 && sizeof(ostream) == 140, "istream/ostream");
static_assert(sizeof(basic_stringbuf<char, TR, allocator<char> >) == 40, "stringbuf");
static_assert(sizeof(streampos) == 136, "fpos");

// ---- cin, cout, cerr, clog ----------------------------------------------------

// unbuffered, straight through the guest's stdio, so printf and cout interleave
class stdio_buf : public streambuf {
	FILE *f;
	int last;
public:
	stdio_buf(FILE *fp) : f(fp), last(-1) { }
protected:
	virtual int overflow(int c) { if (c == -1) return fflush(f) ? -1 : 0; return putc(c, f); }
	virtual streamsize xsputn(const char *s, streamsize n) { return fwrite(s, 1, n, f); }
	virtual int sync() { return fflush(f); }
	virtual int underflow() { int c = getc(f); if (c != -1) ungetc(c, f); return c; }
	virtual int uflow() { return last = getc(f); }
	virtual int pbackfail(int c) { c = c == -1 ? last : c; last = -1; return c == -1 ? -1 : ungetc(c, f); }
	virtual streamsize xsgetn(char *s, streamsize n) { streamsize k = fread(s, 1, n, f); if (k > 0) last = (unsigned char)s[k - 1]; return k; }
};

}	// namespace std

using namespace std;

// GCC keeps the four standard streams as raw storage and builds them in
// ios_base::Init, so they work from any image's static initializers.
alignas(4) char cin_mem[sizeof(istream)] __asm__("__ZSt3cin");
alignas(4) char cout_mem[sizeof(ostream)] __asm__("__ZSt4cout");
alignas(4) char cerr_mem[sizeof(ostream)] __asm__("__ZSt4cerr");
alignas(4) char clog_mem[sizeof(ostream)] __asm__("__ZSt4clog");
alignas(4) static char bufs_mem[4][sizeof(stdio_buf)];

ios_base::Init::Init()
{
	if (_S_refcount++) return;
	stdio_buf *in = new (bufs_mem[0]) stdio_buf(__stdinp);
	stdio_buf *out = new (bufs_mem[1]) stdio_buf(__stdoutp);
	stdio_buf *err = new (bufs_mem[2]) stdio_buf(__stderrp);
	stdio_buf *log = new (bufs_mem[3]) stdio_buf(__stderrp);
	istream *ci = new (cin_mem) istream(in);
	ostream *co = new (cout_mem) ostream(out);
	ostream *ce = new (cerr_mem) ostream(err);
	new (clog_mem) ostream(log);
	ci->tie(co);
	ce->tie(co);
	struct F : ios_base { static void unitbuf(ios_base &b) { ((F &)b)._M_flags = (fmtflags)(((F &)b)._M_flags | ios_base::unitbuf); } };
	F::unitbuf(*ce);
}

ios_base::Init::~Init()
{
	if (--_S_refcount) return;
	((ostream *)cout_mem)->flush();
	((ostream *)cerr_mem)->flush();
	((ostream *)clog_mem)->flush();
}

static ios_base::Init init_streams;
