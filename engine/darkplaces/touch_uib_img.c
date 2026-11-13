/*
Godnotic UI bridge: images by reference.

A renderer asks for `pk3:maps/stormkeep` at 480x270; the engine loads it through its own
filesystem (pk3, pk3dir, zstd entries; any of tga/png/jpg), scales it to fit the box,
writes a PNG into <gamedir>/uib-cache/ and answers with that path (or the bytes in
base64). The cache survives restarts; a request that hits it costs a stat.
Decoding is spread over frames: at most ~6 ms of image work per frame.
*/
#include "quakedef.h"
#include "image.h"
#include "touch_uib_int.h"

typedef struct
{
	int conn;
	int64_t re;
	char ref[256];
	int w, h, asbytes;
} imgreq_t;

#define MAX_REQ 256
static imgreq_t reqs[MAX_REQ];
static int nreq;

int UIBImg_Resolve(const char *ref, char *path, size_t pathsize)
{
	path[0] = 0;
	if (!strncmp(ref, "pk3:", 4))
		dpsnprintf(path, pathsize, "%s", ref + 4);
	else if (!strncmp(ref, "asset:", 6))
	{
		const char *a = ref + 6;
		if (!strcmp(a, "banner") || !strcmp(a, "logo")) dpsnprintf(path, pathsize, "gfx/xonotic_logo");
		else if (!strcmp(a, "background")) dpsnprintf(path, pathsize, "gfx/godnotic_conback");
		else if (!strcmp(a, "loading") || !strcmp(a, "placeholder:map")) dpsnprintf(path, pathsize, "gfx/godnotic_loading");
		else if (!strncmp(a, "mode:", 5)) dpsnprintf(path, pathsize, "gfx/menu/luma/gametype_%s", a + 5);
		else return 0;
	}
	else if (!strncmp(ref, "mod:", 4))
	{
		// mod:<id>/<path> -> the mod's package directory as the Lua runtime mounts it
		const char *slash = strchr(ref + 4, '/');
		if (!slash)
			return 0;
		dpsnprintf(path, pathsize, "lua/mods/%.*s/%s", (int)(slash - ref - 4), ref + 4, slash + 1);
	}
	else
		return 0;
	if (strstr(path, "..") || path[0] == '/' || strchr(path, '\\') || strchr(path, ':'))
	{
		path[0] = 0;
		return 0;
	}
	// drop a known extension: the loader tries every format itself
	size_t l = strlen(path);
	if (l > 4 && (!strcasecmp(path + l - 4, ".jpg") || !strcasecmp(path + l - 4, ".png") || !strcasecmp(path + l - 4, ".tga")))
		path[l - 4] = 0;
	return path[0] != 0;
}

static unsigned adler32(const unsigned char *p, size_t n, unsigned a)
{
	unsigned s1 = a & 0xffff, s2 = a >> 16;
	while (n)
	{
		size_t k = n < 5552 ? n : 5552;
		n -= k;
		while (k--)
		{
			s1 += *p++;
			s2 += s1;
		}
		s1 %= 65521;
		s2 %= 65521;
	}
	return (s2 << 16) | s1;
}

static unsigned crc_table[256];
static unsigned crc32b(const unsigned char *p, size_t n, unsigned c)
{
	if (!crc_table[1])
		for (unsigned i = 0; i < 256; i++)
		{
			unsigned v = i;
			for (int k = 0; k < 8; k++)
				v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
			crc_table[i] = v;
		}
	c = ~c;
	while (n--)
		c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return ~c;
}

static void be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(uibbuf_t *b, const char *type, const unsigned char *data, size_t len)
{
	unsigned char hdr[8];
	be32(hdr, (unsigned)len);
	memcpy(hdr + 4, type, 4);
	UIBB_Put(b, (const char *)hdr, 8);
	if (len)
		UIBB_Put(b, (const char *)data, len);
	unsigned c = crc32b((const unsigned char *)type, 4, 0);
	c = crc32b(data, len, c);
	unsigned char t[4];
	be32(t, c);
	UIBB_Put(b, (const char *)t, 4);
}

