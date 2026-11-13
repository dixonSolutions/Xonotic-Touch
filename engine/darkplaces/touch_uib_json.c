/*
Godnotic UI bridge: JSON writing and a small strict parser (RFC 8259, UTF-8).
No engine dependencies, so the tests can build it alone.
*/
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "touch_uib_int.h"

// ---------------------------------------------------------------- buffer

void UIBB_Reserve(uibbuf_t *b, size_t more)
{
	if (b->n + more + 1 <= b->cap)
		return;
	size_t cap = b->cap ? b->cap : 256;
	while (cap < b->n + more + 1)
		cap *= 2;
	char *p = (char *)realloc(b->p, cap);
	if (!p)
		abort();
	b->p = p;
	b->cap = cap;
}

void UIBB_Put(uibbuf_t *b, const char *s, size_t len)
{
	UIBB_Reserve(b, len);
	memcpy(b->p + b->n, s, len);
	b->n += len;
	b->p[b->n] = 0;
}

void UIBB_Puts(uibbuf_t *b, const char *s) { UIBB_Put(b, s, strlen(s)); }

void UIBB_Putc(uibbuf_t *b, char c)
{
	UIBB_Reserve(b, 1);
	b->p[b->n++] = c;
	b->p[b->n] = 0;
}

void UIBB_Printf(uibbuf_t *b, const char *fmt, ...)
{
	va_list ap;
	char tmp[512];
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(tmp))
	{
		UIBB_Put(b, tmp, n);
		return;
	}
	UIBB_Reserve(b, n);
	va_start(ap, fmt);
	vsnprintf(b->p + b->n, n + 1, fmt, ap);
	va_end(ap);
	b->n += n;
}

void UIBB_Num(uibbuf_t *b, double v)
{
	if (!isfinite(v))
		v = 0;
	if (v == (double)(long long)v && fabs(v) < 9007199254740992.0)
		UIBB_Printf(b, "%lld", (long long)v);
	else
		UIBB_Printf(b, "%.9g", v);
}

char *UIBB_Detach(uibbuf_t *b, size_t *len)
{
	if (!b->p)
		UIBB_Reserve(b, 0), b->p[0] = 0;
	char *p = b->p;
	if (len)
		*len = b->n;
	b->p = NULL;
	b->n = b->cap = 0;
	return p;
}

void UIBB_Free(uibbuf_t *b)
{
	free(b->p);
	b->p = NULL;
	b->n = b->cap = 0;
}

// Length of the valid UTF-8 sequence at s (1..4), 0 if invalid.
static size_t utf8_seq(const unsigned char *s, size_t avail, unsigned *cp)
{
	unsigned c = s[0];
	if (c < 0x80) { *cp = c; return 1; }
	size_t n;
	unsigned v;
	if ((c & 0xe0) == 0xc0) { n = 2; v = c & 0x1f; }
	else if ((c & 0xf0) == 0xe0) { n = 3; v = c & 0x0f; }
	else if ((c & 0xf8) == 0xf0) { n = 4; v = c & 0x07; }
	else return 0;
	if (n > avail)
		return 0;
	for (size_t i = 1; i < n; i++)
	{
		if ((s[i] & 0xc0) != 0x80)
			return 0;
		v = (v << 6) | (s[i] & 0x3f);
	}
	if ((n == 2 && v < 0x80) || (n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10ffff)) || (v >= 0xd800 && v <= 0xdfff))
		return 0;
	*cp = v;
	return n;
}

