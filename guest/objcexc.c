// Legacy i386 Objective-C exceptions (the "fragile" runtime): @try saves a
// jmp_buf with objc_exception_try_enter and calls _setjmp itself; @throw
// longjmps to the innermost handler. Written from the objc4 runtime's
// documented interface; no Apple code.
void mac32_guest_fatal(const char *what, const char *detail);
void *mac32_exc_top(void);			// per-thread handler list, kept by the host
void mac32_exc_set_top(void *);
void _longjmp(int *, int);
void *objc_msgSend(void *, const char *, ...);

// the compiler's local data: jmp_buf (18 words), then 4 pointers
struct handler { int buf[18]; void *pointers[4]; };	// [0] exception, [1] next

void objc_exception_try_enter(struct handler *h)
{
	h->pointers[0] = 0;
	h->pointers[1] = mac32_exc_top();
	mac32_exc_set_top(h);
}
void objc_exception_try_exit(struct handler *h) { mac32_exc_set_top(h->pointers[1]); }
void *objc_exception_extract(struct handler *h) { return h->pointers[0]; }
int objc_exception_match(void *cls, void *exc)
{
	return exc && cls && ((int (*)(void *, const char *, void *))objc_msgSend)(exc, "isKindOfClass:", cls) & 0xff;
}
void objc_exception_throw(void *exc)
{
	struct handler *h = mac32_exc_top();
	if (!h) {
		const char *why = exc ? ((const char *(*)(void *, const char *))objc_msgSend)(
			((void *(*)(void *, const char *))objc_msgSend)(exc, "description"), "UTF8String") : "nil";
		mac32_guest_fatal("uncaught Objective-C exception", why);
	}
	mac32_exc_set_top(h->pointers[1]);	// the handler is used up
	h->pointers[0] = exc;
	_longjmp(h->buf, 1);
}
void objc_exception_rethrow(void) { mac32_guest_fatal("objc_exception_rethrow", "not supported"); }
