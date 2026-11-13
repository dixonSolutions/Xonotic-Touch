// Touch: ASTC textures for phones, included by gl_textures.c ahead of
// R_Textures_Init so the hooks there stay one line each:
//
//   R_Textures_Init:       Touch_KTX_Init();
//   R_LoadTextureDDSFile:  { rtexture_t *k = Touch_KTX_Load(...); if (k) return k; }
//
// The phone pack set carries dds/<name>.ktx next to (or instead of)
// dds/<name>.dds: a KTX 1 file with the whole mip chain in one ASTC block
// size, made by scripts/repack-phone-assets.py. A GPU with
// GL_KHR_texture_compression_astc_ldr (every Mali, Adreno and PowerVR of the
// last decade) takes the blocks as they are. That replaces the software DXT
// decode GPUs without S3TC need (docs/ANDROID.md): no CPU time at load, and
// the full-size texture instead of one two mip levels down.
//
// glInternalFormat gives the block size, glBaseInternalFormat (GL_RGB or
// GL_RGBA) whether the texture has any alpha, and an optional "DPAVGCOLOR"
// key the average colour (4 bytes RGBA) the DDS loader would compute.
// Anything unexpected returns NULL and the DDS file is loaded as before.

#ifndef TOUCH_ASSETS_KTX_H
#define TOUCH_ASSETS_KTX_H

#define TOUCH_KTX_GL_RGB 0x1907
#define TOUCH_KTX_ASTC_FIRST 0x93B0 // GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define TOUCH_KTX_ASTC_LAST 0x93BD // GL_COMPRESSED_RGBA_ASTC_12x12_KHR
#define TOUCH_KTX_ASTC_SRGB 0x20 // to GL_COMPRESSED_SRGB8_ALPHA8_ASTC_*
#define TOUCH_KTX_MAXLEVELS 16

// defined further down in gl_textures.c
static void GL_SetupTextureParameters(int flags, textype_t textype, int texturetype);

static cvar_t r_texture_astc_load = {CF_CLIENT | CF_ARCHIVE, "r_texture_astc_load", "1", "load the ASTC version of a DDS texture (dds/foo.ktx, from the phone packs) when the GPU supports GL_KHR_texture_compression_astc_ldr; -noastc turns it off too"};
static cvar_t r_texture_astc_log = {CF_CLIENT, "r_texture_astc_log", "0", "1: print each ASTC texture loaded and each one refused"};
static cvar_t r_texture_dds_swdecode_reduce = {CF_CLIENT | CF_ARCHIVE, "r_texture_dds_swdecode_reduce", "2", "DXT textures decoded in software (a GPU without S3TC) are kept this many mip levels below full size (2 = a quarter of the width and height, the same memory as DXT1 at full size; 0 = full size, 8 times the memory of DXT1)"};

// compressed textures report one byte per texel in r_texturestats, like DXT5
static textypeinfo_t textype_touch_astc = {"astc", TEXTYPE_DXT5, 4, 1, 1.0f, TOUCH_KTX_ASTC_FIRST, 0, 0};

static int touch_ktx_loaded, touch_ktx_refused;

// DXT textures decoded in software (a GPU without S3TC): how many, how big
// they were, what was uploaded, and the time the decode took
static struct
{
	int count;
	double seconds, begin;
	long long fullpixels;  // texels of their full-size base level
	long long uploaded;    // bytes of RGBA uploaded (whole mip chain)
	long long dxtbytes;    // bytes of the DXT data at full size, whole chain
	char largest[MAX_QPATH];
	int largest_w, largest_h;
}
touch_swdds;

static void Touch_DDS_SwDecodeBegin(void)
{
	touch_swdds.begin = Sys_DirtyTime();
}

