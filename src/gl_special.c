// OpenGL calls that cannot be plain signature thunks: arrays of pointers,
// pointer results, and mapped buffers (legacy GL 2.1 context only) (host mappings sit above 4 GB, so the
// guest writes into a low shadow copy that we push back on flush/unmap).
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <string.h>
#include <pthread.h>
#include "thunks.h"

// array of 32-bit guest pointers -> host array
static const void **ptrs(g32 arr, int n)
{
	const void **h = malloc(sizeof *h * (size_t)(n > 0 ? n : 1));
	for (int i = 0; i < n; i++) {
		g32 p = ((g32 *)G2H(arr))[i];
		h[i] = p ? G2H(p) : NULL;
	}
	return h;
}
static void t_glShaderSource(uint32_t *args, struct hres *r)
{
	(void)r;
	const void **s = ptrs(A(2), (int)A(1));
	glShaderSource(A(0), (GLsizei)A(1), (const GLchar *const *)s, A(3) ? G2H(A(3)) : NULL);
	free(s);
}
static void t_glShaderSourceARB(uint32_t *args, struct hres *r)
{
	(void)r;
	const void **s = ptrs(A(2), (int)A(1));
	glShaderSourceARB((GLhandleARB)(uintptr_t)A(0), (GLsizei)A(1), (const GLcharARB **)s, A(3) ? G2H(A(3)) : NULL);
	free(s);
}
static void t_glMultiDrawElements(uint32_t *args, struct hres *r)
{
	(void)r;
	// index "pointers" are offsets when an element buffer is bound: keep the values
	int n = (int)A(4);
	const void **h = malloc(sizeof *h * (size_t)(n > 0 ? n : 1));
	for (int i = 0; i < n; i++) h[i] = (const void *)(uintptr_t)((g32 *)G2H(A(3)))[i];
	glMultiDrawElements(A(0), G2H(A(1)), A(2), (const GLvoid *const *)h, n);
	free(h);
}
static void t_glTransformFeedbackVaryingsEXT(uint32_t *args, struct hres *r)
{
	(void)r;
	const void **s = ptrs(A(2), (int)A(1));
	glTransformFeedbackVaryingsEXT(A(0), (GLsizei)A(1), (const GLchar *const *)s, A(3));
	free(s);
}
static void t_glGetAttachedObjectsARB(uint32_t *args, struct hres *r)
{
	(void)r;
	int max = (int)A(1);
	GLhandleARB h[256];
	GLsizei n = 0;
	glGetAttachedObjectsARB((GLhandleARB)(uintptr_t)A(0), max < 256 ? max : 256, &n, h);
	if (A(2)) *(int32_t *)G2H(A(2)) = n;
	for (int i = 0; i < n; i++) ((uint32_t *)G2H(A(3)))[i] = (uint32_t)(uintptr_t)h[i];
}
// pointer queries: the values were set by the guest, so they fit in 32 bits
static void put_ptr(g32 dst, void *p) { *(g32 *)G2H(dst) = (g32)(uintptr_t)p; }
static void t_glGetPointerv(uint32_t *args, struct hres *r) { (void)r; void *p = NULL; glGetPointerv(A(0), &p); put_ptr(A(1), p); }
static void t_glGetVertexAttribPointerv(uint32_t *args, struct hres *r) { (void)r; void *p = NULL; glGetVertexAttribPointerv(A(0), A(1), &p); put_ptr(A(2), p); }

// glGetString results are kept for good: programs hold on to them
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static g32 strcache[64][2];
static int nstr;
static g32 keep_string(uint32_t key, const char *s)
{
	if (!s) return 0;
	pthread_mutex_lock(&lk);
	for (int i = 0; i < nstr; i++) if (strcache[i][0] == key) { pthread_mutex_unlock(&lk); return strcache[i][1]; }
	g32 g = gstrdup(s);
	if (nstr < 64) { strcache[nstr][0] = key; strcache[nstr][1] = g; nstr++; }
	pthread_mutex_unlock(&lk);
	return g;
}
static void t_glGetString(uint32_t *args, struct hres *r) { RET(keep_string(A(0), (const char *)glGetString(A(0)))); }

// ---- mapped buffers ---------------------------------------------------------
// One shadow per buffer object name, kept between maps and freed when the
// buffer is deleted. (Mapping state is per context in GL; the game uses one.)
struct mapping { GLuint buf; void *host; g32 shadow; uint32_t cap, size, off; int explicit_flush, flushed, write; };
static struct mapping *maps;
static uint32_t nmaps, capmaps;

