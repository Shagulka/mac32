// zlib for the guest. i386 z_stream is 56 bytes (4-byte pointers and uLongs);
// each guest stream gets a host twin, synced before and after every call.
// Guest zalloc/zfree hooks are ignored: zlib's default allocator is used.
#include "thunks.h"
#include <zlib.h>
#include <string.h>
#include <pthread.h>

enum { NEXT_IN = 0, AVAIL_IN = 4, TOTAL_IN = 8, NEXT_OUT = 12, AVAIL_OUT = 16, TOTAL_OUT = 20,
       MSG = 24, STATE = 28, DATA_TYPE = 44, ADLER = 48 };
#define F(g, off) (*(uint32_t *)G2H((g) + (off)))

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static struct { g32 g; z_stream *h; } twins[256];

static z_stream *twin(g32 g, int create)
{
	pthread_mutex_lock(&lk);
	z_stream *h = NULL;
	for (int i = 0; i < 256 && !h; i++) if (twins[i].g == g && twins[i].h) h = twins[i].h;
	if (!h && create)
		for (int i = 0; i < 256; i++)
			if (!twins[i].h) { h = calloc(1, sizeof *h); twins[i].g = g; twins[i].h = h; break; }
	pthread_mutex_unlock(&lk);
	if (!h && create) die("too many zlib streams");
	return h;
}
static void drop(g32 g)
{
	pthread_mutex_lock(&lk);
	for (int i = 0; i < 256; i++) if (twins[i].g == g && twins[i].h) { free(twins[i].h); twins[i].h = NULL; twins[i].g = 0; }
	pthread_mutex_unlock(&lk);
}
static void to_host(g32 g, z_stream *h)
{
	h->next_in = F(g, NEXT_IN) ? G2H(F(g, NEXT_IN)) : NULL;
	h->avail_in = F(g, AVAIL_IN);
	h->total_in = F(g, TOTAL_IN);
	h->next_out = F(g, NEXT_OUT) ? G2H(F(g, NEXT_OUT)) : NULL;
	h->avail_out = F(g, AVAIL_OUT);
	h->total_out = F(g, TOTAL_OUT);
}
static void to_guest(g32 g, z_stream *h)
{
	F(g, NEXT_IN) = h->next_in ? H2G(h->next_in) : 0;
	F(g, AVAIL_IN) = h->avail_in;
	F(g, TOTAL_IN) = (uint32_t)h->total_in;
	F(g, NEXT_OUT) = h->next_out ? H2G(h->next_out) : 0;
	F(g, AVAIL_OUT) = h->avail_out;
	F(g, TOTAL_OUT) = (uint32_t)h->total_out;
	F(g, MSG) = h->msg ? guest_cstr(h->msg) : 0;
	F(g, STATE) = h->state ? 0x5a5a5a5a : 0;	// nonzero while the stream lives
	F(g, DATA_TYPE) = (uint32_t)h->data_type;
	F(g, ADLER) = (uint32_t)h->adler;
}
#define CALL(expr) do { z_stream *h = twin(A(0), 1); to_host(A(0), h); int rc = (expr); to_guest(A(0), h); RET(rc); } while (0)
static void t_deflateInit2_(uint32_t *args, struct hres *r)
{ CALL(deflateInit2_(h, (int)A(1), (int)A(2), (int)A(3), (int)A(4), (int)A(5), G2H(A(6)), sizeof(z_stream))); }
static void t_deflateInit_(uint32_t *args, struct hres *r) { CALL(deflateInit_(h, (int)A(1), G2H(A(2)), sizeof(z_stream))); }
static void t_inflateInit_(uint32_t *args, struct hres *r) { CALL(inflateInit_(h, G2H(A(1)), sizeof(z_stream))); }
static void t_inflateInit2_(uint32_t *args, struct hres *r) { CALL(inflateInit2_(h, (int)A(1), G2H(A(2)), sizeof(z_stream))); }
static void t_deflate(uint32_t *args, struct hres *r) { CALL(deflate(h, (int)A(1))); }
static void t_inflate(uint32_t *args, struct hres *r) { CALL(inflate(h, (int)A(1))); }
static void t_deflateReset(uint32_t *args, struct hres *r) { CALL(deflateReset(h)); }
static void t_inflateReset(uint32_t *args, struct hres *r) { CALL(inflateReset(h)); }
static void t_deflateEnd(uint32_t *args, struct hres *r) { CALL(deflateEnd(h)); drop(A(0)); }
static void t_inflateEnd(uint32_t *args, struct hres *r) { CALL(inflateEnd(h)); drop(A(0)); }
// one-shot helpers: uLongf* lengths are 4 bytes in the guest
static void t_compress(uint32_t *args, struct hres *r)
{
	uLongf n = *(uint32_t *)G2H(A(1));
	int rc = compress(G2H(A(0)), &n, G2H(A(2)), A(3));
	*(uint32_t *)G2H(A(1)) = (uint32_t)n;
	RET(rc);
}
static void t_compress2(uint32_t *args, struct hres *r)
{
	uLongf n = *(uint32_t *)G2H(A(1));
	int rc = compress2(G2H(A(0)), &n, G2H(A(2)), A(3), (int)A(4));
	*(uint32_t *)G2H(A(1)) = (uint32_t)n;
	RET(rc);
}
static void t_uncompress(uint32_t *args, struct hres *r)
{
	uLongf n = *(uint32_t *)G2H(A(1));
	int rc = uncompress(G2H(A(0)), &n, G2H(A(2)), A(3));
	*(uint32_t *)G2H(A(1)) = (uint32_t)n;
	RET(rc);
}
static uint32_t w_crc32(uint32_t c, const Bytef *b, uint32_t n) { return (uint32_t)crc32(c, b, n); }
static uint32_t w_adler32(uint32_t c, const Bytef *b, uint32_t n) { return (uint32_t)adler32(c, b, n); }
static uint32_t w_compressBound(uint32_t n) { return (uint32_t)compressBound(n); }

static struct hfn z[] = {
	R("_deflateInit2_", t_deflateInit2_), R("_deflateInit_", t_deflateInit_),
	R("_inflateInit_", t_inflateInit_), R("_inflateInit2_", t_inflateInit2_),
	R("_deflate", t_deflate), R("_inflate", t_inflate),
	R("_deflateReset", t_deflateReset), R("_inflateReset", t_inflateReset),
	R("_deflateEnd", t_deflateEnd), R("_inflateEnd", t_inflateEnd),
	R("_compress", t_compress), R("_compress2", t_compress2), R("_uncompress", t_uncompress),
	G("_crc32", w_crc32, "u:upu"), G("_adler32", w_adler32, "u:upu"), G("_compressBound", w_compressBound, "u:u"),
};
void thunks_zlib_init(void) { REGISTER(z); }
