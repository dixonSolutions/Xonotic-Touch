/*
Godnotic: installing, updating and uninstalling Lua mods and plugins
(docs/PLUGIN_STANDARD.md, "Packages" and "Lifecycle").

  lua_mod_install <path | https-url>[#sha256=<hex>] [sha256]
  lua_install_confirm / lua_install_cancel / lua_install_status
  lua_mod_update <name> [path | https-url]
  lua_mod_uninstall <name> [keepdata]
  (sv_lua_plugin_install, _update, _uninstall: the same for server plugins)

A package is a zip archive (or, for development, a folder) holding a mod:
init.lua and mod.txt at its top or in its single top folder. Installing is two
steps, so nothing reaches the mods folder unseen: the package is read, checked
whole and kept in memory, and its manifest and requested permissions (with the
author's reasons) are shown; only lua_install_confirm writes it. Nothing is
enabled or granted by an install; an update keeps the on/off choice and the
grants the new version still requests, and says which new permissions it asks
for (they stay ungranted).

Everything in a package is untrusted:
- size: at most 8 MB downloaded or read, 256 files, 1 MB per file, 8 MB in all;
  the inflater (written here, bounded) never produces more than an entry's
  declared size, so a zip bomb stops at the first byte past it, and the CRC of
  every file is checked;
- names: '/'-separated components of A-Z a-z 0-9 . _ -, none empty, "." or
  "..", none starting with '.', no backslashes, drive letters, absolute paths,
  case-insensitive duplicates, at most 8 deep and 200 long; a few file types
  only (Lua, text, images, sounds); no data/ (the mod's storage is the user's);
  symlinks, encrypted entries, zip64 and unknown compression are refused;
- writing: the files go to <realm>/.install.tmp/ through the same confined
  open as a mod's own files (one component at a time, O_NOFOLLOW, regular files
  only), then the folder is renamed into place, so a package can only ever
  produce <realm>/<its id>/ and never touches another mod, enabled.txt or the
  manager's state in <userdir>/luastate/.

Downloads go through the engine's libcurl (Curl_Begin_ToMemory, driven by
Curl_Frame each frame: the game keeps running), HTTPS only, redirects included
(a hook in libcurl.c restricts the protocols for these transfers only).
lua_install_insecure_localhost 1 also allows plain http:// to 127.0.0.1 for
the test suite. A sha256 in the URL fragment (#sha256=...) or as the second
argument is checked before the review.

A server can never trigger any of this: every command here refuses while text
from a server this engine does not host may be in the command buffer
(TLua_RemoteRefused), like the other manager commands.
*/

#include "quakedef.h"
#include "libcurl.h"
#include "touch_lua.h"
#include "touch_lua_int.h"

#include <errno.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#endif

#define TLUA_PKG_MAXSIZE (8 * 1024 * 1024)  // a package (download or file), and all its files together
#define TLUA_PKG_MAXFILES 256
#define TLUA_PKG_MAXFILE (1024 * 1024)
#define TLUA_PKG_MAXPATH 200
#define TLUA_PKG_MAXDEPTH 8

static cvar_t lua_install_insecure_localhost = {CF_CLIENT | CF_SERVER, "lua_install_insecure_localhost", "0", "also allow plain http:// installs from 127.0.0.1 (for tests; never set this for real use)"};
static cvar_t lua_install_state = {CF_CLIENT | CF_SERVER | CF_READONLY, "_lua_install", "", "the pending Lua install for the Mods dialog: key<TAB>value lines"};

void TLua_Guard(struct cvar_s *var);
void TLua_RegisterCvar(struct cvar_s *var);

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)

typedef struct tlua_sha256_s { unsigned int h[8]; unsigned char buf[64]; unsigned long long len; size_t n; } tlua_sha256_t;

