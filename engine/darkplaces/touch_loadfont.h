// Godnotic: font sizes rendered when first drawn, kerning looked up per pair.
// Included by ft2.c (after its prototypes) for its static helpers.
//
// A `loadfont` line asks for up to 16 sizes and ft2.c rendered every one at
// load: a 256-glyph map with its post-processing and upload, and a 256x256
// kerning table (131072 FT_Get_Char_Index + 65536 FT_Get_Kerning calls,
// 512 KiB). Xonotic's CSQC fonts ask for 12 sizes each and the renderer loads
// all fonts at every start: about 1 s on a Mali phone.
//
// r_font_lazysizes 1: the first size that loads is rendered at once, the rest
// are remembered and rendered when Font_IndexForSize would pick one of them,
// or earlier in idle time: one per frame at the main menu, all of them under a
// loading screen (TouchFont_LoadPendingOne). Text measures and draws exactly
// as before, since a size is always there before it is chosen.
// r_font_lazykerning 1: no table at load; a pair is asked of FreeType the first
// time it is drawn and kept in the table, allocated on first use.

#include <math.h>

static cvar_t r_font_lazysizes = {CF_CLIENT, "r_font_lazysizes", "1", "render font sizes when they are first drawn (or in idle frames) instead of all at load (Godnotic); 0 = all at load"};
static cvar_t r_font_lazykerning = {CF_CLIENT, "r_font_lazykerning", "1", "look up kerning of the first 256 characters per pair when drawn instead of building a 256x256 table per font size at load (Godnotic)"};

static qbool Font_SetSize(ft2_font_t *font, float w, float h);
static qbool Font_LoadSize(ft2_font_t *font, float size, qbool check_only);

#define TOUCHFONT_MAX 64
typedef struct touchfont_pending_s
{
	ft2_font_t *font;
	int n;
	float sizes[MAX_FONT_SIZES];
}
touchfont_pending_t;
static touchfont_pending_t touchfont_pending[TOUCHFONT_MAX];
static qbool touchfont_loading;

static void TouchFont_Init(void)
{
	Cvar_RegisterVariable(&r_font_lazysizes);
	Cvar_RegisterVariable(&r_font_lazykerning);
}

static touchfont_pending_t *TouchFont_Find(ft2_font_t *font, qbool create)
{
	int i, empty = -1;
	for (i = 0; i < TOUCHFONT_MAX; i++)
	{
		if (touchfont_pending[i].font == font)
			return &touchfont_pending[i];
		if (empty < 0 && !touchfont_pending[i].font)
			empty = i;
	}
	if (!create || empty < 0)
		return NULL;
	touchfont_pending[empty].font = font;
	touchfont_pending[empty].n = 0;
	return &touchfont_pending[empty];
}

static void TouchFont_Forget(ft2_font_t *font)
{
	touchfont_pending_t *p = TouchFont_Find(font, false);
	if (p)
	{
		p->font = NULL;
		p->n = 0;
	}
}

// Font_LoadFont's size loop: true when this size is left for later.
static qbool TouchFont_DeferSize(ft2_font_t *font, float size)
{
	touchfont_pending_t *p;
	int i;
	if (!r_font_lazysizes.integer || touchfont_loading || font->image_font || !(size >= 2 && size < 1000))
		return false;
	for (i = 0; i < MAX_FONT_SIZES; i++)
		if (font->font_maps[i] && font->font_maps[i]->size == size)
			return false; // already there, Font_LoadSize returns at once
	if (!(p = TouchFont_Find(font, true)))
		return false;
	for (i = 0; i < p->n; i++)
		if (p->sizes[i] == size)
			return true;
	if (p->n >= MAX_FONT_SIZES)
		return false;
	p->sizes[p->n++] = size;
	return true;
}

// Render pending size k of p now; the dp_font_t using this ft2 font gets
// the widths LoadFont computes for each map.
static void TouchFont_LoadPending(touchfont_pending_t *p, int k)
{
	ft2_font_t *font = p->font;
	float size = p->sizes[k];
	int i, m, ch;

	p->sizes[k] = p->sizes[--p->n];
	touchfont_loading = true;
	if (!Font_LoadSize(font, size, false))
	{
		touchfont_loading = false;
		return;
	}
	touchfont_loading = false;
	for (m = 0; m < MAX_FONT_SIZES; m++)
		if (font->font_maps[m] && font->font_maps[m]->size == size)
			break;
	if (m >= MAX_FONT_SIZES)
		return;
	for (i = 0; i < dp_fonts.maxsize; i++)
	{
		dp_font_t *f = &dp_fonts.f[i];
		if (f->ft2 != font)
			continue;
		for (ch = 0; ch < 256; ch++)
			f->width_of_ft2[m][ch] = Font_SnapTo(f->width_of[ch], 1 / font->font_maps[m]->size);
	}
}