// RGBA (top row first) to a PNG with stored (uncompressed) deflate blocks: no zlib needed.
static void png_encode(uibbuf_t *out, const unsigned char *rgba, int w, int h)
{
	static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
	UIBB_Put(out, (const char *)sig, 8);
	unsigned char ihdr[13];
	be32(ihdr, w);
	be32(ihdr + 4, h);
	ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
	chunk(out, "IHDR", ihdr, 13);
	size_t rowlen = (size_t)w * 4 + 1, raw = rowlen * h;
	unsigned char *filt = (unsigned char *)malloc(raw);
	for (int y = 0; y < h; y++)
	{
		filt[y * rowlen] = 0;
		memcpy(filt + y * rowlen + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
	}
	uibbuf_t z = {0};
	UIBB_Put(&z, "\x78\x01", 2);
	for (size_t off = 0; off < raw;)
	{
		size_t n = raw - off > 65535 ? 65535 : raw - off;
		unsigned char h5[5] = {(unsigned char)(off + n >= raw), (unsigned char)(n & 0xff), (unsigned char)(n >> 8),
			(unsigned char)(~n & 0xff), (unsigned char)((~n >> 8) & 0xff)};
		UIBB_Put(&z, (const char *)h5, 5);
		UIBB_Put(&z, (const char *)filt + off, n);
		off += n;
	}
	unsigned char ad[4];
	be32(ad, adler32(filt, raw, 1));
	UIBB_Put(&z, (const char *)ad, 4);
	chunk(out, "IDAT", (const unsigned char *)z.p, z.n);
	chunk(out, "IEND", NULL, 0);
	UIBB_Free(&z);
	free(filt);
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(uibbuf_t *b, const unsigned char *p, size_t n)
{
	UIBB_Reserve(b, n / 3 * 4 + 8);
	for (size_t i = 0; i < n; i += 3)
	{
		unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		char q[4] = {b64[(v >> 18) & 63], b64[(v >> 12) & 63], i + 1 < n ? b64[(v >> 6) & 63] : '=', i + 2 < n ? b64[v & 63] : '='};
		UIBB_Put(b, q, 4);
	}
}

static void answer(const imgreq_t *r, int w, int h, const char *relpath, const unsigned char *png, size_t pnglen)
{
	uibbuf_t b = {0};
	UIBB_Printf(&b, "{\"uib\":1,\"t\":\"image\",\"id\":%d,\"re\":%lld,\"ref\":", UIB_NextMsgId(r->conn), (long long)r->re);
	UIBB_Str(&b, r->ref, strlen(r->ref), 250, 0);
	UIBB_Printf(&b, ",\"mime\":\"image/png\",\"w\":%d,\"h\":%d,", w, h);
	if (r->asbytes && png)
	{
		UIBB_Puts(&b, "\"data\":\"");
		base64(&b, png, pnglen);
		UIBB_Putc(&b, '"');
	}
	else
	{
		char abs[MAX_OSPATH * 2];
		dpsnprintf(abs, sizeof(abs), "%s%s", fs_gamedir, relpath);
		UIBB_Puts(&b, "\"path\":");
		UIBB_Str(&b, abs, strlen(abs), 0, 0);
	}
	UIBB_Putc(&b, '}');
	UIB_SendTo(r->conn, b.p, b.n);
	UIBB_Free(&b);
}

static unsigned hashref(const char *s)
{
	unsigned h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static void process(const imgreq_t *r)
{
	char path[256], rel[128];
	if (!UIBImg_Resolve(r->ref, path, sizeof(path)))
	{
		UIB_SendError(r->conn, r->re, "not_found", "unknown image ref");
		return;
	}
	int bw = r->w > 0 ? r->w : 512, bh = r->h > 0 ? r->h : 512;
	if (bw > 2048) bw = 2048;
	if (bh > 2048) bh = 2048;
	dpsnprintf(rel, sizeof(rel), "uib-cache/%08x_%dx%d.png", hashref(r->ref), bw, bh);
	// cached: we need its size, kept in a tiny sidecar name scheme: read the PNG header
	qfile_t *f = FS_OpenRealFile(rel, "rb", true);
	if (f)
	{
		unsigned char hdr[24];
		fs_offset_t n = FS_Read(f, hdr, 24);
		FS_Close(f);
		if (n == 24 && !r->asbytes)
		{
			int w = hdr[16] << 24 | hdr[17] << 16 | hdr[18] << 8 | hdr[19];
			int h = hdr[20] << 24 | hdr[21] << 16 | hdr[22] << 8 | hdr[23];
			answer(r, w, h, rel, NULL, 0);
			return;
		}
	}
	unsigned char *bgra = loadimagepixelsbgra(path, false, false, false, NULL);
	if (!bgra)
	{
		UIB_SendError(r->conn, r->re, "not_found", "no such image");
		return;
	}
	int iw = image_width, ih = image_height;
	double s = fmin((double)bw / iw, (double)bh / ih);
	if (s > 1)
		s = 1;
	int w = (int)(iw * s + 0.5), h = (int)(ih * s + 0.5);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	unsigned char *px = bgra;
	if (w != iw || h != ih)
	{
		px = (unsigned char *)Mem_Alloc(tempmempool, (size_t)w * h * 4);
		Image_Resample32(bgra, iw, ih, 1, px, w, h, 1, 1);
	}
	// BGRA -> RGBA
	unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
	for (size_t i = 0; i < (size_t)w * h; i++)
	{
		rgba[i * 4 + 0] = px[i * 4 + 2];
		rgba[i * 4 + 1] = px[i * 4 + 1];
		rgba[i * 4 + 2] = px[i * 4 + 0];
		rgba[i * 4 + 3] = px[i * 4 + 3];
	}
	if (px != bgra)
		Mem_Free(px);
	Mem_Free(bgra);
	uibbuf_t png = {0};
	png_encode(&png, rgba, w, h);
	free(rgba);
	FS_WriteFile(rel, png.p, png.n);
	answer(r, w, h, rel, (const unsigned char *)png.p, png.n);
	UIBB_Free(&png);
}

void UIBImg_Request(int conn, int64_t re, const char *ref, int w, int h, int asbytes)
{
	if (strlen(ref) >= sizeof(reqs[0].ref) || !*ref)
	{
		UIB_SendError(conn, re, "bad_request", "bad image ref");
		return;
	}
	if (nreq >= MAX_REQ)
	{
		UIB_SendError(conn, re, "busy", "too many image requests");
		return;
	}
	imgreq_t *r = &reqs[nreq++];
	r->conn = conn;
	r->re = re;
	dp_strlcpy(r->ref, ref, sizeof(r->ref));
	r->w = w;
	r->h = h;
	r->asbytes = asbytes;
}

void UIBImg_Frame(void)
{
	if (!nreq)
		return;
	double start = Sys_DirtyTime();
	int done = 0;
	while (done < nreq)
	{
		process(&reqs[done++]);
		if (Sys_DirtyTime() - start > 0.006)
			break;
	}
	memmove(reqs, reqs + done, sizeof(imgreq_t) * (nreq - done));
	nreq -= done;
}

// A thumbnail no larger than w x h as a VFS path ("uib-cache/..."), made now if needed:
// the QuakeC renderer draws these instead of decoding full-size previews while scrolling.
int UIBImg_Thumb(const char *ref, int w, int h, char *out, size_t outsize)
{
	char path[256], rel[128];
	out[0] = 0;
	if (!UIBImg_Resolve(ref, path, sizeof(path)))
		return 0;
	w = w < 16 ? 16 : w > 1024 ? 1024 : w;
	h = h < 16 ? 16 : h > 1024 ? 1024 : h;
	dpsnprintf(rel, sizeof(rel), "uib-cache/%08x_%dx%d.png", hashref(ref), w, h);
	if (!FS_FileExists(rel))
	{
		unsigned char *bgra = loadimagepixelsbgra(path, false, false, false, NULL);
		if (!bgra)
			return 0;
		int iw = image_width, ih = image_height;
		double sc = fmin(fmin((double)w / iw, (double)h / ih), 1.0);
		int tw = (int)(iw * sc + 0.5), th = (int)(ih * sc + 0.5);
		if (tw < 1) tw = 1;
		if (th < 1) th = 1;
		unsigned char *px = (unsigned char *)Mem_Alloc(tempmempool, (size_t)tw * th * 4);
		Image_Resample32(bgra, iw, ih, 1, px, tw, th, 1, 1);
		Mem_Free(bgra);
		unsigned char *rgba = (unsigned char *)malloc((size_t)tw * th * 4);
		for (size_t i = 0; i < (size_t)tw * th; i++)
		{
			rgba[i * 4 + 0] = px[i * 4 + 2];
			rgba[i * 4 + 1] = px[i * 4 + 1];
			rgba[i * 4 + 2] = px[i * 4 + 0];
			rgba[i * 4 + 3] = px[i * 4 + 3];
		}
		Mem_Free(px);
		uibbuf_t png = {0};
		png_encode(&png, rgba, tw, th);
		free(rgba);
		FS_WriteFile(rel, png.p, png.n);
		UIBB_Free(&png);
	}
	dp_strlcpy(out, rel, outsize);
	out[strlen(out) - 4] = 0;   // without .png: the loader finds it
	return 1;
}

void UIBImg_Init(void) {}