static const unsigned int tlua_sha_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define TLUA_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void tlua_sha_block(tlua_sha256_t *s, const unsigned char *p)
{
	unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = ((unsigned)p[i * 4] << 24) | ((unsigned)p[i * 4 + 1] << 16) | ((unsigned)p[i * 4 + 2] << 8) | p[i * 4 + 3];
	for (i = 16; i < 64; i++)
	{
		unsigned int s0 = TLUA_ROR(w[i - 15], 7) ^ TLUA_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
		unsigned int s1 = TLUA_ROR(w[i - 2], 17) ^ TLUA_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
	for (i = 0; i < 64; i++)
	{
		t1 = h + (TLUA_ROR(e, 6) ^ TLUA_ROR(e, 11) ^ TLUA_ROR(e, 25)) + ((e & f) ^ (~e & g)) + tlua_sha_k[i] + w[i];
		t2 = (TLUA_ROR(a, 2) ^ TLUA_ROR(a, 13) ^ TLUA_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
	}
	s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void tlua_sha256_hex(const unsigned char *data, size_t len, char out[65])
{
	static const unsigned int init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	tlua_sha256_t s;
	unsigned char tail[128];
	size_t full = len & ~(size_t)63, rest = len - full, padlen, i;
	unsigned long long bits = (unsigned long long)len * 8;
	memcpy(s.h, init, sizeof(init));
	for (i = 0; i < full; i += 64)
		tlua_sha_block(&s, data + i);
	memset(tail, 0, sizeof(tail));
	memcpy(tail, data + full, rest);
	tail[rest] = 0x80;
	padlen = rest + 1 + 8 <= 64 ? 64 : 128;
	for (i = 0; i < 8; i++)
		tail[padlen - 1 - i] = (unsigned char)(bits >> (8 * i));
	tlua_sha_block(&s, tail);
	if (padlen == 128)
		tlua_sha_block(&s, tail + 64);
	for (i = 0; i < 8; i++)
		dpsnprintf(out + i * 8, 9, "%08x", s.h[i]);
}

// ---------------------------------------------------------------------------
// CRC-32 and a bounded inflater (RFC 1951). Written here rather than through
// zlib: FS_Inflate grows its output without a limit (a zip bomb would take
// all memory), and the web and Android builds then need no zlib for this.

static unsigned int tlua_crc32(const unsigned char *p, size_t n)
{
	static unsigned int table[256];
	unsigned int c = 0xffffffffu;
	size_t i;
	if (!table[1])
	{
		unsigned int k, j;
		for (k = 0; k < 256; k++)
		{
			unsigned int v = k;
			for (j = 0; j < 8; j++)
				v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
			table[k] = v;
		}
	}
	for (i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xff] ^ (c >> 8);
	return c ^ 0xffffffffu;
}

typedef struct tlua_inflate_s
{
	const unsigned char *in;
	size_t inlen, inpos;
	unsigned int bitbuf, bitcnt;
	unsigned char *out;
	size_t outlen, outpos;   // outlen: the declared size, never exceeded
	int err;
} tlua_inflate_t;

typedef struct tlua_huff_s { short count[16]; short symbol[288]; } tlua_huff_t;

static int tlua_bits(tlua_inflate_t *s, int need)
{
	unsigned int val = s->bitbuf;
	while (s->bitcnt < (unsigned)need)
	{
		if (s->inpos >= s->inlen)
		{
			s->err = 1; // ran out of input
			return 0;
		}
		val |= (unsigned int)s->in[s->inpos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = val >> need;
	s->bitcnt -= need;
	return (int)(val & ((1u << need) - 1));
}

static int tlua_decode(tlua_inflate_t *s, const tlua_huff_t *h)
{
	int code = 0, first = 0, index = 0, len;
	for (len = 1; len < 16; len++)
	{
		int count;
		code |= tlua_bits(s, 1);
		if (s->err)
			return -1;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	s->err = 2;
	return -1;
}

// 0 complete, >0 incomplete (allowed only for the distance code of length 1), <0 over-subscribed
static int tlua_construct(tlua_huff_t *h, const short *length, int n)
{
	short offs[16];
	int sym, len, left;
	for (len = 0; len < 16; len++)
		h->count[len] = 0;
	for (sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)
		return 0;
	left = 1;
	for (len = 1; len < 16; len++)
	{
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (len = 1; len < 15; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (sym = 0; sym < n; sym++)
		if (length[sym] != 0)
			h->symbol[offs[length[sym]]++] = (short)sym;
	return left;
}

static int tlua_codes(tlua_inflate_t *s, const tlua_huff_t *lencode, const tlua_huff_t *distcode)
{
	static const short lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
	static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
	static const short dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
	static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
	int symbol;
	for (;;)
	{
		symbol = tlua_decode(s, lencode);
		if (symbol < 0)
			return -1;
		if (symbol < 256)
		{
			if (s->outpos >= s->outlen)
				return -3; // more than declared: a bomb, or a lie
			s->out[s->outpos++] = (unsigned char)symbol;
		}
		else if (symbol > 256)
		{
			size_t len, dist;
			symbol -= 257;
			if (symbol >= 29)
				return -4;
			len = (size_t)lbase[symbol] + (size_t)tlua_bits(s, lext[symbol]);
			symbol = tlua_decode(s, distcode);
			if (symbol < 0 || symbol >= 30)
				return -4;
			dist = (size_t)dbase[symbol] + (size_t)tlua_bits(s, dext[symbol]);
			if (s->err)
				return -1;
			if (dist > s->outpos)
				return -4;
			if (len > s->outlen - s->outpos)
				return -3;
			while (len--)
			{
				s->out[s->outpos] = s->out[s->outpos - dist];
				s->outpos++;
			}
		}
		else
			return 0;
	}
}

static int tlua_inflate_run(tlua_inflate_t *s)
{
	static tlua_huff_t fixlen, fixdist;
	static qbool fixed_done;
	int last;
	do
	{
		int type;
		last = tlua_bits(s, 1);
		type = tlua_bits(s, 2);
		if (s->err)
			return -1;
		if (type == 0)
		{
			unsigned int len;
			s->bitbuf = 0;
			s->bitcnt = 0;
			if (s->inpos + 4 > s->inlen)
				return -1;
			len = s->in[s->inpos] | (s->in[s->inpos + 1] << 8);
			if ((unsigned)(s->in[s->inpos + 2] | (s->in[s->inpos + 3] << 8)) != (~len & 0xffff))
				return -2;
			s->inpos += 4;
			if (s->inpos + len > s->inlen)
				return -1;
			if (len > s->outlen - s->outpos)
				return -3;
			memcpy(s->out + s->outpos, s->in + s->inpos, len);
			s->outpos += len;
			s->inpos += len;
		}
		else if (type == 1)
		{
			if (!fixed_done)
			{
				short lengths[288];
				int sym;
				for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
				for (; sym < 256; sym++) lengths[sym] = 9;
				for (; sym < 280; sym++) lengths[sym] = 7;
				for (; sym < 288; sym++) lengths[sym] = 8;
				tlua_construct(&fixlen, lengths, 288);
				for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
				tlua_construct(&fixdist, lengths, 30);
				fixed_done = true;
			}
			if (tlua_codes(s, &fixlen, &fixdist))
				return -4;
		}
		else if (type == 2)
		{
			static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
			short lengths[320];
			tlua_huff_t lencode, distcode;
			int nlen = tlua_bits(s, 5) + 257, ndist = tlua_bits(s, 5) + 1, ncode = tlua_bits(s, 4) + 4, index, err;
			if (s->err || nlen > 286 || ndist > 30)
				return -5;
			for (index = 0; index < ncode; index++)
				lengths[order[index]] = (short)tlua_bits(s, 3);
			for (; index < 19; index++)
				lengths[order[index]] = 0;
			if (s->err || tlua_construct(&lencode, lengths, 19) != 0)
				return -5;
			index = 0;
			while (index < nlen + ndist)
			{
				int symbol = tlua_decode(s, &lencode), len = 0, rep;
				if (symbol < 0)
					return -5;
				if (symbol < 16)
					lengths[index++] = (short)symbol;
				else
				{
					if (symbol == 16)
					{
						if (index == 0)
							return -5;
						len = lengths[index - 1];
						rep = 3 + tlua_bits(s, 2);
					}
					else if (symbol == 17)
						rep = 3 + tlua_bits(s, 3);
					else
						rep = 11 + tlua_bits(s, 7);
					if (s->err || index + rep > nlen + ndist)
						return -5;
					while (rep--)
						lengths[index++] = (short)len;
				}
			}
			if (lengths[256] == 0)
				return -5;
			err = tlua_construct(&lencode, lengths, nlen);
			if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
				return -5;
			err = tlua_construct(&distcode, lengths + nlen, ndist);
			if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
				return -5;
			if (tlua_codes(s, &lencode, &distcode))
				return -4;
		}
		else
			return -6;
	} while (!last);
	return 0;
}

// raw deflate data into exactly outlen bytes; false on any error, short
// output or an attempt to produce more
static qbool tlua_inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen)
{
	tlua_inflate_t s;
	memset(&s, 0, sizeof(s));
	s.in = in;
	s.inlen = inlen;
	s.out = out;
	s.outlen = outlen;
	return tlua_inflate_run(&s) == 0 && !s.err && s.outpos == outlen;
}

// ---------------------------------------------------------------------------
// A package read into memory and checked

typedef struct tlua_pkgfile_s
{
	char path[TLUA_PKG_MAXPATH + 1];  // relative to the mod folder
	size_t off, len;                  // in tlua_pkg.data
} tlua_pkgfile_t;

typedef struct tlua_pkg_s
{
	int state;                        // TLUA_PKG_*
	tlua_realm_t *realm;
	char source[512];                 // path or URL, as given (without the fragment)
	char pin[65];                     // the sha256 the user pinned, or ""
	char sha[65];                     // the package's
	char message[512];
	tlua_mod_t info;                  // the manifest as parsed (name = the install name)
	qbool update;                     // the folder exists already
	char oldversion[32];
	unsigned oldrequested, oldgranted;
	qbool oldscoped;
	unsigned char *data;              // the extracted files
	size_t datalen;
	tlua_pkgfile_t files[TLUA_PKG_MAXFILES];
	int numfiles;
	double started;
} tlua_pkg_t;

#define TLUA_PKG_IDLE 0
#define TLUA_PKG_DOWNLOADING 1
#define TLUA_PKG_REVIEW 2
#define TLUA_PKG_ERROR 3
#define TLUA_PKG_DONE 4

static tlua_pkg_t tlua_pkg;
static unsigned char *tlua_dlbuf;
static int tlua_dltoken;

static const char *tlua_pkg_statename(int st)
{
	switch (st)
	{
	case TLUA_PKG_DOWNLOADING: return "downloading";
	case TLUA_PKG_REVIEW: return "review";
	case TLUA_PKG_ERROR: return "error";
	case TLUA_PKG_DONE: return "done";
	default: return "idle";
	}
}

static void tlua_pkg_publish(void)
{
	static char buf[8192];
	char line[1024], clean[600];
	tlua_pkg_t *p = &tlua_pkg;
	int i;
	if (cls.state == ca_dedicated)
		return;
	buf[0] = 0;
#define ADD(k, v) do { dpsnprintf(line, sizeof(line), "%s\t%s\n", k, TLua_CleanText(v, clean, sizeof(clean), false)); dp_strlcat(buf, line, sizeof(buf)); } while (0)
	ADD("state", tlua_pkg_statename(p->state));
	ADD("realm", p->realm ? p->realm->kind : "mod");
	ADD("source", p->source);
	ADD("message", p->message);
	if (p->state == TLUA_PKG_REVIEW || p->state == TLUA_PKG_DONE)
	{
		tlua_mod_t *m = &p->info;
		ADD("name", m->name);
		ADD("title", m->title[0] ? m->title : m->name);
		ADD("version", m->version);
		ADD("author", m->author);
		ADD("homepage", m->homepage);
		ADD("description", m->description);
		ADD("api", m->api ? va(line, sizeof(line), "%d", m->api) : "legacy");
		ADD("update", p->update ? "1" : "0");
		ADD("oldversion", p->oldversion);
		ADD("files", va(line, sizeof(line), "%d files, %u KB", p->numfiles, (unsigned)((p->datalen + 1023) >> 10)));
		ADD("sha256", p->sha);
		ADD("pinned", p->pin[0] ? "1" : "0");
		for (i = 0; tlua_permissions[i].name; i++)
		{
			unsigned bit = tlua_permissions[i].bit;
			const char *tag;
			if (!(m->requested & bit))
				continue;
			// new: an update asks for something the old version did not
			tag = p->update && !(p->oldrequested & bit) ? "new" : (p->update && (p->oldgranted & p->oldrequested & bit) && p->oldscoped == m->scoped) ? "kept" : "ask";
			dpsnprintf(line, sizeof(line), "perm\t%s\t%s\t%s\n", tlua_permissions[i].name, tag,
				TLua_CleanText(m->reasons[i][0] ? m->reasons[i] : tlua_permissions[i].help, clean, sizeof(clean), false));
			dp_strlcat(buf, line, sizeof(buf));
		}
	}
#undef ADD
	Cvar_SetQuick(&lua_install_state, buf);
}

static void tlua_pkg_free(void)
{
	if (tlua_pkg.data)
		Mem_Free(tlua_pkg.data);
	tlua_pkg.data = NULL;
	tlua_pkg.datalen = 0;
	tlua_pkg.numfiles = 0;
}

static void tlua_pkg_fail(const char *fmt, ...) DP_FUNC_PRINTF(1);
static void tlua_pkg_fail(const char *fmt, ...)
{
	va_list ap;
	char text[512];
	va_start(ap, fmt);
	dpvsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	tlua_pkg_free();
	tlua_pkg.state = TLUA_PKG_ERROR;
	dp_strlcpy(tlua_pkg.message, text, sizeof(tlua_pkg.message));
	TLua_Say("^1Install refused:^7 %s", text);
	tlua_pkg_publish();
}

static unsigned tlua_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned tlua_u32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

// An entry name in a package; NULL when fine, else why not
static const char *tlua_pkg_badpath(const char *name)
{
	const char *p, *comp;
	int depth = 0;
	size_t n = strlen(name);
	if (!n)
		return "an empty name";
	if (n > TLUA_PKG_MAXPATH)
		return "a name over 200 characters";
	if (name[0] == '/')
		return "an absolute path";
	for (p = name; *p; p++)
	{
		unsigned char c = (unsigned char)*p;
		if (c == '\\')
			return "a backslash";
		if (c == ':')
			return "a drive letter or colon";
		if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/'))
			return "a character other than A-Z a-z 0-9 . _ - /";
	}
	for (comp = name; *comp; )
	{
		const char *slash = strchr(comp, '/');
		size_t len = slash ? (size_t)(slash - comp) : strlen(comp);
		if (len == 0)
			return "an empty path component";
		if (comp[0] == '.')
			return len <= 2 && (len == 1 || comp[1] == '.') ? "a . or .. component" : "a hidden (dot) file";
		if (++depth > TLUA_PKG_MAXDEPTH)
			return "more than 8 levels of folders";
		if (!slash)
			break;
		comp = slash + 1;
	}
	return NULL;
}

static qbool tlua_pkg_typeok(const char *path)
{
	static const char *const ext[] = {".lua", ".txt", ".md", ".json", ".png", ".tga", ".jpg", ".jpeg", ".ogg", ".wav", NULL};
	static const char *const bare[] = {"LICENSE", "COPYING", "README", "AUTHORS", NULL};
	const char *base = strrchr(path, '/'), *dot;
	int i;
	base = base ? base + 1 : path;
	dot = strrchr(base, '.');
	if (!dot)
	{
		for (i = 0; bare[i]; i++)
			if (!strcmp(base, bare[i]))
				return true;
		return false;
	}
	for (i = 0; ext[i]; i++)
		if (!strcasecmp(dot, ext[i]))
			return true;
	return false;
}

static qbool tlua_pkg_add(const char *name, const unsigned char *bytes, size_t len, size_t *total)
{
	tlua_pkgfile_t *f;
	int i;
	if (tlua_pkg.numfiles >= TLUA_PKG_MAXFILES)
	{
		tlua_pkg_fail("more than %d files", TLUA_PKG_MAXFILES);
		return false;
	}
	if (len > TLUA_PKG_MAXFILE)
	{
		tlua_pkg_fail("%s is over 1 MB", name);
		return false;
	}
	if (*total + len > TLUA_PKG_MAXSIZE)
	{
		tlua_pkg_fail("over 8 MB unpacked");
		return false;
	}
	for (i = 0; i < tlua_pkg.numfiles; i++)
		if (!strcasecmp(tlua_pkg.files[i].path, name))
		{
			tlua_pkg_fail("%s is in it twice", name);
			return false;
		}
	f = &tlua_pkg.files[tlua_pkg.numfiles++];
	dp_strlcpy(f->path, name, sizeof(f->path));
	f->off = *total;
	f->len = len;
	if (bytes)
		memcpy(tlua_pkg.data + *total, bytes, len);
	*total += len;
	return true;
}

// a zip in memory: every entry checked and extracted into tlua_pkg.data
static qbool tlua_pkg_readzip(const unsigned char *z, size_t size)
{
	size_t eocd = 0, cdoff, cdsize, p, total = 0;
	unsigned count, i;
	qbool found = false;
	if (size < 22)
	{
		tlua_pkg_fail("not a zip archive (too short)");
		return false;
	}
	for (p = size - 22 + 1; p-- > 0 && size - p <= 22 + 65535; )
		if (tlua_u32(z + p) == 0x06054b50)
		{
			eocd = p;
			found = true;
			break;
		}
	if (!found)
	{
		tlua_pkg_fail("not a zip archive");
		return false;
	}
	count = tlua_u16(z + eocd + 10);
	cdsize = tlua_u32(z + eocd + 12);
	cdoff = tlua_u32(z + eocd + 16);
	if (tlua_u16(z + eocd + 4) || tlua_u16(z + eocd + 6) || count != tlua_u16(z + eocd + 8) || count == 0xffff || cdoff == 0xffffffffu)
	{
		tlua_pkg_fail("a split, empty or zip64 archive");
		return false;
	}
	if (cdoff > eocd || cdsize > eocd - cdoff)
	{
		tlua_pkg_fail("a damaged zip archive (central directory)");
		return false;
	}
	tlua_pkg.data = (unsigned char *)Mem_Alloc(tempmempool, TLUA_PKG_MAXSIZE);
	for (i = 0, p = cdoff; i < count; i++)
	{
		unsigned flags, method, nlen, xlen, clen, hostos, crc;
		size_t csize, usize, loff, dstart;
		unsigned int attr;
		char name[TLUA_PKG_MAXPATH + 2];
		const char *why;
		if (p + 46 > cdoff + cdsize || tlua_u32(z + p) != 0x02014b50)
		{
			tlua_pkg_fail("a damaged zip archive (entry %u)", i);
			return false;
		}
		hostos = z[p + 5];
		flags = tlua_u16(z + p + 8);
		method = tlua_u16(z + p + 10);
		crc = tlua_u32(z + p + 16);
		csize = tlua_u32(z + p + 20);
		usize = tlua_u32(z + p + 24);
		nlen = tlua_u16(z + p + 28);
		xlen = tlua_u16(z + p + 30);
		clen = tlua_u16(z + p + 32);
		attr = tlua_u32(z + p + 38);
		loff = tlua_u32(z + p + 42);
		if (p + 46 + nlen + xlen + clen > cdoff + cdsize)
		{
			tlua_pkg_fail("a damaged zip archive (entry %u name)", i);
			return false;
		}
		if (nlen > TLUA_PKG_MAXPATH + 1)
		{
			tlua_pkg_fail("an entry name over 200 characters");
			return false;
		}
		memcpy(name, z + p + 46, nlen);
		name[nlen] = 0;
		if (memchr(name, 0, nlen))
		{
			tlua_pkg_fail("an entry name with a NUL byte");
			return false;
		}
		p += 46 + nlen + xlen + clen;
		// macOS adds these; they are not part of the mod
		if (!strncmp(name, "__MACOSX/", 9) || !strcmp(name, ".DS_Store") || (strlen(name) > 10 && !strcmp(name + strlen(name) - 10, "/.DS_Store")))
			continue;
		if (nlen && name[nlen - 1] == '/')
		{
			// a folder entry: its name is checked, nothing is created for it
			name[nlen - 1] = 0;
			if ((why = tlua_pkg_badpath(name)))
			{
				tlua_pkg_fail("%s: %s", name, why);
				return false;
			}
			continue;
		}
		if ((why = tlua_pkg_badpath(name)))
		{
			tlua_pkg_fail("entry '%s': %s", name, why);
			return false;
		}
		if (hostos == 3 && ((attr >> 16) & 0170000) == 0120000)
		{
			tlua_pkg_fail("%s is a symlink", name);
			return false;
		}
		if (hostos == 3 && ((attr >> 16) & 0170000) && ((attr >> 16) & 0170000) != 0100000)
		{
			tlua_pkg_fail("%s is not a regular file", name);
			return false;
		}
		if (flags & 1)
		{
			tlua_pkg_fail("%s is encrypted", name);
			return false;
		}
		if (method != 0 && method != 8)
		{
			tlua_pkg_fail("%s uses an unsupported compression (%u)", name, method);
			return false;
		}
		if (usize == 0xffffffffu || csize == 0xffffffffu || loff == 0xffffffffu)
		{
			tlua_pkg_fail("zip64 entries are not supported");
			return false;
		}
		if (usize > TLUA_PKG_MAXFILE)
		{
			tlua_pkg_fail("%s is over 1 MB", name);
			return false;
		}
		if (method == 0 && csize != usize)
		{
			tlua_pkg_fail("%s: stored sizes disagree", name);
			return false;
		}
		// the local header must name the same file, and its data lie before the directory
		if (loff + 30 > cdoff || tlua_u32(z + loff) != 0x04034b50 || tlua_u16(z + loff + 26) != nlen || memcmp(z + loff + 30, z + p - nlen - xlen - clen, nlen))
		{
			tlua_pkg_fail("%s: the local header does not match the directory", name);
			return false;
		}
		dstart = loff + 30 + nlen + tlua_u16(z + loff + 28);
		if (dstart > cdoff || csize > cdoff - dstart)
		{
			tlua_pkg_fail("%s: data outside the archive", name);
			return false;
		}
		if (!tlua_pkg_add(name, NULL, usize, &total))
			return false;
		{
			unsigned char *dst = tlua_pkg.data + tlua_pkg.files[tlua_pkg.numfiles - 1].off;
			if (method == 0)
				memcpy(dst, z + dstart, usize);
			else if (!tlua_inflate(z + dstart, csize, dst, usize))
			{
				tlua_pkg_fail("%s: damaged, or it unpacks to more than it says (%u bytes)", name, (unsigned)usize);
				return false;
			}
			if (tlua_crc32(dst, usize) != crc)
			{
				tlua_pkg_fail("%s: CRC mismatch", name);
				return false;
			}
		}
	}
	tlua_pkg.datalen = total;
	return true;
}

#ifndef _WIN32
// a folder on disk (development): regular files only, no symlinks
static qbool tlua_pkg_readdir_r(const char *base, const char *rel, int depth, size_t *total)
{
	char path[1200];
	DIR *d;
	struct dirent *ent;
	dpsnprintf(path, sizeof(path), "%s%s%s", base, rel[0] ? "/" : "", rel);
	if (depth > TLUA_PKG_MAXDEPTH)
	{
		tlua_pkg_fail("more than 8 levels of folders");
		return false;
	}
	if (!(d = opendir(path)))
	{
		tlua_pkg_fail("cannot open the folder %s", path);
		return false;
	}
	while ((ent = readdir(d)))
	{
		char sub[TLUA_PKG_MAXPATH + 64], full[1400];
		struct stat st;
		const char *why;
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		dpsnprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", ent->d_name);
		dpsnprintf(full, sizeof(full), "%s/%s", base, sub);
		if (!strcmp(ent->d_name, ".DS_Store") || !strcmp(ent->d_name, ".git"))
			continue;
		if ((why = tlua_pkg_badpath(sub)))
		{
			closedir(d);
			tlua_pkg_fail("'%s': %s", sub, why);
			return false;
		}
		if (lstat(full, &st))
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if (!tlua_pkg_readdir_r(base, sub, depth + 1, total))
			{
				closedir(d);
				return false;
			}
		}
		else if (S_ISREG(st.st_mode))
		{
			int fd;
			size_t got = 0;
			if (st.st_size > TLUA_PKG_MAXFILE)
			{
				closedir(d);
				tlua_pkg_fail("%s is over 1 MB", sub);
				return false;
			}
			if (!tlua_pkg_add(sub, NULL, (size_t)st.st_size, total))
			{
				closedir(d);
				return false;
			}
			fd = open(full, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
			while (fd >= 0 && got < (size_t)st.st_size)
			{
				ssize_t r = read(fd, tlua_pkg.data + tlua_pkg.files[tlua_pkg.numfiles - 1].off + got, (size_t)st.st_size - got);
				if (r < 0 && errno == EINTR)
					continue;
				if (r <= 0)
					break;
				got += (size_t)r;
			}
			if (fd >= 0)
				close(fd);
			if (got != (size_t)st.st_size)
			{
				closedir(d);
				tlua_pkg_fail("could not read %s", sub);
				return false;
			}
		}
		else
		{
			closedir(d);
			tlua_pkg_fail("%s is a symlink or not a regular file", sub);
			return false;
		}
	}
	closedir(d);
	return true;
}
#endif

static tlua_pkgfile_t *tlua_pkg_file(const char *path)
{
	int i;
	for (i = 0; i < tlua_pkg.numfiles; i++)
		if (!strcmp(tlua_pkg.files[i].path, path))
			return &tlua_pkg.files[i];
	return NULL;
}

// After reading: the mod's root (strip one shared top folder), file types,
// no data/, an init.lua, the manifest, the install name, and the review.
static void tlua_pkg_review(const char *stem)
{
	tlua_pkg_t *p = &tlua_pkg;
	char top[TLUA_PKG_MAXPATH + 1] = "", manifest[64 * 1024];
	int i;
	tlua_pkgfile_t *f;
	tlua_mod_t *old;
	tlua_mod_t *m = &p->info;
	// one top folder holding everything: that folder is the mod
	if (p->numfiles)
	{
		const char *slash = strchr(p->files[0].path, '/');
		if (slash)
		{
			size_t n = (size_t)(slash - p->files[0].path) + 1;
			memcpy(top, p->files[0].path, n);
			top[n] = 0;
			for (i = 0; i < p->numfiles; i++)
				if (strncmp(p->files[i].path, top, n))
					top[0] = 0;
		}
		if (top[0])
		{
			size_t n = strlen(top);
			for (i = 0; i < p->numfiles; i++)
				memmove(p->files[i].path, p->files[i].path + n, strlen(p->files[i].path + n) + 1);
			top[n - 1] = 0;
		}
	}
	for (i = 0; i < p->numfiles; i++)
	{
		if (!strncmp(p->files[i].path, "data/", 5))
		{
			tlua_pkg_fail("it has a data/ folder: that is where the mod's own storage goes, not part of a package");
			return;
		}
		if (!tlua_pkg_typeok(p->files[i].path))
		{
			tlua_pkg_fail("%s: only .lua, .txt, .md, .json, images (.png .tga .jpg) and sounds (.ogg .wav) belong in a mod", p->files[i].path);
			return;
		}
	}
	if (!(f = tlua_pkg_file("init.lua")))
	{
		tlua_pkg_fail("no init.lua at the top of the package (or of its single top folder)");
		return;
	}
	memset(m, 0, sizeof(*m));
	m->realm = p->realm;
	manifest[0] = 0;
	if ((f = tlua_pkg_file("mod.txt")))
	{
		size_t n = min(f->len, sizeof(manifest) - 1);
		memcpy(manifest, p->data + f->off, n);
		manifest[n] = 0;
		if (memchr(manifest, 0, n))
		{
			tlua_pkg_fail("mod.txt is not text");
			return;
		}
	}
	// the name: the manifest's id, else the top folder's, else the file's
	{
		char idline[64] = "";
		const char *idp = strstr(manifest, "\nid:");
		if (!strncmp(manifest, "id:", 3))
			idp = manifest;
		else if (idp)
			idp++;
		if (idp)
		{
			const char *v = idp + 3;
			size_t k = 0;
			while (*v == ' ' || *v == '\t')
				v++;
			while (*v && *v != '\n' && *v != '\r' && *v != ' ' && k + 1 < sizeof(idline))
				idline[k++] = *v++;
			idline[k] = 0;
		}
		if (idline[0])
			dp_strlcpy(m->name, idline, sizeof(m->name));
		else if (top[0] && TLua_ValidModName(top))
			dp_strlcpy(m->name, top, sizeof(m->name));
		else if (stem && TLua_ValidModName(stem))
			dp_strlcpy(m->name, stem, sizeof(m->name));
	}
	if (!m->name[0] || !TLua_ValidModName(m->name))
	{
		tlua_pkg_fail("no usable name: give mod.txt an id: line (a-z, 0-9, _, up to %d)", TLUA_NAME_LEN);
		return;
	}
	if (TLua_ReservedName(p->realm, m->name))
	{
		tlua_pkg_fail("the name '%s' is reserved (it clashes with the engine's commands or settings)", m->name);
		return;
	}
	if (manifest[0])
		TLua_ParseManifest(m, manifest);
	else
		m->requested = TLUA_PERM_API1;
	if (m->manifest_error)
	{
		tlua_pkg_fail("mod.txt: %s", m->manifest_why);
		return;
	}
	// an update: the same name exists already
	TLua_Rescan(p->realm);
	old = TLua_Find(p->realm, m->name);
	p->update = old != NULL;
	p->oldversion[0] = 0;
	p->oldrequested = p->oldgranted = 0;
	if (old)
	{
		dp_strlcpy(p->oldversion, old->version, sizeof(p->oldversion));
		p->oldrequested = old->requested;
		p->oldgranted = old->granted;
		p->oldscoped = old->scoped;
	}
	p->state = TLUA_PKG_REVIEW;
	dpsnprintf(p->message, sizeof(p->message), p->update ? "Update %s: review the permissions, then update (it stays on or off; new permissions stay ungranted)" : "Install %s: review the permissions, then install (it stays off; nothing is granted)", m->name);
}

static void tlua_pkg_print(void)
{
	tlua_pkg_t *p = &tlua_pkg;
	tlua_mod_t *m = &p->info;
	int i;
	unsigned added = 0;
	char num[16];
	Con_Printf("\n^3%s Lua %s ^7%s^3 from %s^7\n", p->update ? "Update of" : "New", TLUA_NOUN(p->realm), m->name, p->source);
	Con_Printf("  title: %s\n", m->title[0] ? m->title : m->name);
	if (m->version[0] || p->update)
		Con_Printf("  version: %s%s%s%s\n", m->version[0] ? m->version : "?", p->update ? " (installed: " : "", p->update ? (p->oldversion[0] ? p->oldversion : "?") : "", p->update ? ")" : "");
	if (m->author[0])
		Con_Printf("  author: %s\n", m->author);
	if (m->homepage[0])
		Con_Printf("  homepage: %s\n", m->homepage);
	if (m->description[0])
		Con_Printf("  %s\n", m->description);
	dpsnprintf(num, sizeof(num), "%d", m->api);
	Con_Printf("  api: %s, %d files, %u KB, sha256 %s%s\n", m->api ? num : "legacy (no api: line)", p->numfiles, (unsigned)((p->datalen + 1023) >> 10), p->sha, p->pin[0] ? " (^2matches the pinned hash^7)" : "");
	Con_Printf("  permissions it asks for (none is granted by installing):\n");
	for (i = 0; tlua_permissions[i].name; i++)
	{
		unsigned bit = tlua_permissions[i].bit;
		if (!(m->requested & bit))
			continue;
		if (p->update && !(p->oldrequested & bit))
			added |= bit;
		Con_Printf("    %s%s^7: %s\n", (p->update && !(p->oldrequested & bit)) ? "^3NEW " : "^7", tlua_permissions[i].name,
			m->reasons[i][0] ? m->reasons[i] : tlua_permissions[i].help);
	}
	if (!m->requested)
		Con_Printf("    none\n");
	if (!m->scoped)
		Con_Printf("  ^3a legacy mod (no api: line): it gets the HUD set of permissions once turned on^7\n");
	if (added)
		Con_Printf("  ^3this update asks for new permissions; they stay ungranted until you grant them^7\n");
	Con_Printf("  ^2lua_install_confirm^7 installs it (it stays %s), ^1lua_install_cancel^7 drops it\n", p->update ? "on or off as it is" : "off");
	dpsnprintf(p->message, sizeof(p->message), p->update ? "Update %s: review the permissions, then update (it stays on or off; new permissions stay ungranted)" : "Install %s: review the permissions, then install (it stays off; nothing is granted)", m->name);
}

// the package bytes (an archive) arrived: check, unpack in memory, review
static void tlua_pkg_got(const unsigned char *bytes, size_t len, const char *stem)
{
	char sha[65];
	tlua_sha256_hex(bytes, len, sha);
	if (tlua_pkg.pin[0] && strcasecmp(sha, tlua_pkg.pin))
	{
		tlua_pkg_fail("sha256 mismatch: the package is %s, the pinned hash %s", sha, tlua_pkg.pin);
		return;
	}
	if (!tlua_pkg_readzip(bytes, len))
		return;
	tlua_pkg_review(stem);
	if (tlua_pkg.state != TLUA_PKG_REVIEW)
		return;
	dp_strlcpy(tlua_pkg.sha, sha, sizeof(tlua_pkg.sha));
	tlua_pkg_print();
	tlua_pkg_publish();
}

// ---------------------------------------------------------------------------
// Downloads

// libcurl.c asks this for each transfer (the // Touch: hook there): ours stay
// on HTTPS, redirects included; 0 leaves a transfer alone
static void tlua_dl_cb(int status, size_t length, unsigned char *buffer, void *cbdata);
static qbool tlua_dl_localhttp;

long TouchLua_CurlProtocols(curl_callback_t callback, qbool redirect)
{
	long TLua_Http_Protocols(curl_callback_t callback, qbool redirect);
	if (callback != tlua_dl_cb)
		return TLua_Http_Protocols(callback, redirect); // xt.http (touch_lua_api.c)
	// 1: CURLPROTO_HTTP, 2: CURLPROTO_HTTPS
	return (tlua_dl_localhttp && !redirect) ? (1 | 2) : 2;
}

static void tlua_dl_cb(int status, size_t length, unsigned char *buffer, void *cbdata)
{
	char stem[64] = "";
	const char *b, *e;
	if (cbdata != &tlua_dltoken || tlua_pkg.state != TLUA_PKG_DOWNLOADING)
		return;
	if (status != CURLCBSTATUS_OK)
	{
		if (status == CURLCBSTATUS_FAILED && length > TLUA_PKG_MAXSIZE)
			tlua_pkg_fail("the download is over 8 MB");
		else if (status > 0)
			tlua_pkg_fail("the server answered HTTP %d", status);
		else
			tlua_pkg_fail("the download failed (%s)", status == CURLCBSTATUS_ABORTED ? "cancelled" : length > TLUA_PKG_MAXSIZE ? "over 8 MB" : "network, TLS or a redirect away from https");
		return;
	}
	// a stem from the URL's last path component, for a package with no id
	b = strrchr(tlua_pkg.source, '/');
	b = b ? b + 1 : tlua_pkg.source;
	e = strchr(b, '?');
	dp_strlcpy(stem, b, min(sizeof(stem), (size_t)((e ? e : b + strlen(b)) - b) + 1));
	if ((e = strstr(stem, ".zip")))
		stem[e - stem] = 0;
	tlua_pkg_got(buffer, length, stem);
	if (tlua_dlbuf)
	{
		Mem_Free(tlua_dlbuf);
		tlua_dlbuf = NULL;
	}
}

static qbool tlua_isloopbackhttp(const char *url)
{
	return !strncmp(url, "http://127.0.0.1:", 17) || !strncmp(url, "http://127.0.0.1/", 17);
}

// ---------------------------------------------------------------------------
// Starting an install

static void tlua_pkg_cancel_download(void)
{
	if (tlua_pkg.state == TLUA_PKG_DOWNLOADING)
		Curl_Cancel_ToMemory(tlua_dl_cb, &tlua_dltoken);
	if (tlua_dlbuf)
	{
		Mem_Free(tlua_dlbuf);
		tlua_dlbuf = NULL;
	}
}

static void tlua_pkg_begin(tlua_realm_t *r, const char *arg, const char *pinarg)
{
	tlua_pkg_t *p = &tlua_pkg;
	char src[1024], *frag;
	tlua_pkg_cancel_download();
	tlua_pkg_free();
	memset(&p->info, 0, sizeof(p->info));
	p->realm = r;
	p->state = TLUA_PKG_IDLE;
	p->update = false;
	p->pin[0] = p->sha[0] = p->message[0] = 0;
	p->started = host.realtime;
	dp_strlcpy(src, arg, sizeof(src));
	if ((frag = strstr(src, "#sha256=")))
	{
		dp_strlcpy(p->pin, frag + 8, sizeof(p->pin));
		*frag = 0;
	}
	if (pinarg && *pinarg)
		dp_strlcpy(p->pin, pinarg, sizeof(p->pin));
	dp_strlcpy(p->source, src, sizeof(p->source));
	if (p->pin[0])
	{
		size_t i;
		for (i = 0; p->pin[i]; i++)
			if (!isxdigit((unsigned char)p->pin[i]))
				break;
		if (i != 64 || p->pin[i])
		{
			tlua_pkg_fail("a pinned sha256 is 64 hex digits");
			return;
		}
	}
	if (!strncmp(src, "https://", 8) || !strncmp(src, "http://", 7) || strstr(src, "://"))
	{
		tlua_dl_localhttp = false;
		if (strncmp(src, "https://", 8))
		{
			if (lua_install_insecure_localhost.integer && tlua_isloopbackhttp(src))
				tlua_dl_localhttp = true;
			else
			{
				tlua_pkg_fail("only https:// URLs (or a file on this device)");
				return;
			}
		}
		if (strpbrk(src, " \"'\\;$\n\r\t"))
		{
			tlua_pkg_fail("the URL has spaces, quotes or other characters a URL does not need");
			return;
		}
		if (!Curl_Available())
		{
			tlua_pkg_fail("downloads are not available in this build (no libcurl): download the .zip and install it from the file");
			return;
		}
		tlua_dlbuf = (unsigned char *)Mem_Alloc(tempmempool, TLUA_PKG_MAXSIZE);
		p->state = TLUA_PKG_DOWNLOADING;
		dpsnprintf(p->message, sizeof(p->message), "downloading %s", src);
		if (!Curl_Begin_ToMemory(src, 0, tlua_dlbuf, TLUA_PKG_MAXSIZE, tlua_dl_cb, &tlua_dltoken))
		{
			tlua_pkg_fail("the download could not start (curl_enabled 0, or already downloading it)");
			return;
		}
		TLua_Say("Lua: downloading %s (the game keeps running; lua_install_status)", src);
		tlua_pkg_publish();
		return;
	}
	// a file or folder on this device
	{
		char path[1024];
		const char *base, *dot;
		char stem[64];
#ifndef _WIN32
		struct stat st;
#endif
		if (src[0] == '/')
			dp_strlcpy(path, src, sizeof(path));
		else
			dpsnprintf(path, sizeof(path), "%s%s", fs_userdir, src);
		base = strrchr(path, '/');
		base = base ? base + 1 : path;
		dp_strlcpy(stem, base, sizeof(stem));
		if ((dot = strrchr(stem, '.')))
			stem[dot - stem] = 0;
#ifndef _WIN32
		if (stat(path, &st))
		{
			tlua_pkg_fail("no such file or folder: %s", path);
			return;
		}
		if (S_ISDIR(st.st_mode))
		{
			size_t total = 0;
			tlua_pkg.data = (unsigned char *)Mem_Alloc(tempmempool, TLUA_PKG_MAXSIZE);
			if (!tlua_pkg_readdir_r(path, "", 0, &total))
				return;
			tlua_pkg.datalen = total;
			// a folder named like the mod: its name counts as the top folder's
			tlua_pkg_review(stem);
			if (p->state == TLUA_PKG_REVIEW)
			{
				dp_strlcpy(p->sha, "(a folder)", sizeof(p->sha));
				if (p->pin[0])
				{
					tlua_pkg_fail("a pinned sha256 needs an archive, not a folder");
					return;
				}
				tlua_pkg_print();
				tlua_pkg_publish();
			}
			return;
		}
		if (!S_ISREG(st.st_mode))
		{
			tlua_pkg_fail("%s is not a file", path);
			return;
		}
		if (st.st_size > TLUA_PKG_MAXSIZE)
		{
			tlua_pkg_fail("the file is over 8 MB");
			return;
		}
		{
			int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			unsigned char *buf;
			size_t got = 0;
			if (fd < 0)
			{
				tlua_pkg_fail("cannot open %s", path);
				return;
			}
			buf = (unsigned char *)Mem_Alloc(tempmempool, (size_t)st.st_size + 1);
			while (got < (size_t)st.st_size)
			{
				ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					break;
				got += (size_t)n;
			}
			close(fd);
			if (got != (size_t)st.st_size)
			{
				Mem_Free(buf);
				tlua_pkg_fail("could not read %s", path);
				return;
			}
			tlua_pkg_got(buf, got, stem);
			Mem_Free(buf);
		}
#else
		tlua_pkg_fail("installing is not supported on this platform");
#endif
	}
}

// ---------------------------------------------------------------------------
// Writing it: <realm>/.install.tmp/, then renamed into place

#ifndef _WIN32
// whether the last tlua_rmtree with keepdata found a data/ worth keeping
static qbool tlua_rmtree_kept;

// is <fd>/<name> a real folder (not a symlink) with something in it
static qbool tlua_dir_has_entries(int fd, const char *name)
{
	int dfd = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	DIR *d;
	struct dirent *ent;
	qbool any = false;
	if (dfd < 0)
		return false;
	if (!(d = fdopendir(dfd)))
	{
		close(dfd);
		return false;
	}
	while ((ent = readdir(d)))
		if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
		{
			any = true;
			break;
		}
	closedir(d);
	return any;
}

// removes a folder below parentfd without following any symlink in it;
// keepdata keeps its top-level data/ (and so the folder itself) when there
// is anything in it: a mod that never saved anything leaves no empty folder
static qbool tlua_rmtree(int parentfd, const char *name, int depth, qbool keepdata)
{
	int fd;
	DIR *d;
	struct dirent *ent;
	qbool ok = true;
	qbool kept = false;
	if (depth > 16)
		return false;
	fd = openat(parentfd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
	{
		if (errno == ENOENT)
			return true;
		// a symlink or a file: remove the entry itself, never what it points to
		return unlinkat(parentfd, name, 0) == 0;
	}
	if (!(d = fdopendir(fd)))
	{
		close(fd);
		return false;
	}
	while ((ent = readdir(d)))
	{
		struct stat st;
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;
		if (keepdata && depth == 0 && !strcmp(ent->d_name, "data") && tlua_dir_has_entries(dirfd(d), ent->d_name))
		{
			kept = true;
			continue;
		}
		if (fstatat(dirfd(d), ent->d_name, &st, AT_SYMLINK_NOFOLLOW))
		{
			ok = false;
			continue;
		}
		if (S_ISDIR(st.st_mode))
			ok &= tlua_rmtree(dirfd(d), ent->d_name, depth + 1, false);
		else if (unlinkat(dirfd(d), ent->d_name, 0))
			ok = false;
	}
	closedir(d);
	if (depth == 0)
		tlua_rmtree_kept = kept;
	if (!kept && unlinkat(parentfd, name, AT_REMOVEDIR))
		ok = false;
	return ok;
}

// is <realm>/<name> a real folder (not a symlink); -1 when it does not exist
static int tlua_realdir_at(int realmfd, const char *name)
{
	struct stat st;
	if (fstatat(realmfd, name, &st, AT_SYMLINK_NOFOLLOW))
		return errno == ENOENT ? -1 : 0;
	return S_ISDIR(st.st_mode) ? 1 : 0;
}
#endif

static void tlua_pkg_sourcepath(tlua_realm_t *r, const char *name, char *dir, size_t dirsize, char *file, size_t filesize)
{
	TLua_StateDir(r, dir, dirsize);
	dp_strlcat(dir, "sources/", dirsize);
	dpsnprintf(file, filesize, "%s.txt", name);
}

static void tlua_grantfile(tlua_realm_t *r, const char *name, char *dir, size_t dirsize, char *file, size_t filesize)
{
	TLua_StateDir(r, dir, dirsize);
	dp_strlcat(dir, "grants/", dirsize);
	dpsnprintf(file, filesize, "%s.txt", name);
}

void TLua_Pkg_Forget(tlua_realm_t *r, const char *name)
{
	char dir[1024], file[64], path[1100];
	tlua_grantfile(r, name, dir, sizeof(dir), file, sizeof(file));
	dpsnprintf(path, sizeof(path), "%s%s", dir, file);
	remove(path);
	tlua_pkg_sourcepath(r, name, dir, sizeof(dir), file, sizeof(file));
	dpsnprintf(path, sizeof(path), "%s%s", dir, file);
	remove(path);
}

static void tlua_pkg_confirm(void)
{
#ifndef _WIN32
	tlua_pkg_t *p = &tlua_pkg;
	tlua_realm_t *r = p->realm;
	tlua_mod_t *m;
	char dir[1024], rel[TLUA_PKG_MAXPATH + 32];
	int realmfd, i, ex;
	qbool wasenabled = false, wasrunning = false;
	unsigned newperms = 0;
	const char *name = p->info.name;
	TLua_RealmDir(r, dir, sizeof(dir));
	FS_CreatePath(dir);
	realmfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (realmfd < 0)
	{
		tlua_pkg_fail("cannot open %s", dir);
		return;
	}
	ex = tlua_realdir_at(realmfd, name);
	if (ex == 0)
	{
		close(realmfd);
		tlua_pkg_fail("%s%s exists and is not a real folder (a symlink or a file): remove it first", dir, name);
		return;
	}
	tlua_rmtree(realmfd, ".install.tmp", 0, false);
	tlua_rmtree(realmfd, ".old.tmp", 0, false);
	for (i = 0; i < p->numfiles; i++)
	{
		tlua_pkgfile_t *f = &p->files[i];
		int fd;
		size_t done = 0;
		dpsnprintf(rel, sizeof(rel), ".install.tmp/%s", f->path);
		fd = TLua_OpenConfined(dir, rel, TLUA_WR, true);
		if (fd < 0)
		{
			tlua_rmtree(realmfd, ".install.tmp", 0, false);
			close(realmfd);
			tlua_pkg_fail("could not write %s", f->path);
			return;
		}
		while (done < f->len)
		{
			ssize_t w = write(fd, p->data + f->off + done, f->len - done);
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0)
				break;
			done += (size_t)w;
		}
		close(fd);
		if (done != f->len)
		{
			tlua_rmtree(realmfd, ".install.tmp", 0, false);
			close(realmfd);
			tlua_pkg_fail("could not write %s (disk full?)", f->path);
			return;
		}
	}
	// stop the old version; keep its on/off choice
	TLua_Rescan(r);
	if ((m = TLua_Find(r, name)))
	{
		wasenabled = m->enabled;
		wasrunning = m->L != NULL;
		TLua_Stop(m);
	}
	if (ex == 1)
	{
		char olddata[TLUA_NAME_LEN + 8];
		dpsnprintf(olddata, sizeof(olddata), "%s/data", name);
		// the mod's storage moves over to the new version
		if (tlua_realdir_at(realmfd, olddata) == 1 && renameat(realmfd, olddata, realmfd, ".install.tmp/data"))
		{
			tlua_rmtree(realmfd, ".install.tmp", 0, false);
			close(realmfd);
			tlua_pkg_fail("could not keep the mod's data folder");
			return;
		}
		if (renameat(realmfd, name, realmfd, ".old.tmp"))
		{
			close(realmfd);
			tlua_pkg_fail("could not move the old version away");
			return;
		}
	}
	if (renameat(realmfd, ".install.tmp", realmfd, name))
	{
		if (ex == 1)
			renameat(realmfd, ".old.tmp", realmfd, name);
		close(realmfd);
		tlua_pkg_fail("could not put the new folder in place");
		return;
	}
	tlua_rmtree(realmfd, ".old.tmp", 0, false);
	close(realmfd);
	// grants: a fresh install has none (a stale file from before is removed);
	// an update keeps only what the new version still requests
	{
		char gdir[1024], gfile[64], text[32];
		tlua_grantfile(r, name, gdir, sizeof(gdir), gfile, sizeof(gfile));
		if (!p->update)
		{
			char path[1100];
			dpsnprintf(path, sizeof(path), "%s%s", gdir, gfile);
			remove(path);
		}
		else if (p->oldscoped == p->info.scoped)
		{
			dpsnprintf(text, sizeof(text), "%s %x\n", p->info.scoped ? "v1" : "legacy", p->oldgranted & p->info.requested);
			TLua_WriteState(gdir, gfile, text, true);
		}
		newperms = p->update ? p->info.requested & ~p->oldrequested : 0;
	}
	// where it came from, for lua_mod_update
	{
		char sdir[1024], sfile[64], text[700];
		tlua_pkg_sourcepath(r, name, sdir, sizeof(sdir), sfile, sizeof(sfile));
		dpsnprintf(text, sizeof(text), "source %s\nsha256 %s\n", p->source, p->sha);
		TLua_WriteState(sdir, sfile, text, true);
	}
	TLua_Rescan(r);
	m = TLua_Find(r, name);
	if (m)
	{
		TLua_Log(m, "%s from %s", p->update ? "updated" : "installed", p->source);
		if (wasenabled && wasrunning && r->loaded && r->enable->integer && m->enabled)
			TLua_Start(m);
	}
	TLua_UpdateStatus(r);
	TLua_Hooks_Changed();
	p->state = TLUA_PKG_DONE;
	if (newperms)
	{
		char list[256] = "";
		for (i = 0; tlua_permissions[i].name; i++)
			if (newperms & tlua_permissions[i].bit)
			{
				if (list[0])
					dp_strlcat(list, ", ", sizeof(list));
				dp_strlcat(list, tlua_permissions[i].name, sizeof(list));
			}
		dpsnprintf(p->message, sizeof(p->message), "updated %s; it asks for new permissions, not granted: %s", name, list);
	}
	else if (p->update)
		dpsnprintf(p->message, sizeof(p->message), "updated %s (%s)", name, m && m->enabled ? "still on" : "off");
	else
		dpsnprintf(p->message, sizeof(p->message), "installed %s: it is off and has no permissions; grant what it needs and turn it on", name);
	TLua_Say("^2Lua:^7 %s", p->message);
	tlua_pkg_free();
	tlua_pkg_publish();
#else
	tlua_pkg_fail("installing is not supported on this platform");
#endif
}

// ---------------------------------------------------------------------------
// Commands

static tlua_realm_t *tlua_pkg_realm(cmd_state_t *cmd)
{
	return !strncmp(Cmd_Argv(cmd, 0), "sv_", 3) ? &tlua_server : &tlua_client;
}

static void tlua_install_f(cmd_state_t *cmd)
{
	if (TLua_RemoteRefused(cmd))
		return;
	if (Cmd_Argc(cmd) < 2)
	{
		Con_Printf("usage: %s <file.zip | folder | https://...zip[#sha256=<hex>]> [sha256]\n", Cmd_Argv(cmd, 0));
		return;
	}
	tlua_pkg_begin(tlua_pkg_realm(cmd), Cmd_Argv(cmd, 1), Cmd_Argc(cmd) >= 3 ? Cmd_Argv(cmd, 2) : NULL);
}

static void tlua_confirm_f(cmd_state_t *cmd)
{
	if (TLua_RemoteRefused(cmd))
		return;
	if (tlua_pkg.state != TLUA_PKG_REVIEW)
	{
		TLua_Say("lua_install_confirm: nothing to install (lua_mod_install first)");
		return;
	}
	tlua_pkg_confirm();
}

static void tlua_cancel_f(cmd_state_t *cmd)
{
	if (TLua_RemoteRefused(cmd))
		return;
	tlua_pkg_cancel_download();
	tlua_pkg_free();
	tlua_pkg.state = TLUA_PKG_IDLE;
	dp_strlcpy(tlua_pkg.message, "cancelled", sizeof(tlua_pkg.message));
	TLua_Say("Lua: install cancelled");
	tlua_pkg_publish();
}

static void tlua_status_f(cmd_state_t *cmd)
{
	(void)cmd;
	Con_Printf("Lua install: %s%s%s\n", tlua_pkg_statename(tlua_pkg.state), tlua_pkg.message[0] ? ": " : "", tlua_pkg.message);
	if (tlua_pkg.state == TLUA_PKG_DOWNLOADING)
		Con_Printf("  %.0f s so far\n", host.realtime - tlua_pkg.started);
	if (tlua_pkg.state == TLUA_PKG_REVIEW)
		tlua_pkg_print();
}

static void tlua_uninstall_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_pkg_realm(cmd);
	tlua_mod_t *m;
	const char *name;
	qbool keepdata;
	char dir[1024];
	if (TLua_RemoteRefused(cmd))
		return;
	if (Cmd_Argc(cmd) < 2)
	{
		Con_Printf("usage: %s <name> [keepdata]\n", Cmd_Argv(cmd, 0));
		return;
	}
	name = Cmd_Argv(cmd, 1);
	keepdata = Cmd_Argc(cmd) >= 3 && !strcmp(Cmd_Argv(cmd, 2), "keepdata");
	TLua_Rescan(r);
	if (!(m = TLua_Find(r, name)))
	{
		TLua_Say("%s: no %s named %s", Cmd_Argv(cmd, 0), TLUA_NOUN(r), name);
		return;
	}
	m->enabled = false;
	TLua_WriteEnabled(r);
	TLua_Stop(m);
	TLua_RealmDir(r, dir, sizeof(dir));
#ifndef _WIN32
	{
		int realmfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		qbool ok = realmfd >= 0 && tlua_rmtree(realmfd, name, 0, keepdata);
		if (realmfd >= 0)
			close(realmfd);
		if (!ok)
		{
			TLua_Say("^1%s: could not remove everything in %s%s^7", Cmd_Argv(cmd, 0), dir, name);
			TLua_Rescan(r);
			return;
		}
	}
#endif
	TLua_Pkg_Forget(r, name);
	TLua_Rescan(r);
	TLua_Hooks_Changed();
#ifndef _WIN32
	if (keepdata && !tlua_rmtree_kept)
		keepdata = false; // it had no saved data: nothing was kept, not even an empty folder
#endif
	TLua_Say("^2Lua:^7 uninstalled %s %s%s", TLUA_NOUN(r), name, keepdata ? " (kept its data/ folder)" : " (its code, data, grants and source record are gone)");
}

static void tlua_update_f(cmd_state_t *cmd)
{
	tlua_realm_t *r = tlua_pkg_realm(cmd);
	tlua_mod_t *m;
	char sdir[1024], sfile[64], path[1100], buf[1024], src[600] = "";
	if (TLua_RemoteRefused(cmd))
		return;
	if (Cmd_Argc(cmd) < 2)
	{
		Con_Printf("usage: %s <name> [file | https://...]\n", Cmd_Argv(cmd, 0));
		return;
	}
	TLua_Rescan(r);
	if (!(m = TLua_Find(r, Cmd_Argv(cmd, 1))))
	{
		TLua_Say("%s: no %s named %s", Cmd_Argv(cmd, 0), TLUA_NOUN(r), Cmd_Argv(cmd, 1));
		return;
	}
	if (Cmd_Argc(cmd) >= 3)
		dp_strlcpy(src, Cmd_Argv(cmd, 2), sizeof(src));
	else
	{
		tlua_pkg_sourcepath(r, m->name, sdir, sizeof(sdir), sfile, sizeof(sfile));
		dpsnprintf(path, sizeof(path), "%s%s", sdir, sfile);
		if (TLua_ReadState(path, buf, sizeof(buf)) > 0 && !strncmp(buf, "source ", 7))
		{
			buf[strcspn(buf, "\r\n")] = 0;
			dp_strlcpy(src, buf + 7, sizeof(src));
		}
		else if (m->updateurl[0])
			dp_strlcpy(src, m->updateurl, sizeof(src));
	}
	if (!src[0])
	{
		TLua_Say("%s: %s was not installed from a package and its mod.txt has no update: URL; give a file or URL", Cmd_Argv(cmd, 0), m->name);
		return;
	}
	tlua_pkg_begin(r, src, NULL);
	if (tlua_pkg.state == TLUA_PKG_REVIEW && strcmp(tlua_pkg.info.name, m->name))
		tlua_pkg_fail("that package is %s, not %s", tlua_pkg.info.name, m->name);
}

// The launcher (or anything outside the game) asks for an install by leaving
// <userdir>/luastate/incoming/request.txt: "mod <path>" or "plugin <path>". It
// is read once, at the first frame, removed, and goes through the same review;
// the Mods tab opens on it. Only the user's own files can put it there.
static void tlua_pkg_request(void)
{
	char path[1024], buf[1100], *p;
	tlua_realm_t *r;
	dpsnprintf(path, sizeof(path), "%sluastate/incoming/request.txt", fs_userdir);
	if (TLua_ReadState(path, buf, sizeof(buf)) <= 0)
		return;
	remove(path);
	buf[strcspn(buf, "\r\n")] = 0;
	if (!strncmp(buf, "mod ", 4))
		r = &tlua_client, p = buf + 4;
	else if (!strncmp(buf, "plugin ", 7))
		r = &tlua_server, p = buf + 7;
	else
		return;
	Con_Printf("Lua: install requested from outside the game: %s\n", p);
	tlua_pkg_begin(r, p, NULL);
	if (cls.state != ca_dedicated)
		Cbuf_AddText(cmd_local, "\ntogglemenu 1\nmenu_cmd directmenu touchmods\n");
}

void TLua_Pkg_Frame(void)
{
	static unsigned lastframe;
	static double lastpub;
	static qbool asked;
	if (lastframe == host.framecount)
		return;
	lastframe = host.framecount;
	if (!asked)
	{
		asked = true;
		tlua_pkg_request();
	}
	if (tlua_pkg.state == TLUA_PKG_DOWNLOADING && host.realtime - lastpub > 0.5)
	{
		lastpub = host.realtime;
		dpsnprintf(tlua_pkg.message, sizeof(tlua_pkg.message), "downloading %s (%.0f s)", tlua_pkg.source, host.realtime - tlua_pkg.started);
		tlua_pkg_publish();
	}
}

void TLua_Pkg_Setup(void)
{
	TLua_RegisterCvar(&lua_install_insecure_localhost);
	TLua_RegisterCvar(&lua_install_state);
	TLua_Guard(&lua_install_insecure_localhost);
	Cmd_AddCommand(CF_CLIENT, "lua_mod_install", tlua_install_f, "review a Lua client mod package (.zip, folder or https:// URL[#sha256=...]) before installing it");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_update", tlua_update_f, "fetch a Lua client mod again from where it came from (or a given file or URL) and review it");
	Cmd_AddCommand(CF_CLIENT, "lua_mod_uninstall", tlua_uninstall_f, "remove a Lua client mod (folder, grants, record; keepdata keeps its data/)");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_install", tlua_install_f, "review a Lua server mod package (.zip, folder or https:// URL[#sha256=...]) before installing it");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_update", tlua_update_f, "fetch a Lua server mod again from where it came from (or a given file or URL) and review it");
	Cmd_AddCommand(CF_SERVER, "sv_lua_mod_uninstall", tlua_uninstall_f, "remove a Lua server mod (folder, grants, record; keepdata keeps its data/)");
	// old names, kept so scripts and configs go on working
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_install", tlua_install_f, "old name of sv_lua_mod_install");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_update", tlua_update_f, "old name of sv_lua_mod_update");
	Cmd_AddCommand(CF_SERVER, "sv_lua_plugin_uninstall", tlua_uninstall_f, "old name of sv_lua_mod_uninstall");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_install_confirm", tlua_confirm_f, "install the package lua_mod_install / sv_lua_mod_install showed (it stays off, nothing is granted)");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_install_cancel", tlua_cancel_f, "drop the pending Lua install or download");
	Cmd_AddCommand(CF_CLIENT | CF_SERVER, "lua_install_status", tlua_status_f, "what the pending Lua install is doing");
}