static void Touch_DDS_SwDecodeEnd(const char *name, int fullw, int fullh, int bytesperblock, long long uploaded)
{
	long long full = (long long)fullw * fullh;
	touch_swdds.count++;
	touch_swdds.seconds += Sys_DirtyTime() - touch_swdds.begin;
	touch_swdds.fullpixels += full;
	touch_swdds.uploaded += uploaded;
	touch_swdds.dxtbytes += (((long long)(fullw + 3) / 4) * ((fullh + 3) / 4) * bytesperblock) * 4 / 3;
	if (full > (long long)touch_swdds.largest_w * touch_swdds.largest_h)
	{
		dp_strlcpy(touch_swdds.largest, name, sizeof(touch_swdds.largest));
		touch_swdds.largest_w = fullw;
		touch_swdds.largest_h = fullh;
	}
	if (r_texture_astc_log.integer)
		Con_Printf("^3%s: DXT decoded in software, %dx%d -> %lld KB\n", name, fullw, fullh, uploaded / 1024);
}

static void Touch_KTX_Stats_f(cmd_state_t *cmd)
{
	Con_Printf("ASTC textures: %d loaded, %d refused; GPU support: %s\n", touch_ktx_loaded, touch_ktx_refused,
		GL_ExtensionSupported("GL_KHR_texture_compression_astc_ldr") ? "yes" : "no");
	Con_Printf("DXT textures decoded in software: %d, %.1f ms, %.1f MB of DXT (whole chains), %.1f MB RGBA uploaded at %d mip level(s) down; full size would be %.1f MB RGBA; largest %s (%dx%d)\n",
		touch_swdds.count, touch_swdds.seconds * 1000.0, touch_swdds.dxtbytes / 1048576.0, touch_swdds.uploaded / 1048576.0,
		r_texture_dds_swdecode_reduce.integer, touch_swdds.fullpixels * 4.0 * 4 / 3 / 1048576.0, touch_swdds.largest[0] ? touch_swdds.largest : "-", touch_swdds.largest_w, touch_swdds.largest_h);
}

static void Touch_KTX_Init(void)
{
	Cvar_RegisterVariable(&r_texture_astc_load);
	Cvar_RegisterVariable(&r_texture_astc_log);
	Cvar_RegisterVariable(&r_texture_dds_swdecode_reduce);
	Cmd_AddCommand(CF_CLIENT, "r_texture_astc_stats", Touch_KTX_Stats_f, "how many ASTC (.ktx) textures were loaded");
}

// Once the GL extensions are known, before the first texture loads: a GPU
// without S3TC gets the DDS-only textures (the DXT entries the phone packs
// kept where ASTC would have been bigger, and every DDS of the full packs)
// decoded in software, and no runtime S3TC compression. The menu's
// updateCompression() sets the same values at its init and restarts the
// renderer when they changed; r_texture_dds_swdecode is not archived and
// touch/xonotic.cfg sets gl_texturecompression 1, so on such a GPU that was a
// full second renderer start (fonts, pics, shaders) on every launch.
// ASTC textures (dds/foo.ktx) load before any of this and are not decoded or
// downscaled.
static void Touch_KTX_Start(void)
{
	if (vid.support.ext_texture_compression_s3tc)
		return;
	if (r_texture_dds_swdecode.integer < 1)
		Cvar_SetValueQuick(&r_texture_dds_swdecode, 1);
	if (gl_texturecompression.integer)
		Cvar_SetValueQuick(&gl_texturecompression, 0);
}

static qbool Touch_KTX_Usable(void)
{
	static int noastc = -1;
	if (!r_texture_astc_load.integer)
		return false;
	if (vid.renderpath != RENDERPATH_GLES2 && vid.renderpath != RENDERPATH_GL32)
		return false;
	if (noastc < 0)
		noastc = Sys_CheckParm("-noastc") != 0;
	if (noastc)
		return false;
	return GL_ExtensionSupported("GL_KHR_texture_compression_astc_ldr");
}

static rtexture_t *Touch_KTX_Refuse(const char *name, const char *why, unsigned char *ktx)
{
	touch_ktx_refused++;
	if (r_texture_astc_log.integer)
		Con_Printf("^3%s: %s, loading the DDS file\n", name, why);
	if (ktx)
		Mem_Free(ktx);
	return NULL;
}