size_t UIB_CleanText(const char *in, size_t inlen, char *out, size_t outsize)
{
	const unsigned char *s = (const unsigned char *)in;
	size_t o = 0, i = 0;
	if (!outsize)
		return 0;
	while (i < inlen && s[i])
	{
		unsigned cp;
		if (s[i] == '^' && i + 1 < inlen)
		{
			unsigned char d = s[i + 1];
			if (d >= '0' && d <= '9') { i += 2; continue; }
			if (d == '^') { if (o + 1 < outsize) out[o++] = '^'; i += 2; continue; }
			if (d == 'x' && i + 4 < inlen && isxdigit(s[i + 2]) && isxdigit(s[i + 3]) && isxdigit(s[i + 4])) { i += 5; continue; }
		}
		size_t n = utf8_seq(s + i, inlen - i, &cp);
		if (!n)
		{
			// a Latin-1 or Quake byte: map the Quake charset's coloured letters to ASCII
			unsigned char c = s[i] & 0x7f;
			if (c >= 0x20 && c < 0x7f && o + 1 < outsize)
				out[o++] = c;
			i++;
			continue;
		}
		if (cp >= 0xe000 && cp <= 0xe0ff)
		{
			// DarkPlaces maps the Quake charset to U+E000..U+E0FF
			unsigned c = cp & 0x7f;
			if (c >= 0x20 && c < 0x7f)
			{
				if (o + 1 < outsize)
					out[o++] = (char)c;
			}
			else if ((c >= 0x10 && c <= 0x11) || c == 0x1d || c == 0x1e || c == 0x1f)
			{
				if (o + 1 < outsize)
					out[o++] = c == 0x10 ? '[' : c == 0x11 ? ']' : '-';
			}
			i += n;
			continue;
		}
		if (cp < 0x20 && cp != '\n' && cp != '\t')
		{
			i += n;
			continue;
		}
		if (o + n >= outsize)
			break;
		memcpy(out + o, s + i, n);
		o += n;
		i += n;
	}
	out[o] = 0;
	return o;
}

void UIBB_Str(uibbuf_t *b, const char *s, size_t len, size_t maxbytes, int strip)
{
	static const char hex[] = "0123456789abcdef";
	char *clean = NULL;
	if (strip)
	{
		clean = (char *)malloc(len + 1);
		len = UIB_CleanText(s, len, clean, len + 1);
		s = clean;
	}
	const unsigned char *u = (const unsigned char *)s;
	size_t used = 0;
	UIBB_Reserve(b, len + 8);
	UIBB_Putc(b, '"');
	for (size_t i = 0; i < len && u[i];)
	{
		unsigned cp;
		size_t n = utf8_seq(u + i, len - i, &cp);
		if (!n)
		{
			// invalid byte: U+FFFD
			if (maxbytes && used + 3 > maxbytes) break;
			UIBB_Put(b, "\xef\xbf\xbd", 3);
			used += 3;
			i++;
			continue;
		}
		if (maxbytes && used + n > maxbytes)
			break;
		used += n;
		if (cp == '"') UIBB_Put(b, "\\\"", 2);
		else if (cp == '\\') UIBB_Put(b, "\\\\", 2);
		else if (cp == '\n') UIBB_Put(b, "\\n", 2);
		else if (cp == '\t') UIBB_Put(b, "\\t", 2);
		else if (cp == '\r') UIBB_Put(b, "\\r", 2);
		else if (cp < 0x20 || cp == 0x2028 || cp == 0x2029)
		{
			char e[6] = {'\\', 'u', hex[(cp >> 12) & 15], hex[(cp >> 8) & 15], hex[(cp >> 4) & 15], hex[cp & 15]};
			UIBB_Put(b, e, 6);
		}
		else
			UIBB_Put(b, (const char *)u + i, n);
		i += n;
	}
	UIBB_Putc(b, '"');
	free(clean);
}

// ---------------------------------------------------------------- parser

typedef struct uibjblock_s
{
	struct uibjblock_s *next;
	size_t used, size;
	char data[1];
} uibjblock_t;

struct uibjdoc_s
{
	uibjblock_t *blocks;
	uibj_t *root;
};

typedef struct
{
	const char *s, *end;
	uibjdoc_t *doc;
	char *err;
	size_t errlen;
	int depth;
} jparse_t;

static void *jalloc(uibjdoc_t *d, size_t n)
{
	n = (n + 7) & ~(size_t)7;
	if (!d->blocks || d->blocks->used + n > d->blocks->size)
	{
		size_t sz = n > 32768 ? n : 32768;
		uibjblock_t *b = (uibjblock_t *)malloc(sizeof(uibjblock_t) + sz);
		if (!b)
			abort();
		b->next = d->blocks;
		b->used = 0;
		b->size = sz;
		d->blocks = b;
	}
	void *p = d->blocks->data + d->blocks->used;
	d->blocks->used += n;
	memset(p, 0, n);
	return p;
}