static GLenum binding_of(GLenum target)
{
	switch (target) {
	case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
	case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
	case GL_PIXEL_PACK_BUFFER: return GL_PIXEL_PACK_BUFFER_BINDING;
	case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
	default: die("glMapBuffer: unknown target %#x", target);
	}
}
static struct mapping *find_buf(GLuint b)
{
	for (uint32_t i = 0; i < nmaps; i++) if (maps[i].buf == b) return &maps[i];
	return NULL;
}
static struct mapping *mapping_for(GLenum target, int create)
{
	GLint b = 0;
	glGetIntegerv(binding_of(target), &b);
	struct mapping *m = find_buf((GLuint)b);
	if (m || !create) return m;
	if (nmaps == capmaps) {
		capmaps = capmaps ? capmaps * 2 : 256;
		maps = realloc(maps, sizeof *maps * capmaps);
	}
	m = &maps[nmaps++];
	memset(m, 0, sizeof *m);
	m->buf = (GLuint)b;
	return m;
}
static void forget_buffers(GLsizei n, const GLuint *names)
{
	for (GLsizei i = 0; i < n; i++) {
		struct mapping *m = find_buf(names[i]);
		if (!m) continue;
		gfree(m->shadow);
		*m = maps[--nmaps];
	}
}
static void t_glDeleteBuffers(uint32_t *args, struct hres *r)
{
	(void)r;
	forget_buffers((GLsizei)A(0), G2H(A(1)));
	glDeleteBuffers((GLsizei)A(0), G2H(A(1)));
}
static void t_glDeleteBuffersARB(uint32_t *args, struct hres *r)
{
	(void)r;
	forget_buffers((GLsizei)A(0), G2H(A(1)));
	glDeleteBuffersARB((GLsizei)A(0), G2H(A(1)));
}
static g32 shadow_map(GLenum target, void *host, uint32_t off, uint32_t size, int read, int write, int explicit_flush)
{
	if (!host) return 0;
	struct mapping *m = mapping_for(target, 1);
	if (m->cap < size) {
		gfree(m->shadow);
		m->cap = size;
		m->shadow = gmalloc(size ? size : 16);
	}
	m->host = host;
	m->size = size;
	m->off = off;
	m->write = write;
	m->explicit_flush = explicit_flush;
	m->flushed = 0;
	if (read) memcpy(G2H(m->shadow), host, size);
	return m->shadow;
}
static void t_glMapBuffer(uint32_t *args, struct hres *r)
{
	GLint size = 0, flushable = GL_TRUE;
	glGetBufferParameteriv(A(0), GL_BUFFER_SIZE, &size);
	glGetBufferParameteriv(A(0), GL_BUFFER_FLUSHING_UNMAP_APPLE, &flushable);
	void *h = glMapBuffer(A(0), A(1));
	RET(shadow_map(A(0), h, 0, (uint32_t)size, A(1) != GL_WRITE_ONLY, A(1) != GL_READ_ONLY, !flushable));
}
static void t_glFlushMappedBufferRangeAPPLE(uint32_t *args, struct hres *r)
{
	(void)r;
	struct mapping *m = mapping_for(A(0), 0);
	if (!m) return;
	memcpy((char *)m->host + A(1), (char *)G2H(m->shadow) + A(1), A(2));
	m->flushed = 1;
	glFlushMappedBufferRangeAPPLE(A(0), (GLintptr)A(1), (GLsizeiptr)A(2));
}
static void t_glUnmapBuffer(uint32_t *args, struct hres *r)
{
	struct mapping *m = mapping_for(A(0), 0);
	if (m && m->write && !(m->explicit_flush && m->flushed))
		memcpy(m->host, G2H(m->shadow), m->size);
	if (m) m->host = NULL;
	RET(glUnmapBuffer(A(0)));
}
static void t_glGetBufferPointerv(uint32_t *args, struct hres *r)
{
	(void)r;
	struct mapping *m = mapping_for(A(0), 0);
	*(g32 *)G2H(A(2)) = m && m->host ? m->shadow : 0;
}

static struct hfn gls[] = {
	R("_glShaderSource", t_glShaderSource), R("_glShaderSourceARB", t_glShaderSourceARB),
	R("_glMultiDrawElements", t_glMultiDrawElements), R("_glMultiDrawElementsEXT", t_glMultiDrawElements),
	R("_glTransformFeedbackVaryingsEXT", t_glTransformFeedbackVaryingsEXT),
	R("_glGetAttachedObjectsARB", t_glGetAttachedObjectsARB),
	R("_glGetPointerv", t_glGetPointerv), R("_glGetPointervEXT", t_glGetPointerv),
	R("_glGetVertexAttribPointerv", t_glGetVertexAttribPointerv),
	R("_glGetVertexAttribPointervARB", t_glGetVertexAttribPointerv),
	R("_glGetString", t_glGetString),
	R("_glMapBuffer", t_glMapBuffer), R("_glMapBufferARB", t_glMapBuffer),
	R("_glFlushMappedBufferRangeAPPLE", t_glFlushMappedBufferRangeAPPLE),
	R("_glUnmapBuffer", t_glUnmapBuffer), R("_glUnmapBufferARB", t_glUnmapBuffer),
	R("_glDeleteBuffers", t_glDeleteBuffers), R("_glDeleteBuffersARB", t_glDeleteBuffersARB),
	R("_glGetBufferPointerv", t_glGetBufferPointerv), R("_glGetBufferPointervARB", t_glGetBufferPointerv),
};

void thunks_gl_gen_init(void);
void thunks_gl_init(void) { REGISTER(gls); thunks_gl_gen_init(); }