// Font_IndexForSize, before it chooses: if a pending size would be the
// better match by its own measure, render that one first.
static void TouchFont_EnsureSize(ft2_font_t *font, float fsize, float *outw, float *outh)
{
	touchfont_pending_t *p;
	float fx, fy, best = 1e30f, bestsize = -1, val;
	int m, k, pick = -1;

	if (touchfont_loading || !(p = TouchFont_Find(font, false)) || !p->n)
		return;
	fx = fy = fsize * vid.mode.height / vid_conheight.value;
	if (outw && *outw)
		fx = *outw * vid.mode.width / vid_conwidth.value;
	if (outh && *outh)
		fy = *outh * vid.mode.height / vid_conheight.value;
	if (fx < 0)
		fx = fy = (fy < 0) ? 16 : fy;
	else if (fy < 0)
		fy = fx;
	for (m = 0; m < MAX_FONT_SIZES; m++)
	{
		if (!font->font_maps[m])
			continue;
		val = 0.5f * (fabs(font->font_maps[m]->size - fx) + fabs(font->font_maps[m]->size - fy));
		if (val < best || (val == best && font->font_maps[m]->size > bestsize))
		{
			best = val;
			bestsize = font->font_maps[m]->size;
		}
	}
	for (k = 0; k < p->n; k++)
	{
		val = 0.5f * (fabs(p->sizes[k] - fx) + fabs(p->sizes[k] - fy));
		if (val < best || (val == best && p->sizes[k] > bestsize))
		{
			best = val;
			bestsize = p->sizes[k];
			pick = k;
		}
	}
	if (pick >= 0)
		TouchFont_LoadPending(p, pick);
}

// Idle time: render one pending size; false when none is left.
qbool TouchFont_LoadPendingOne(void)
{
	int i;
	for (i = 0; i < TOUCHFONT_MAX; i++)
	{
		if (touchfont_pending[i].font && touchfont_pending[i].n)
		{
			TouchFont_LoadPending(&touchfont_pending[i], touchfont_pending[i].n - 1);
			return true;
		}
	}
	return false;
}

// Font_LoadSize: true to skip the 256x256 kerning table.
static qbool TouchFont_LazyKerning(ft2_font_t *font)
{
	return r_font_lazykerning.integer != 0;
}

// Font_GetKerningForMap for both characters below 256.
static qbool TouchFont_Kerning(ft2_font_t *font, ft2_font_map_t *fmap, Uchar left, Uchar right, float *outx, float *outy)
{
	ft2_kernvec *k;
	if (!fmap->kerning)
	{
		int l, r;
		fmap->kerning = (ft2_kerning_t *)Mem_Alloc(font_mempool, sizeof(ft2_kerning_t));
		for (l = 0; l < 256; l++)
			for (r = 0; r < 256; r++)
				fmap->kerning->kerning[l][r][0] = NAN;
	}
	k = &fmap->kerning->kerning[left][right];
	if (isnan((*k)[0]))
	{
		FT_Vector kernvec;
		FT_ULong ul, ur;
		(*k)[0] = (*k)[1] = 0;
		if (Font_SetSize(font, fmap->intSize, fmap->intSize))
		{
			ul = qFT_Get_Char_Index((FT_Face)font->face, left);
			ur = qFT_Get_Char_Index((FT_Face)font->face, right);
			if (!qFT_Get_Kerning((FT_Face)font->face, ul, ur, FT_KERNING_DEFAULT, &kernvec))
			{
				(*k)[0] = Font_SnapTo(kernvec.x * fmap->sfx, 1 / fmap->size);
				(*k)[1] = Font_SnapTo(kernvec.y * fmap->sfy, 1 / fmap->size);
			}
		}
	}
	if (outx) *outx = (*k)[0];
	if (outy) *outy = (*k)[1];
	return true;
}