static int jfail(jparse_t *p, const char *what)
{
	if (p->err && p->errlen)
		snprintf(p->err, p->errlen, "%s", what);
	return 0;
}

static void jws(jparse_t *p)
{
	while (p->s < p->end && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r'))
		p->s++;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void put_utf8(char **o, unsigned cp)
{
	char *q = *o;
	if (cp < 0x80) *q++ = (char)cp;
	else if (cp < 0x800) { *q++ = (char)(0xc0 | (cp >> 6)); *q++ = (char)(0x80 | (cp & 0x3f)); }
	else if (cp < 0x10000) { *q++ = (char)(0xe0 | (cp >> 12)); *q++ = (char)(0x80 | ((cp >> 6) & 0x3f)); *q++ = (char)(0x80 | (cp & 0x3f)); }
	else { *q++ = (char)(0xf0 | (cp >> 18)); *q++ = (char)(0x80 | ((cp >> 12) & 0x3f)); *q++ = (char)(0x80 | ((cp >> 6) & 0x3f)); *q++ = (char)(0x80 | (cp & 0x3f)); }
	*o = q;
}

static int jstring(jparse_t *p, const char **out, size_t *outlen)
{
	// p->s at the opening quote
	const char *s = ++p->s;
	size_t maxlen = 0;
	while (s < p->end && *s != '"')
	{
		if (*s == '\\') s++;
		if ((unsigned char)*s < 0x20) return jfail(p, "control character in string");
		s++, maxlen++;
	}
	if (s >= p->end)
		return jfail(p, "unterminated string");
	char *buf = (char *)jalloc(p->doc, (size_t)(s - p->s) * 2 + 1);
	char *o = buf;
	while (p->s < s)
	{
		char c = *p->s++;
		if (c != '\\')
		{
			*o++ = c;
			continue;
		}
		c = *p->s++;
		switch (c)
		{
		case '"': *o++ = '"'; break;
		case '\\': *o++ = '\\'; break;
		case '/': *o++ = '/'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'n': *o++ = '\n'; break;
		case 'r': *o++ = '\r'; break;
		case 't': *o++ = '\t'; break;
		case 'u':
		{
			if (s - p->s < 4) return jfail(p, "bad \\u escape");
			int h0 = hexval(p->s[0]), h1 = hexval(p->s[1]), h2 = hexval(p->s[2]), h3 = hexval(p->s[3]);
			if ((h0 | h1 | h2 | h3) < 0) return jfail(p, "bad \\u escape");
			unsigned cp = (unsigned)(h0 << 12 | h1 << 8 | h2 << 4 | h3);
			p->s += 4;
			if (cp >= 0xd800 && cp <= 0xdbff && s - p->s >= 6 && p->s[0] == '\\' && p->s[1] == 'u')
			{
				int g0 = hexval(p->s[2]), g1 = hexval(p->s[3]), g2 = hexval(p->s[4]), g3 = hexval(p->s[5]);
				unsigned lo = (unsigned)(g0 << 12 | g1 << 8 | g2 << 4 | g3);
				if ((g0 | g1 | g2 | g3) >= 0 && lo >= 0xdc00 && lo <= 0xdfff)
				{
					cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
					p->s += 6;
				}
				else cp = 0xfffd;
			}
			else if (cp >= 0xd800 && cp <= 0xdfff)
				cp = 0xfffd;
			if (cp == 0) cp = 0xfffd;  // no NULs inside our strings
			put_utf8(&o, cp);
			break;
		}
		default:
			return jfail(p, "bad escape");
		}
	}
	*o = 0;
	p->s = s + 1;
	*out = buf;
	*outlen = (size_t)(o - buf);
	return 1;
}

static uibj_t *jvalue(jparse_t *p);

static uibj_t *jvalue(jparse_t *p)
{
	jws(p);
	if (p->s >= p->end) { jfail(p, "unexpected end"); return NULL; }
	uibj_t *v = (uibj_t *)jalloc(p->doc, sizeof(uibj_t));
	v->raw = p->s;
	char c = *p->s;
	if (c == '{' || c == '[')
	{
		if (++p->depth > 64) { jfail(p, "too deep"); return NULL; }
		int obj = c == '{';
		v->type = obj ? UIBJ_OBJ : UIBJ_ARR;
		p->s++;
		jws(p);
		uibj_t **tail = &v->child;
		if (p->s < p->end && *p->s == (obj ? '}' : ']'))
			p->s++;
		else
		{
			for (;;)
			{
				const char *key = NULL;
				size_t klen;
				if (obj)
				{
					jws(p);
					if (p->s >= p->end || *p->s != '"') { jfail(p, "expected key"); return NULL; }
					if (!jstring(p, &key, &klen)) return NULL;
					jws(p);
					if (p->s >= p->end || *p->s != ':') { jfail(p, "expected ':'"); return NULL; }
					p->s++;
				}
				uibj_t *m = jvalue(p);
				if (!m) return NULL;
				m->key = key;
				*tail = m;
				tail = &m->next;
				v->len++;
				jws(p);
				if (p->s < p->end && *p->s == ',') { p->s++; continue; }
				if (p->s < p->end && *p->s == (obj ? '}' : ']')) { p->s++; break; }
				jfail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
				return NULL;
			}
		}
		p->depth--;
	}
	else if (c == '"')
	{
		v->type = UIBJ_STR;
		if (!jstring(p, &v->str, &v->len)) return NULL;
	}
	else if (c == 't' && p->end - p->s >= 4 && !memcmp(p->s, "true", 4)) { v->type = UIBJ_TRUE; p->s += 4; }
	else if (c == 'f' && p->end - p->s >= 5 && !memcmp(p->s, "false", 5)) { v->type = UIBJ_FALSE; p->s += 5; }
	else if (c == 'n' && p->end - p->s >= 4 && !memcmp(p->s, "null", 4)) { v->type = UIBJ_NULL; p->s += 4; }
	else if (c == '-' || (c >= '0' && c <= '9'))
	{
		const char *q = p->s;
		if (*q == '-') q++;
		if (q >= p->end || !(*q >= '0' && *q <= '9')) { jfail(p, "bad number"); return NULL; }
		while (q < p->end && ((*q >= '0' && *q <= '9') || *q == '.' || *q == 'e' || *q == 'E' || *q == '+' || *q == '-'))
			q++;
		char tmp[64];
		size_t n = (size_t)(q - p->s);
		if (n >= sizeof(tmp)) { jfail(p, "number too long"); return NULL; }
		memcpy(tmp, p->s, n);
		tmp[n] = 0;
		char *endp;
		v->num = strtod(tmp, &endp);
		if (*endp) { jfail(p, "bad number"); return NULL; }
		v->type = UIBJ_NUM;
		p->s = q;
	}
	else
	{
		jfail(p, "unexpected character");
		return NULL;
	}
	v->rawlen = (size_t)(p->s - v->raw);
	return v;
}

uibjdoc_t *UIBJ_Parse(const char *s, size_t len, char *err, size_t errlen)
{
	uibjdoc_t *d = (uibjdoc_t *)calloc(1, sizeof(*d));
	jparse_t p = {s, s + len, d, err, errlen, 0};
	if (err && errlen)
		err[0] = 0;
	d->root = jvalue(&p);
	if (d->root)
	{
		jws(&p);
		if (p.s != p.end)
		{
			jfail(&p, "trailing characters");
			d->root = NULL;
		}
	}
	if (!d->root)
	{
		if (err && errlen && !err[0])
			snprintf(err, errlen, "parse error");
		else if (err && errlen)
		{
			// put the offset in
			char tmp[160];
			snprintf(tmp, sizeof(tmp), "%.120s (byte %d)", err, (int)(p.s - s));
			snprintf(err, errlen, "%s", tmp);
		}
		UIBJ_Free(d);
		return NULL;
	}
	return d;
}

uibj_t *UIBJ_Root(uibjdoc_t *d) { return d ? d->root : NULL; }

void UIBJ_Free(uibjdoc_t *d)
{
	if (!d)
		return;
	while (d->blocks)
	{
		uibjblock_t *n = d->blocks->next;
		free(d->blocks);
		d->blocks = n;
	}
	free(d);
}

uibj_t *UIBJ_Get(const uibj_t *obj, const char *key)
{
	if (!obj || obj->type != UIBJ_OBJ)
		return NULL;
	for (uibj_t *m = obj->child; m; m = m->next)
		if (!strcmp(m->key, key))
			return m;
	return NULL;
}

uibj_t *UIBJ_At(const uibj_t *arr, size_t i)
{
	if (!arr || (arr->type != UIBJ_ARR && arr->type != UIBJ_OBJ))
		return NULL;
	uibj_t *m = arr->child;
	while (m && i--)
		m = m->next;
	return m;
}

const char *UIBJ_Str(const uibj_t *obj, const char *key, const char *def)
{
	uibj_t *v = UIBJ_Get(obj, key);
	return v && v->type == UIBJ_STR ? v->str : def;
}

double UIBJ_Num(const uibj_t *obj, const char *key, double def)
{
	uibj_t *v = UIBJ_Get(obj, key);
	return v && v->type == UIBJ_NUM ? v->num : def;
}

int UIBJ_Bool(const uibj_t *obj, const char *key, int def)
{
	uibj_t *v = UIBJ_Get(obj, key);
	if (!v) return def;
	if (v->type == UIBJ_TRUE) return 1;
	if (v->type == UIBJ_FALSE) return 0;
	if (v->type == UIBJ_NUM) return v->num != 0;
	return def;
}

void UIBJ_Write(uibbuf_t *b, const uibj_t *v)
{
	switch (v->type)
	{
	case UIBJ_NULL: UIBB_Puts(b, "null"); break;
	case UIBJ_TRUE: UIBB_Puts(b, "true"); break;
	case UIBJ_FALSE: UIBB_Puts(b, "false"); break;
	case UIBJ_NUM: UIBB_Num(b, v->num); break;
	case UIBJ_STR: UIBB_Str(b, v->str, v->len, 0, 0); break;
	default:
	{
		int obj = v->type == UIBJ_OBJ;
		UIBB_Putc(b, obj ? '{' : '[');
		for (uibj_t *m = v->child; m; m = m->next)
		{
			if (m != v->child)
				UIBB_Putc(b, ',');
			if (obj)
			{
				UIBB_Str(b, m->key, strlen(m->key), 0, 0);
				UIBB_Putc(b, ':');
			}
			UIBJ_Write(b, m);
		}
		UIBB_Putc(b, obj ? '}' : ']');
	}
	}
}

int UIBJ_PathText(const char *json, size_t len, const char *path, char *out, size_t outsize)
{
	if (outsize)
		out[0] = 0;
	uibjdoc_t *d = UIBJ_Parse(json, len, NULL, 0);
	if (!d)
		return 0;
	uibj_t *v = d->root;
	char seg[128];
	const char *s = path;
	while (v && s && *s)
	{
		const char *dot = strchr(s, '.');
		size_t n = dot ? (size_t)(dot - s) : strlen(s);
		if (n >= sizeof(seg)) n = sizeof(seg) - 1;
		memcpy(seg, s, n);
		seg[n] = 0;
		s = dot ? dot + 1 : NULL;
		if (v->type == UIBJ_ARR)
		{
			if (!strcmp(seg, "#"))
			{
				// count
				snprintf(out, outsize, "%d", (int)v->len);
				UIBJ_Free(d);
				return 1;
			}
			v = UIBJ_At(v, (size_t)atoi(seg));
		}
		else
			v = UIBJ_Get(v, seg);
	}
	int found = v != NULL;
	if (v)
	{
		switch (v->type)
		{
		case UIBJ_STR: snprintf(out, outsize, "%s", v->str); break;
		case UIBJ_NUM:
			if (v->num == (double)(long long)v->num) snprintf(out, outsize, "%lld", (long long)v->num);
			else snprintf(out, outsize, "%.9g", v->num);
			break;
		case UIBJ_TRUE: snprintf(out, outsize, "1"); break;
		case UIBJ_FALSE: snprintf(out, outsize, "0"); break;
		case UIBJ_NULL: break;
		default: snprintf(out, outsize, "%.*s", (int)v->rawlen, v->raw); break;
		}
	}
	UIBJ_Free(d);
	return found;
}
