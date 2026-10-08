// printf for guest varargs. On i386 every argument sits on the stack: int,
// long, pointers and size_t take 4 bytes; long long and double 8; long double
// 12. We walk the format, pull each argument at its i386 size, and let the
// host snprintf format one conversion at a time.
#include "mac32.h"
#include "thunks.h"
#include <string.h>
#include <wchar.h>

struct buf { char *p; int len, cap; };

static void put(struct buf *b, const char *s, int n)
{
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		b->p = realloc(b->p, (size_t)b->cap);
	}
	memcpy(b->p + b->len, s, (size_t)n);
	b->len += n;
	b->p[b->len] = 0;
}

char *gvformat(const char *fmt, const uint32_t *ap, int *len)
{
	struct buf b = {0};
	put(&b, "", 0);
	while (*fmt) {
		const char *pct = strchr(fmt, '%');
		if (!pct) { put(&b, fmt, (int)strlen(fmt)); break; }
		put(&b, fmt, (int)(pct - fmt));
		const char *s = pct + 1;
		char spec[64];
		int n = 0;
		spec[n++] = '%';
		while (strchr("-+ #0'", *s) && n < 40) spec[n++] = *s++;
		if (*s == '*') { n += snprintf(spec + n, 12, "%d", (int32_t)*ap++); s++; }
		else while (*s >= '0' && *s <= '9' && n < 40) spec[n++] = *s++;
		if (*s == '.') {
			spec[n++] = *s++;
			if (*s == '*') { n += snprintf(spec + n, 12, "%d", (int32_t)*ap++); s++; }
			else while (*s >= '0' && *s <= '9' && n < 50) spec[n++] = *s++;
		}
		int lng = 0;		// 0 int, 1 long, 2 long long, 3 long double, -1 short, -2 char
		for (;; s++) {
			if (*s == 'h') lng = lng == -1 ? -2 : -1;
			else if (*s == 'l') lng = lng == 1 ? 2 : 1;
			else if (*s == 'q' || *s == 'j') lng = 2;
			else if (*s == 'L') lng = 3;
			else if (*s == 'z' || *s == 't') lng = 0;
			else break;
		}
		char c = *s ? *s++ : 0;
		char out[512], *o = out;
		int on = 0;
		switch (c) {
		case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c':
			if (lng == 2) {
				uint64_t v = ap[0] | (uint64_t)ap[1] << 32;
				ap += 2;
				spec[n++] = 'l'; spec[n++] = 'l'; spec[n++] = c; spec[n] = 0;
				on = snprintf(out, sizeof out, spec, v);
			} else {
				uint32_t v = *ap++;
				if (c == 'c' && lng == 1) { spec[n++] = 'l'; }
				spec[n++] = c; spec[n] = 0;
				if (c == 'd' || c == 'i') {
					int32_t sv = (int32_t)v;
					if (lng == -1) sv = (int16_t)sv;
					if (lng == -2) sv = (int8_t)sv;
					on = snprintf(out, sizeof out, spec, sv);
				} else {
					if (lng == -1) v = (uint16_t)v;
					if (lng == -2) v = (uint8_t)v;
					on = snprintf(out, sizeof out, spec, v);
				}
			}
			break;
		case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
			if (lng == 3) {
				long double v = 0;
				memcpy(&v, ap, 10);
				ap += 3;
				spec[n++] = 'L'; spec[n++] = c; spec[n] = 0;
				on = snprintf(out, sizeof out, spec, v);
			} else {
				double v;
				memcpy(&v, ap, 8);
				ap += 2;
				spec[n++] = c; spec[n] = 0;
				on = snprintf(out, sizeof out, spec, v);
			}
			break;
		case 's': case 'S': {
			g32 p = *ap++;
			spec[n++] = (c == 'S' || lng == 1) ? 'l' : 's';
			if (spec[n - 1] == 'l') spec[n++] = 's';
			spec[n] = 0;
			int need = p ? snprintf(NULL, 0, spec, G2H(p)) : snprintf(NULL, 0, spec, "(null)");
			if (need >= (int)sizeof out) o = malloc((size_t)need + 1);
			on = p ? snprintf(o, (size_t)need + 1, spec, G2H(p))
			       : snprintf(o, (size_t)need + 1, spec, "(null)");
			break;
		}
		case 'p':
			on = snprintf(out, sizeof out, "0x%x", *ap++);
			break;
		case 'n': {
			g32 p = *ap++;
			if (lng == 2) *(int64_t *)G2H(p) = b.len;
			else if (lng == -1) *(int16_t *)G2H(p) = (int16_t)b.len;
			else if (lng == -2) *(int8_t *)G2H(p) = (int8_t)b.len;
			else *(int32_t *)G2H(p) = b.len;
			break;
		}
		case '%':
			out[0] = '%';
			on = 1;
			break;
		default:	// unknown: copy it through as written
			on = snprintf(out, sizeof out, "%.*s", (int)(s - pct), pct);
			break;
		}
		if (on > 0) put(&b, o, on);
		if (o != out) free(o);
		fmt = s;
	}
	if (len) *len = b.len;
	return b.p;
}