static rtexture_t *Touch_KTX_Load(rtexturepool_t *rtexturepool, const char *ddsname, qbool srgb, int flags, qbool *hasalphaflag, float *avgcolor, int miplevel)
{
	static const unsigned char ktxid[12] = {0xAB, 'K', 'T', 'X', ' ', '1', '1', 0xBB, '\r', '\n', 0x1A, '\n'};
	char ktxname[MAX_QPATH];
	size_t namelen = strlen(ddsname);
	unsigned char *ktx, *p, *end, *kvend;
	const unsigned char *levels[TOUCH_KTX_MAXLEVELS];
	unsigned int levelsizes[TOUCH_KTX_MAXLEVELS];
	unsigned int internalformat, baseformat, width, height, numlevels, kvbytes, i, bw, bh, maxsize;
	fs_offset_t ktxsize;
	GLint oldbindtexnum;
	gltexture_t *glt;
	gltexturepool_t *pool = (gltexturepool_t *)rtexturepool;
	static const unsigned char blockdims[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};

	if (!Touch_KTX_Usable())
		return NULL;
	if (namelen < 5 || namelen >= sizeof(ktxname) || strcasecmp(ddsname + namelen - 4, ".dds"))
		return NULL;
	memcpy(ktxname, ddsname, namelen - 4);
	memcpy(ktxname + namelen - 4, ".ktx", 5);

	ktx = FS_LoadFile(ktxname, tempmempool, true, &ktxsize);
	if (!ktx)
		return NULL;
	end = ktx + ktxsize;

	if (ktxsize < 64 || memcmp(ktx, ktxid, sizeof(ktxid)) || BuffLittleLong(ktx + 12) != 0x04030201
	 || BuffLittleLong(ktx + 16) != 0 // glType 0: compressed
	 || BuffLittleLong(ktx + 44) > 1 || BuffLittleLong(ktx + 48) > 1 || BuffLittleLong(ktx + 52) != 1) // 2D, no array, one face
		return Touch_KTX_Refuse(ktxname, "not a 2D KTX 1 image", ktx);
	internalformat = BuffLittleLong(ktx + 28);
	baseformat = BuffLittleLong(ktx + 32);
	width = BuffLittleLong(ktx + 36);
	height = BuffLittleLong(ktx + 40);
	numlevels = max(BuffLittleLong(ktx + 56), 1u);
	kvbytes = BuffLittleLong(ktx + 60);
	if (internalformat < TOUCH_KTX_ASTC_FIRST || internalformat > TOUCH_KTX_ASTC_LAST || width < 1 || height < 1
	 || width > 32768 || height > 32768 || numlevels > TOUCH_KTX_MAXLEVELS || kvbytes > (unsigned int)ktxsize - 64)
		return Touch_KTX_Refuse(ktxname, "not ASTC", ktx);
	bw = blockdims[internalformat - TOUCH_KTX_ASTC_FIRST][0];
	bh = blockdims[internalformat - TOUCH_KTX_ASTC_FIRST][1];

	// key/value pairs: u32 size, "key\0value", padded to 4 bytes
	if (avgcolor)
		avgcolor[0] = avgcolor[1] = avgcolor[2] = avgcolor[3] = 0.5f;
	p = ktx + 64;
	kvend = p + kvbytes;
	while (p + 4 <= kvend)
	{
		unsigned int size = BuffLittleLong(p);
		if (size > (unsigned int)(kvend - p - 4))
			break;
		if (avgcolor && size >= 11 + 4 && !memcmp(p + 4, "DPAVGCOLOR", 11))
			for (i = 0; i < 4; i++)
				avgcolor[i] = p[4 + 11 + i] * (1.0f / 255.0f);
		p += 4 + ((size + 3) & ~3u);
	}

	// the levels, largest first, each after its u32 size; a level whose size
	// is not what its dimensions need ends the chain
	p = kvend;
	for (i = 0; i < numlevels; i++)
	{
		unsigned int size, w = max(width >> i, 1u), h = max(height >> i, 1u);
		if (p + 4 > end)
			break;
		size = BuffLittleLong(p);
		if (size > (unsigned int)(end - p - 4) || size != ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16)
			break;
		levels[i] = p + 4;
		levelsizes[i] = size;
		p += 4 + ((size + 3) & ~3u);
	}
	if (i == 0)
		return Touch_KTX_Refuse(ktxname, "truncated", ktx);
	numlevels = i;

	if (baseformat == TOUCH_KTX_GL_RGB)
		flags &= ~TEXF_ALPHA;
	if (hasalphaflag)
		*hasalphaflag = (flags & TEXF_ALPHA) != 0;

	// gl_picmip and gl_max_size: start further down the chain
	maxsize = vid.maxtexturesize_2d;
	if (flags & TEXF_PICMIP)
		maxsize = bound(1, gl_max_size.integer, (int)maxsize);
	else
		miplevel = 0;
	while (numlevels > 1 && (miplevel > 0 || width > maxsize || height > maxsize))
	{
		memmove(levels, levels + 1, (numlevels - 1) * sizeof(levels[0]));
		memmove(levelsizes, levelsizes + 1, (numlevels - 1) * sizeof(levelsizes[0]));
		numlevels--;
		miplevel--;
		width = max(width >> 1, 1u);
		height = max(height >> 1, 1u);
	}

	if (!(flags & TEXF_MIPMAP) || numlevels < 2)
	{
		numlevels = 1;
		flags &= ~TEXF_MIPMAP;
	}
#ifndef GL_TEXTURE_MAX_LEVEL
	// a chain that stops before 1x1 needs GL_TEXTURE_MAX_LEVEL
	else if (max(width, height) >> (numlevels - 1) > 1)
	{
		numlevels = 1;
		flags &= ~TEXF_MIPMAP;
	}
#endif

	if (srgb && vid.support.ext_texture_srgb)
		internalformat += TOUCH_KTX_ASTC_SRGB;

	glt = (gltexture_t *)Mem_ExpandableArray_AllocRecord(&texturearray);
	dp_strlcpy(glt->identifier, ddsname, sizeof(glt->identifier));
	glt->pool = pool;
	glt->chain = pool->gltchain;
	pool->gltchain = glt;
	glt->inputwidth = width;
	glt->inputheight = height;
	glt->inputdepth = 1;
	glt->flags = flags;
	glt->textype = &textype_touch_astc;
	glt->texturetype = GLTEXTURETYPE_2D;
	glt->inputdatasize = (int)ktxsize;
	glt->glinternalformat = internalformat;
	glt->glformat = 0;
	glt->gltype = 0;
	glt->bytesperpixel = 1;
	glt->sides = 1;
	glt->gltexturetypeenum = gltexturetypeenums[glt->texturetype];
	glt->tilewidth = width;
	glt->tileheight = height;
	glt->tiledepth = 1;
	glt->miplevels = numlevels - 1;

	// texture uploading can take a while, so make sure we're sending keepalives
	CL_KeepaliveMessage(false);

	CHECKGLERROR
	GL_ActiveTexture(0);
	oldbindtexnum = R_Mesh_TexBound(0, gltexturetypeenums[glt->texturetype]);
	qglGenTextures(1, (GLuint *)&glt->texnum);CHECKGLERROR
	qglBindTexture(gltexturetypeenums[glt->texturetype], glt->texnum);CHECKGLERROR
	for (i = 0; i < numlevels; i++)
	{
		qglCompressedTexImage2D(GL_TEXTURE_2D, i, internalformat, max(width >> i, 1u), max(height >> i, 1u), 0, levelsizes[i], levels[i]);CHECKGLERROR
	}
#ifdef GL_TEXTURE_MAX_LEVEL
	if (numlevels > 1 && (max(width, height) >> (numlevels - 1)) > 1)
	{
		qglTexParameteri(gltexturetypeenums[glt->texturetype], GL_TEXTURE_MAX_LEVEL, numlevels - 1);CHECKGLERROR
	}
#endif
	GL_SetupTextureParameters(glt->flags, glt->textype->textype, glt->texturetype);
	qglBindTexture(gltexturetypeenums[glt->texturetype], oldbindtexnum);CHECKGLERROR

	touch_ktx_loaded++;
	if (r_texture_astc_log.integer)
		Con_Printf("ASTC %s: %ux%u, %u levels, %ux%u blocks%s\n", ktxname, width, height, numlevels, bw, bh, (flags & TEXF_ALPHA) ? ", alpha" : "");
	Mem_Free(ktx);
	return (rtexture_t *)glt;
}

#endif
