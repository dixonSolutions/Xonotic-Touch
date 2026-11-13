/*
Godnotic: API level 3 for client mods (docs/LUA_API.md): drawing beyond
rectangles and text, the camera, HUD panels and touch buttons the engine
places, key binds, effects, the clipboard, vibration, the entities around the
player under the offline-only rule, and the client events.

Events come from three places:
- the engine itself, so they work on every server: print, chat and
  centerprint (csprogs.c hands each line over first, one // Touch: line
  each), and what the engine sees in the player's stats every frame: weapon,
  damage, spawn, death, spectate, score, connect, disconnect, touch;
- this game's own client code (client/touch_lua.qc) through builtin #1740:
  notification, kill, hit, pickup, match, hud_overlay, scoreboard. A server
  that sends different client code has none of these: xt.csqc() says which.

The offline-only rule (touch_weapons.h TouchWep_PracticeGate, the same gate
the see-through practice helpers use): xt.entities lists players and
projectiles online only while they are on screen and not behind a wall, so a
mod cannot show anyone the player could not see. Offline against bots it
lists everything (a radar, ESP-style training).
*/

#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "csprogs.h"
#include "cl_collision.h"
#include "image.h"
#include "image_png.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua_api_gen.h"

extern cvar_t v_flipped;

static cvar_t lua_hud_panels = {CF_CLIENT | CF_READONLY, "_lua_hud_panels", "", "HUD panels and touch buttons of Lua mods, JSON: [{\"mod\",\"id\",\"kind\",\"title\",\"x\",\"y\",\"w\",\"h\",\"xcvar\",\"ycvar\"}] (for the HUD editor)"};

void TLua_RegisterCvar(cvar_t *var);

// ---------------------------------------------------------------------------
// Whose client game code runs

static qbool tlua_cl_hello;      // this game's CSQC answered since it loaded
static unsigned tlua_cl_hello_signon;

void TLua_CL_CsqcHello(void)
{
	tlua_cl_hello = true;
}

int TLua_CL_CsqcKind(void)
{
	if (cls.state != ca_connected || !CLVM_prog->loaded)
		return 0;
	return tlua_cl_hello ? 2 : 1;
}

int TLuaAPI_cl_csqc(lua_State *L)
{
	int k = TLua_CL_CsqcKind();
	lua_pushstring(L, k == 2 ? "godnotic" : k == 1 ? "foreign" : "none");
	return 1;
}

int TLuaAPI_cl_offline(lua_State *L)
{
	const char *why;
	lua_pushboolean(L, TLua_Offline(&why));
	lua_pushstring(L, why);
	return 2;
}

// ---------------------------------------------------------------------------
// Drawing

static void tlua_needdraw(lua_State *L)
{
	if (!tlua_cl_drawing)
		luaL_error(L, "xt.draw works only inside drawing events (\"hud\", \"draw\", \"hud_overlay\", \"scoreboard\") and HUD panels");
}

static float tlua_c(lua_State *L, int idx)
{
	return (float)bound(0, luaL_optnumber(L, idx, 1), 1);
}

static void tlua_rgba(lua_State *L, int idx, float *c)
{
	int i;
	if (lua_type(L, idx) == LUA_TTABLE)
	{
		for (i = 0; i < 4; i++)
		{
			lua_rawgeti(L, idx, i + 1);
			c[i] = (float)bound(0, luaL_optnumber(L, -1, 1), 1);
			lua_pop(L, 1);
		}
	}
	else
		c[0] = c[1] = c[2] = c[3] = 1;
}

int TLuaAPI_cl_draw_line(lua_State *L)
{
	tlua_needdraw(L);
	DrawQ_Line((float)bound(0.5, luaL_optnumber(L, 5, 1), 64), (float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
		(float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4), tlua_c(L, 6), tlua_c(L, 7), tlua_c(L, 8), tlua_c(L, 9), 0);
	return 0;
}

int TLuaAPI_cl_draw_rect(lua_State *L)
{
	float x = (float)luaL_checknumber(L, 1), y = (float)luaL_checknumber(L, 2), w = (float)luaL_checknumber(L, 3), h = (float)luaL_checknumber(L, 4);
	float t = (float)bound(0.5, luaL_optnumber(L, 5, 1), 64), r = tlua_c(L, 6), g = tlua_c(L, 7), b = tlua_c(L, 8), a = tlua_c(L, 9);
	tlua_needdraw(L);
	DrawQ_Fill(x, y, w, t, r, g, b, a, 0);
	DrawQ_Fill(x, y + h - t, w, t, r, g, b, a, 0);
	DrawQ_Fill(x, y + t, t, h - 2 * t, r, g, b, a, 0);
	DrawQ_Fill(x + w - t, y + t, t, h - 2 * t, r, g, b, a, 0);
	return 0;
}

int TLuaAPI_cl_draw_gradient(lua_State *L)
{
	float x = (float)luaL_checknumber(L, 1), y = (float)luaL_checknumber(L, 2), w = (float)luaL_checknumber(L, 3), h = (float)luaL_checknumber(L, 4);
	float c1[4], c2[4];
	qbool vertical = lua_toboolean(L, 7);
	const float *tr, *br, *bl;
	tlua_needdraw(L);
	tlua_rgba(L, 5, c1);
	tlua_rgba(L, 6, c2);
	// corners: top-left c1; vertical: bottom ones c2; horizontal: right ones c2
	tr = vertical ? c1 : c2;
	bl = vertical ? c2 : c1;
	br = c2;
	DrawQ_SuperPic(x, y, NULL, w, h,
		0, 0, c1[0], c1[1], c1[2], c1[3],
		1, 0, tr[0], tr[1], tr[2], tr[3],
		0, 1, bl[0], bl[1], bl[2], bl[3],
		1, 1, br[0], br[1], br[2], br[3], 0);
	return 0;
}

int TLuaAPI_cl_draw_circle(lua_State *L)
{
	float cx = (float)luaL_checknumber(L, 1), cy = (float)luaL_checknumber(L, 2), rad = (float)luaL_checknumber(L, 3);
	float r = tlua_c(L, 4), g = tlua_c(L, 5), b = tlua_c(L, 6), a = tlua_c(L, 7);
	qbool filled = lua_toboolean(L, 8);
	int segs = (int)bound(8, luaL_optinteger(L, 9, 32), 64), i;
	tlua_needdraw(L);
	if (!(rad > 0) || rad > 4096)
		return 0;
	for (i = 0; i < segs; i++)
	{
		double a0 = 2 * M_PI * i / segs, a1 = 2 * M_PI * (i + 1) / segs;
		float x0 = cx + rad * (float)cos(a0), y0 = cy + rad * (float)sin(a0);
		float x1 = cx + rad * (float)cos(a1), y1 = cy + rad * (float)sin(a1);
		if (filled)
		{
			// a fan of thin quads: each slice as a 4-corner picture with two corners at the centre
			DrawQ_SuperPic(0, 0, NULL, 1, 1, 0, 0, r, g, b, a, 0, 0, r, g, b, a, 0, 0, r, g, b, a, 0, 0, r, g, b, a, 0);
			{
				model_t *mod = CL_Mesh_UI();
				msurface_t *surf = Mod_Mesh_AddSurface(mod, Mod_Mesh_GetTexture(mod, "white", 0, 0, MATERIALFLAG_WALL | MATERIALFLAG_VERTEXCOLOR | MATERIALFLAG_ALPHAGEN_VERTEX | MATERIALFLAG_ALPHA | MATERIALFLAG_BLENDED | MATERIALFLAG_NOSHADOW), true);
				int e0 = Mod_Mesh_IndexForVertex(mod, surf, cx, cy, 10, 0, 0, -1, 0, 0, 0, 0, r, g, b, a);
				int e1 = Mod_Mesh_IndexForVertex(mod, surf, x0, y0, 10, 0, 0, -1, 0, 0, 0, 0, r, g, b, a);
				int e2 = Mod_Mesh_IndexForVertex(mod, surf, x1, y1, 10, 0, 0, -1, 0, 0, 0, 0, r, g, b, a);
				Mod_Mesh_AddTriangle(mod, surf, e0, e1, e2);
			}
		}
		else
			DrawQ_Line(1.5f, x0, y0, x1, y1, r, g, b, a, 0);
	}
	return 0;
}

int TLuaAPI_cl_draw_textsize(lua_State *L)
{
	size_t len;
	const char *s = luaL_checklstring(L, 1, &len);
	float size = (float)bound(1, luaL_optnumber(L, 2, 12), 256);
	if (len > 1024)
		len = 1024;
	lua_pushnumber(L, DrawQ_TextWidth(s, len, size, size, false, FONT_DEFAULT));
	lua_pushnumber(L, size);
	return 2;
}

// world point -> 2D screen
static qbool tlua_project(const vec3_t p, float *sx, float *sy, float *depth)
{
	matrix4x4_t m;
	vec3_t v;
	Matrix4x4_Invert_Full(&m, &r_refdef.view.matrix);
	Matrix4x4_Transform(&m, p, v);
	if (v_flipped.integer)
		v[1] = -v[1];
	*depth = v[0];
	if (v[0] <= 0.01f || !r_refdef.view.frustum_x || !r_refdef.view.frustum_y)
	{
		*sx = *sy = -1;
		return false;
	}
	*sx = vid_conwidth.value * (0.5f * (1.0f + v[1] / v[0] / -r_refdef.view.frustum_x));
	*sy = vid_conheight.value * (0.5f * (1.0f + v[2] / v[0] / -r_refdef.view.frustum_y));
	return *sx >= 0 && *sy >= 0 && *sx <= vid_conwidth.value && *sy <= vid_conheight.value;
}

static void tlua_checkvec(lua_State *L, int idx, vec3_t out)
{
	static const char *const k[3] = {"x", "y", "z"};
	int i;
	luaL_checktype(L, idx, LUA_TTABLE);
	for (i = 0; i < 3; i++)
	{
		if (lua_getfield(L, idx, k[i]) != LUA_TNUMBER)
		{
			lua_pop(L, 1);
			lua_rawgeti(L, idx, i + 1);
		}
		out[i] = (float)luaL_optnumber(L, -1, 0);
		lua_pop(L, 1);
	}
}

static void tlua_pushvec(lua_State *L, const vec3_t v)
{
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, v[0]);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, v[1]);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, v[2]);
	lua_setfield(L, -2, "z");
}

int TLuaAPI_cl_draw_project(lua_State *L)
{
	vec3_t p;
	float x, y, d;
	qbool on;
	tlua_checkvec(L, 1, p);
	on = tlua_project(p, &x, &y, &d);
	lua_pushnumber(L, x);
	lua_pushnumber(L, y);
	lua_pushboolean(L, on);
	return 3;
}

int TLuaAPI_cl_view(lua_State *L)
{
	vec3_t org, fwd, left, up, ang;
	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, org);
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, org);
	AnglesFromVectors(ang, fwd, up, false);
	lua_createtable(L, 0, 7);
	tlua_pushvec(L, org);
	lua_setfield(L, -2, "origin");
	tlua_pushvec(L, ang);
	lua_setfield(L, -2, "angles");
	tlua_pushvec(L, fwd);
	lua_setfield(L, -2, "forward");
	lua_pushnumber(L, r_refdef.view.frustum_x ? 2 * atan(r_refdef.view.frustum_x) * 180 / M_PI : 0);
	lua_setfield(L, -2, "fov_x");
	lua_pushnumber(L, r_refdef.view.frustum_y ? 2 * atan(r_refdef.view.frustum_y) * 180 / M_PI : 0);
	lua_setfield(L, -2, "fov_y");
	lua_pushnumber(L, vid_conwidth.value);
	lua_setfield(L, -2, "w");
	lua_pushnumber(L, vid_conheight.value);
	lua_setfield(L, -2, "h");
	return 1;
}

// ---------------------------------------------------------------------------
// Images from the mod's own package (xt.draw.image)

#define TLUA_MAX_IMAGES 128
#define TLUA_IMAGES_PER_MOD 32
#define TLUA_IMAGE_PIXELS_PER_MOD (1024 * 1024)
typedef struct tlua_image_s
{
	char mod[TLUA_NAME_LEN + 1];
	char file[64];
	char picname[128];
	int w, h;
	qbool failed;
	cachepic_t *pic;
} tlua_image_t;
static tlua_image_t tlua_images[TLUA_MAX_IMAGES];

static tlua_image_t *tlua_image_get(lua_State *L, tlua_mod_t *m, const char *file)
{
	int i, mine = 0, free_i = -1;
	long pixels = 0;
	const char *p, *src;
	size_t len, n = strlen(file);
	unsigned char *bgra = NULL;
	int mip = 0;
	tlua_image_t *im;
	for (i = 0; i < TLUA_MAX_IMAGES; i++)
	{
		im = &tlua_images[i];
		if (!im->mod[0])
		{
			if (free_i < 0)
				free_i = i;
			continue;
		}
		if (strcmp(im->mod, m->name))
			continue;
		if (!strcmp(im->file, file))
			return im->failed ? NULL : im;
		mine++;
		pixels += (long)im->w * im->h;
	}
	if (n < 5 || n >= sizeof(im->file) || strstr(file, "..") || file[0] == '/' || (strcasecmp(file + n - 4, ".png") && strcasecmp(file + n - 4, ".tga")))
		luaL_error(L, "xt.draw.image: a .png or .tga path inside the mod (\"img/logo.png\")");
	for (p = file; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("/_-.", *p))
			luaL_error(L, "xt.draw.image: bad character in the path");
	if (mine >= TLUA_IMAGES_PER_MOD || free_i < 0)
		return NULL;
	im = &tlua_images[free_i];
	memset(im, 0, sizeof(*im));
	dp_strlcpy(im->mod, m->name, sizeof(im->mod));
	dp_strlcpy(im->file, file, sizeof(im->file));
	im->failed = true;
	src = TLua_ReadModFile(L, m, file, &len);
	if (src && len > 0)
	{
		bgra = !strcasecmp(file + n - 4, ".png") ? PNG_LoadImage_BGRA((const unsigned char *)src, (int)len, &mip) : LoadTGA_BGRA((const unsigned char *)src, (int)len, &mip);
		if (bgra && image_width > 0 && image_height > 0 && image_width <= 1024 && image_height <= 1024 && pixels + (long)image_width * image_height <= TLUA_IMAGE_PIXELS_PER_MOD)
		{
			im->w = image_width;
			im->h = image_height;
			dpsnprintf(im->picname, sizeof(im->picname), "luamod/%s/%s", m->name, file);
			im->pic = Draw_NewPic(im->picname, im->w, im->h, bgra, TEXTYPE_BGRA, TEXF_ALPHA | TEXF_CLAMP);
			im->failed = im->pic == NULL;
		}
		if (bgra)
			Mem_Free(bgra);
	}
	return im->failed ? NULL : im;
}

int TLuaAPI_cl_draw_image(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	tlua_image_t *im;
	tlua_needdraw(L);
	im = tlua_image_get(L, m, luaL_checkstring(L, 1));
	if (!im)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	DrawQ_Pic((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), im->pic, (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5),
		tlua_c(L, 6), tlua_c(L, 7), tlua_c(L, 8), tlua_c(L, 9), 0);
	lua_pushboolean(L, 1);
	return 1;
}

static void tlua_images_free(tlua_mod_t *m)
{
	int i;
	for (i = 0; i < TLUA_MAX_IMAGES; i++)
		if (tlua_images[i].mod[0] && !strcmp(tlua_images[i].mod, m->name))
		{
			if (tlua_images[i].pic)
				Draw_FreePic(tlua_images[i].picname);
			memset(&tlua_images[i], 0, sizeof(tlua_images[i]));
		}
}

// ---------------------------------------------------------------------------
// HUD panels and touch buttons the engine places

#define TLUA_MAX_WIDGETS 64
#define TLUA_WIDGETS_PER_MOD 8
typedef struct tlua_widget_s
{
	qbool used, button;
	char mod[TLUA_NAME_LEN + 1];
	char id[TLUA_SUFFIX_LEN - 1];
	char title[64], label[32], icon[64];
	float w, h;
	char side[8];
	int draw, press, hold, release; // registry refs in the mod's state
	int finger;                     // the finger holding a button (-1: none; 10: the mouse)
	float x, y;                     // where it is this frame
	cvar_t *xcv, *ycv;
} tlua_widget_t;
static tlua_widget_t tlua_widgets[TLUA_MAX_WIDGETS];
static qbool tlua_widgets_dirty;
static qbool tlua_cl_userinput; // inside a key, bind or button callback (xt.clipboard)

static tlua_widget_t *tlua_widget_find(tlua_mod_t *m, const char *id)
{
	int i;
	for (i = 0; i < TLUA_MAX_WIDGETS; i++)
		if (tlua_widgets[i].used && !strcmp(tlua_widgets[i].mod, m->name) && !strcmp(tlua_widgets[i].id, id))
			return &tlua_widgets[i];
	return NULL;
}

static void tlua_widget_free(tlua_mod_t *m, tlua_widget_t *w)
{
	TLua_Unref(m, w->draw);
	TLua_Unref(m, w->press);
	TLua_Unref(m, w->hold);
	TLua_Unref(m, w->release);
	w->used = false;
	tlua_widgets_dirty = true;
}

static int tlua_optref(lua_State *L, int t, const char *k)
{
	int ref = 0;
	if (lua_getfield(L, t, k) == LUA_TFUNCTION)
		ref = TLua_Ref(L, -1);
	else if (!lua_isnil(L, -1))
		luaL_error(L, "%s must be a function", k);
	lua_pop(L, 1);
	return ref;
}

static const char *tlua_optstr(lua_State *L, int t, const char *k, const char *def, char *out, size_t size)
{
	if (lua_getfield(L, t, k) == LUA_TSTRING)
		TLua_CleanText(lua_tostring(L, -1), out, size, false);
	else
		dp_strlcpy(out, def, size);
	lua_pop(L, 1);
	return out;
}

static float tlua_optnum(lua_State *L, int t, const char *k, float def)
{
	float v = def;
	if (lua_getfield(L, t, k) == LUA_TNUMBER)
		v = (float)lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

// a widget's place: the cvars lua_<mod>_<id>x / y (the player may move it,
// in 2D units), created at a free spot the first time, below the earlier ones
static void tlua_widget_place(tlua_mod_t *m, tlua_widget_t *w)
{
	char xn[96], yn[96], def[32], desc[128];
	float fx, fy;
	int i, stack = 0;
	float pw = w->w, ph = w->h, W = vid_conwidth.value, H = vid_conheight.value;
	for (i = 0; i < TLUA_MAX_WIDGETS; i++)
		if (tlua_widgets[i].used && &tlua_widgets[i] != w && !strcmp(tlua_widgets[i].side, w->side))
			stack++;
	fy = 0.18f * H + stack * (ph + 0.01f * H);
	if (fy + ph > 0.55f * H)
		fy = 0.17f * H;
	fx = !strcmp(w->side, "right") ? W - pw - 0.01f * W : 0.01f * W;
	dpsnprintf(xn, sizeof(xn), "%s%s_%sx", m->realm->cmdprefix, m->name, w->id);
	dpsnprintf(yn, sizeof(yn), "%s%s_%sy", m->realm->cmdprefix, m->name, w->id);
	dpsnprintf(desc, sizeof(desc), "position of Lua mod %s's %s %s (2D units; -1: automatic)", m->name, w->button ? "button" : "panel", w->id);
	w->xcv = Cvar_FindVar(&cvars_all, xn, ~0);
	if (!w->xcv && !Cmd_Exists(cmd_local, xn))
		w->xcv = Cvar_Get(&cvars_all, xn, "-1", CF_CLIENT | CF_ARCHIVE, desc);
	w->ycv = Cvar_FindVar(&cvars_all, yn, ~0);
	if (!w->ycv && !Cmd_Exists(cmd_local, yn))
		w->ycv = Cvar_Get(&cvars_all, yn, "-1", CF_CLIENT | CF_ARCHIVE, desc);
	dpsnprintf(def, sizeof(def), "%g", fx);
	w->x = fx;
	w->y = fy;
	(void)def;
}

static void tlua_widget_pos(tlua_widget_t *w)
{
	float W = vid_conwidth.value, H = vid_conheight.value;
	float x = w->xcv && w->xcv->value >= 0 ? w->xcv->value : -1;
	float y = w->ycv && w->ycv->value >= 0 ? w->ycv->value : -1;
	if (x >= 0)
		w->x = bound(0, x, W - w->w);
	if (y >= 0)
		w->y = bound(0, y, H - w->h);
}

static int tlua_widget_new(lua_State *L, qbool button)
{
	tlua_mod_t *m = TLua_Self(L);
	tlua_widget_t *w;
	char id[64];
	int i, mine = 0;
	const char *p;
	luaL_checktype(L, 1, LUA_TTABLE);
	tlua_optstr(L, 1, "id", "", id, sizeof(id));
	if (!id[0] || strlen(id) > TLUA_SUFFIX_LEN - 2)
		return luaL_error(L, "%s: id is a-z and 0-9, up to %d", button ? "xt.touch.button" : "xt.hud.panel", TLUA_SUFFIX_LEN - 2);
	for (p = id; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
			return luaL_error(L, "%s: id is a-z and 0-9", button ? "xt.touch.button" : "xt.hud.panel");
	if ((w = tlua_widget_find(m, id)))
		tlua_widget_free(m, w);
	for (i = 0, w = NULL; i < TLUA_MAX_WIDGETS; i++)
	{
		if (tlua_widgets[i].used && !strcmp(tlua_widgets[i].mod, m->name))
			mine++;
		else if (!tlua_widgets[i].used && !w)
			w = &tlua_widgets[i];
	}
	if (mine >= TLUA_WIDGETS_PER_MOD * 2 || !w)
		return luaL_error(L, "too many panels and buttons (%d of each per mod)", TLUA_WIDGETS_PER_MOD);
	memset(w, 0, sizeof(*w));
	w->button = button;
	w->finger = -1;
	dp_strlcpy(w->mod, m->name, sizeof(w->mod));
	dp_strlcpy(w->id, id, sizeof(w->id));
	tlua_optstr(L, 1, "title", id, w->title, sizeof(w->title));
	tlua_optstr(L, 1, "label", "", w->label, sizeof(w->label));
	tlua_optstr(L, 1, "icon", "", w->icon, sizeof(w->icon));
	tlua_optstr(L, 1, "side", button ? "right" : "left", w->side, sizeof(w->side));
	if (strcmp(w->side, "right"))
		dp_strlcpy(w->side, "left", sizeof(w->side));
	w->w = bound(button ? 48 : 16, tlua_optnum(L, 1, "w", button ? 72 : 160), 1024);
	w->h = bound(button ? 48 : 16, tlua_optnum(L, 1, "h", button ? 72 : 60), 1024);
	if (button)
	{
		w->press = tlua_optref(L, 1, "press");
		w->hold = tlua_optref(L, 1, "hold");
		w->release = tlua_optref(L, 1, "release");
		w->draw = tlua_optref(L, 1, "draw");
	}
	else
	{
		w->draw = tlua_optref(L, 1, "draw");
		if (!w->draw)
			return luaL_error(L, "xt.hud.panel: draw = function(x, y, w, h) is required");
	}
	w->used = true;
	tlua_widget_place(m, w);
	tlua_widget_pos(w);
	tlua_widgets_dirty = true;
	// the returned object: the same table, with the id
	lua_pushvalue(L, 1);
	return 1;
}

int TLuaAPI_cl_hud_panel(lua_State *L) { return tlua_widget_new(L, false); }
int TLuaAPI_cl_touch_button(lua_State *L) { return tlua_widget_new(L, true); }

int TLuaAPI_cl_touch_remove(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	tlua_widget_t *w = tlua_widget_find(m, luaL_checkstring(L, 1));
	if (w)
		tlua_widget_free(m, w);
	lua_pushboolean(L, w != NULL);
	return 1;
}

typedef struct tlua_wcall_s { tlua_widget_t *w; int phase; } tlua_wcall_t;
static int tlua_widget_push(lua_State *L, void *ud)
{
	tlua_widget_t *w = ((tlua_wcall_t *)ud)->w;
	lua_pushnumber(L, w->x);
	lua_pushnumber(L, w->y);
	lua_pushnumber(L, w->w);
	lua_pushnumber(L, w->h);
	return 4;
}

static void tlua_widgets_publish(void)
{
	static char buf[8192];
	char line[512];
	int i;
	qbool first = true;
	void TLua_JsonAppendString(char *out, size_t size, const char *s);
	tlua_widgets_dirty = false;
	dp_strlcpy(buf, "[", sizeof(buf));
	for (i = 0; i < TLUA_MAX_WIDGETS; i++)
	{
		tlua_widget_t *w = &tlua_widgets[i];
		if (!w->used)
			continue;
		if (!first)
			dp_strlcat(buf, ",", sizeof(buf));
		first = false;
		dp_strlcat(buf, "{\"mod\":", sizeof(buf));
		TLua_JsonAppendString(buf, sizeof(buf), w->mod);
		dp_strlcat(buf, ",\"id\":", sizeof(buf));
		TLua_JsonAppendString(buf, sizeof(buf), w->id);
		dp_strlcat(buf, ",\"title\":", sizeof(buf));
		TLua_JsonAppendString(buf, sizeof(buf), w->title);
		dpsnprintf(line, sizeof(line), ",\"kind\":\"%s\",\"x\":%g,\"y\":%g,\"w\":%g,\"h\":%g,\"xcvar\":\"%s\",\"ycvar\":\"%s\"}",
			w->button ? "button" : "panel", w->x, w->y, w->w, w->h, w->xcv ? w->xcv->name : "", w->ycv ? w->ycv->name : "");
		dp_strlcat(buf, line, sizeof(buf));
	}
	dp_strlcat(buf, "]", sizeof(buf));
	if (strcmp(buf, lua_hud_panels.string))
		Cvar_SetQuick(&lua_hud_panels, buf);
}

// the "hud" pass, after the mods' own hud handlers: panels, then buttons
void TLua_CL_DrawWidgets(void);
void TLua_CL_DrawWidgets(void)
{
	int i;
	for (i = 0; i < TLUA_MAX_WIDGETS; i++)
	{
		tlua_widget_t *w = &tlua_widgets[i];
		tlua_mod_t *m;
		tlua_wcall_t c;
		if (!w->used)
			continue;
		m = TLua_Find(&tlua_client, w->mod);
		if (!m || m->state != TLUA_RUNNING)
			continue;
		tlua_widget_pos(w);
		c.w = w;
		c.phase = 0;
		if (w->button)
		{
			float a = w->finger >= 0 ? 0.85f : 0.55f;
			DrawQ_Fill(w->x, w->y, w->w, w->h, 0.08f, 0.10f, 0.14f, a, 0);
			DrawQ_Fill(w->x, w->y + w->h - 3, w->w, 3, 0.20f, 0.55f, 0.95f, w->finger >= 0 ? 1 : 0.6f, 0);
			if (w->icon[0])
			{
				tlua_image_t *im = NULL;
				int k;
				for (k = 0; k < TLUA_MAX_IMAGES; k++)
					if (tlua_images[k].mod[0] && !strcmp(tlua_images[k].mod, m->name) && !strcmp(tlua_images[k].file, w->icon) && !tlua_images[k].failed)
						im = &tlua_images[k];
				if (im)
					DrawQ_Pic(w->x + w->w * 0.2f, w->y + w->h * 0.12f, im->pic, w->w * 0.6f, w->h * 0.6f, 1, 1, 1, 1, 0);
			}
			if (w->label[0])
			{
				float size = bound(8, w->h * 0.24f, 32);
				float tw = DrawQ_TextWidth(w->label, 0, size, size, false, FONT_DEFAULT);
				DrawQ_String(w->x + (w->w - tw) / 2, w->y + (w->icon[0] ? w->h * 0.72f : (w->h - size) / 2), w->label, 0, size, size, 1, 1, 1, 1, 0, NULL, false, FONT_DEFAULT);
			}
			if (!w->draw)
				continue;
		}
		tlua_cl_drawing = true;
		DrawQ_SetClipArea(w->x, w->y, w->w, w->h);
		TLua_CallRef(m, w->draw, tlua_widget_push, &c, NULL);
		DrawQ_ResetClipArea();
		tlua_cl_drawing = false;
	}
	if (tlua_widgets_dirty)
		tlua_widgets_publish();
}

// fingers, with lua_mod_simulate's stand-ins (for testing a mod without a touchscreen)
static int tlua_sim_finger[10];
static float tlua_sim_fx[10], tlua_sim_fy[10];
static void tlua_finger(int slot, int *act, float *x, float *y)
{
	float nx, ny;
	if (tlua_sim_finger[slot])
	{
		*act = 1;
		*x = tlua_sim_fx[slot];
		*y = tlua_sim_fy[slot];
		return;
	}
	VID_GetTouchFinger(slot, act, &nx, &ny);
	*x = nx * vid_conwidth.value;
	*y = ny * vid_conheight.value;
}

// touch buttons: a finger (or the mouse with MOUSE1) landing on one presses
// it, hold runs every frame while it stays down, release when it lifts
static qbool tlua_mouse1;
static void tlua_widgets_input(void)
{
	int i, slot;
	float fx[11], fy[11];
	int act[11];
	for (slot = 0; slot < 10; slot++)
		tlua_finger(slot, &act[slot], &fx[slot], &fy[slot]);
	act[10] = tlua_mouse1 && key_dest == key_game;
	fx[10] = vid.mode.width ? in_windowmouse_x * vid_conwidth.value / vid.mode.width : 0;
	fy[10] = vid.mode.height ? in_windowmouse_y * vid_conheight.value / vid.mode.height : 0;
	for (i = 0; i < TLUA_MAX_WIDGETS; i++)
	{
		tlua_widget_t *w = &tlua_widgets[i];
		tlua_mod_t *m;
		tlua_wcall_t c;
		if (!w->used || !w->button)
			continue;
		m = TLua_Find(&tlua_client, w->mod);
		if (!m || m->state != TLUA_RUNNING)
			continue;
		c.w = w;
		tlua_cl_userinput = true;
		if (w->finger >= 0)
		{
			if (!act[w->finger])
			{
				w->finger = -1;
				if (w->release)
					TLua_CallRef(m, w->release, tlua_widget_push, &c, NULL);
			}
			else if (w->hold)
				TLua_CallRef(m, w->hold, tlua_widget_push, &c, NULL);
		}
		else
			for (slot = 0; slot < 11; slot++)
				if (act[slot] && fx[slot] >= w->x && fx[slot] < w->x + w->w && fy[slot] >= w->y && fy[slot] < w->y + w->h)
				{
					int j;
					qbool taken = false;
					for (j = 0; j < TLUA_MAX_WIDGETS; j++)
						if (tlua_widgets[j].used && tlua_widgets[j].finger == slot)
							taken = true;
					if (taken)
						continue;
					w->finger = slot;
					if (w->press)
						TLua_CallRef(m, w->press, tlua_widget_push, &c, NULL);
					break;
				}
		tlua_cl_userinput = false;
	}
}

// ---------------------------------------------------------------------------
// Binds: keys a mod takes from their binding while it runs

#define TLUA_MAX_BINDS 64
typedef struct tlua_bind_s { qbool used; char mod[TLUA_NAME_LEN + 1]; int key, down, up; } tlua_bind_t;
static tlua_bind_t tlua_binds[TLUA_MAX_BINDS];

int TLuaAPI_cl_bind(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	int key = Key_StringToKeynum(name), i, mine = 0;
	tlua_bind_t *b = NULL;
	const char *bind;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (key < 0 || key >= MAX_KEYS || key == K_ESCAPE)
		return luaL_error(L, "xt.bind: no key '%s' (or Escape)", name);
	bind = Key_GetBind(key, -1);
	if (bind && (strstr(bind, "toggleconsole") || strstr(bind, "togglemenu")))
		return luaL_error(L, "xt.bind: %s opens the console or the menu; it stays theirs", name);
	for (i = 0; i < TLUA_MAX_BINDS; i++)
	{
		tlua_bind_t *x = &tlua_binds[i];
		if (x->used && !strcmp(x->mod, m->name))
		{
			if (x->key == key)
			{
				TLua_Unref(m, x->down);
				TLua_Unref(m, x->up);
				x->used = false;
			}
			else
				mine++;
		}
		if (!x->used && !b)
			b = x;
	}
	if (mine >= 16 || !b)
		return luaL_error(L, "xt.bind: too many keys (16 per mod)");
	b->used = true;
	dp_strlcpy(b->mod, m->name, sizeof(b->mod));
	b->key = key;
	b->down = TLua_Ref(L, 2);
	b->up = lua_type(L, 3) == LUA_TFUNCTION ? TLua_Ref(L, 3) : 0;
	return 0;
}

int TLuaAPI_cl_unbind(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	int key = Key_StringToKeynum(luaL_checkstring(L, 1)), i;
	for (i = 0; i < TLUA_MAX_BINDS; i++)
		if (tlua_binds[i].used && tlua_binds[i].key == key && !strcmp(tlua_binds[i].mod, m->name))
		{
			TLua_Unref(m, tlua_binds[i].down);
			TLua_Unref(m, tlua_binds[i].up);
			tlua_binds[i].used = false;
		}
	return 0;
}

static int tlua_bind_push(lua_State *L, void *ud)
{
	char name[64];
	lua_pushstring(L, Key_KeynumToString(*(int *)ud, name, sizeof(name)));
	return 1;
}

// keys.c -> TouchLua_KeyEvent -> here first: true when a mod's bind took it
qbool TLua_CL_BindKey(int key, qbool down);
qbool TLua_CL_BindKey(int key, qbool down)
{
	int i;
	qbool took = false;
	if (key == K_MOUSE1)
		tlua_mouse1 = down;
	for (i = 0; i < TLUA_MAX_BINDS; i++)
	{
		tlua_bind_t *b = &tlua_binds[i];
		tlua_mod_t *m;
		if (!b->used || b->key != key)
			continue;
		m = TLua_Find(&tlua_client, b->mod);
		if (!m || m->state != TLUA_RUNNING)
			continue;
		took = true;
		tlua_cl_userinput = true;
		if (down)
			TLua_CallRef(m, b->down, tlua_bind_push, &key, NULL);
		else if (b->up)
			TLua_CallRef(m, b->up, tlua_bind_push, &key, NULL);
		tlua_cl_userinput = false;
	}
	return took;
}

void TLua_CL_SetUserInput(qbool on);
void TLua_CL_SetUserInput(qbool on) { tlua_cl_userinput = on; }

// ---------------------------------------------------------------------------
// Effects, the clipboard, vibration

int TLuaAPI_cl_effect(lua_State *L)
{
	static double window;
	static int count;
	const char *name = luaL_checkstring(L, 1);
	vec3_t org;
	int idx, n = (int)bound(1, luaL_optinteger(L, 3, 1), 16);
	tlua_checkvec(L, 2, org);
	if (cls.state != ca_connected || !cl.worldmodel)
		return 0;
	if (host.realtime - window > 1)
	{
		window = host.realtime;
		count = 0;
	}
	if ((count += n) > 32)
		return 0;
	idx = CL_ParticleEffectIndexForName(name);
	if (idx <= 0)
		return 0;
	CL_ParticleEffect(idx, n, org, org, vec3_origin, vec3_origin, NULL, 0);
	lua_pushboolean(L, 1);
	return 1;
}

// SDL's, when this binary links SDL (not the dedicated server, not the web)
#if defined(__GNUC__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__APPLE__)
extern int SDL_SetClipboardText(const char *text) __attribute__((weak));
static qbool tlua_setclipboard(const char *s) { return SDL_SetClipboardText && SDL_SetClipboardText(s) == 0; }
#else
static qbool tlua_setclipboard(const char *s) { (void)s; return false; }
#endif

int TLuaAPI_cl_clipboard(lua_State *L)
{
	size_t len;
	const char *s = luaL_checklstring(L, 1, &len);
	if (!tlua_cl_userinput)
		return luaL_error(L, "xt.clipboard: only inside a key, bind or touch button callback");
	if (len > 4096)
		return luaL_error(L, "xt.clipboard: over 4 KB");
	lua_pushboolean(L, tlua_setclipboard(s));
	return 1;
}

int TLuaAPI_cl_vibrate(lua_State *L)
{
	static double window;
	static int count;
	int ms = (int)bound(10, luaL_checkinteger(L, 1), 1000);
	if (!TouchLua_VibrateHook)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	if (host.realtime - window > 1)
	{
		window = host.realtime;
		count = 0;
	}
	if (++count > 2)
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	lua_pushboolean(L, TouchLua_VibrateHook(ms));
	return 1;
}

// ---------------------------------------------------------------------------
// Entities around the player (this game's client code; the offline-only rule)

static qbool tlua_visible(const vec3_t from, const vec3_t to)
{
	trace_t tr = CL_TraceLine(from, to, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0,
		collision_extendmovelength.value, true, false, NULL, true, false);
	return !tr.startsolid && tr.fraction >= 0.999f;
}

// the players of the server this process runs (offline only: the caller checks)
static int tlua_server_players(lua_State *L, const vec3_t eye, int n)
{
	prvm_prog_t *prog = SVVM_prog;
	int i;
	if (!prog->loaded)
		return n;
	for (i = 0; i < svs.maxclients && n < 256; i++)
	{
		client_t *c = svs.clients + i;
		prvm_edict_t *ed;
		float *org, sx, sy, depth;
		vec3_t mid;
		char clean[MAX_SCOREBOARDNAME];
		if (!c->active || !c->edict || i + 1 == cl.playerentity)
			continue;
		ed = c->edict;
		if (PRVM_serveredictfloat(ed, health) <= 0 || PRVM_serveredictfloat(ed, frags) == -666)
			continue;
		org = PRVM_serveredictvector(ed, origin);
		VectorCopy(org, mid);
		mid[2] += 16;
		lua_createtable(L, 0, 8);
		lua_pushinteger(L, i + 1);
		lua_setfield(L, -2, "id");
		lua_pushstring(L, "player");
		lua_setfield(L, -2, "kind");
		lua_pushnumber(L, org[0]);
		lua_setfield(L, -2, "x");
		lua_pushnumber(L, org[1]);
		lua_setfield(L, -2, "y");
		lua_pushnumber(L, org[2]);
		lua_setfield(L, -2, "z");
		lua_pushboolean(L, tlua_project(mid, &sx, &sy, &depth) && tlua_visible(eye, mid));
		lua_setfield(L, -2, "visible");
		lua_pushinteger(L, (lua_Integer)PRVM_serveredictfloat(ed, team));
		lua_setfield(L, -2, "team");
		lua_pushstring(L, TLua_CleanText(c->name, clean, sizeof(clean), false));
		lua_setfield(L, -2, "name");
		lua_rawseti(L, -2, ++n);
	}
	return n;
}

int TLuaAPI_cl_entities(lua_State *L)
{
	prvm_prog_t *prog = CLVM_prog;
	const char *want = luaL_optstring(L, 1, NULL);
	static int f_isplayer = -2, f_entnum, f_team, f_draw;
	static qbool i_isplayer, i_entnum, i_team;
	static int fn_proj, fn_item;
	static unsigned seenprog;
	int i, n = 0;
	qbool offline = TLua_Offline(NULL);
	vec3_t eye;
	lua_newtable(L);
	if (TLua_CL_CsqcKind() != 2)
		return 1;
	if (seenprog != (unsigned)(size_t)prog->functions)
	{
		// field and function offsets of this load of the client program
		mfunction_t *f;
		seenprog = (unsigned)(size_t)prog->functions;
		f_isplayer = PRVM_ED_FindFieldOffset(prog, "isplayermodel");
		f_entnum = PRVM_ED_FindFieldOffset(prog, "entnum");
		f_team = PRVM_ED_FindFieldOffset(prog, "team");
		// gmqcc's int fields hold integers, not floats
		{
			mdef_t *d;
			i_isplayer = (d = PRVM_ED_FindField(prog, "isplayermodel")) && (d->type & ~DEF_SAVEGLOBAL) != ev_float;
			i_entnum = (d = PRVM_ED_FindField(prog, "entnum")) && (d->type & ~DEF_SAVEGLOBAL) != ev_float;
			i_team = (d = PRVM_ED_FindField(prog, "team")) && (d->type & ~DEF_SAVEGLOBAL) != ev_float;
		}
		f_draw = PRVM_ED_FindFieldOffset(prog, "draw");
		f = PRVM_ED_FindFunction(prog, "Projectile_Draw");
		fn_proj = f ? (int)(f - prog->functions) : -1;
		f = PRVM_ED_FindFunction(prog, "ItemDraw");
		fn_item = f ? (int)(f - prog->functions) : -1;
	}
	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	// Offline, this process runs the server: the players come from it, so a
	// radar sees bots the network would not send yet (outside the view)
	if (offline && sv.active && (!want || !strcmp(want, "player")))
	{
		n = tlua_server_players(L, eye, n);
		if (want)
			return 1;
	}
	for (i = 1; i < prog->num_edicts && n < 256; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		const char *kind = NULL;
		float *org;
		vec3_t mid;
		float sx, sy, depth;
		qbool onscreen, vis;
		int pnum = 0, isp;
		if (ed->free)
			continue;
		isp = f_isplayer < 0 ? 0 : i_isplayer ? PRVM_E_INT(ed, f_isplayer) : (int)PRVM_E_FLOAT(ed, f_isplayer);
		if (isp & 2) // ISPLAYER_CLIENT
		{
			if (offline && sv.active)
				continue; // listed from the server above
			if (isp & 4) // ISPLAYER_LOCAL: the player itself
				continue;
			kind = "player";
			pnum = f_entnum < 0 ? 0 : i_entnum ? PRVM_E_INT(ed, f_entnum) : (int)PRVM_E_FLOAT(ed, f_entnum);
		}
		else if (f_draw >= 0 && fn_proj >= 0 && PRVM_E_INT(ed, f_draw) == fn_proj)
			kind = "projectile";
		else if (f_draw >= 0 && fn_item >= 0 && PRVM_E_INT(ed, f_draw) == fn_item)
			kind = "item";
		if (!kind || (want && strcmp(want, kind)))
			continue;
		org = PRVM_clientedictvector(ed, origin);
		VectorCopy(org, mid);
		if (!strcmp(kind, "player"))
			mid[2] += 16;
		onscreen = tlua_project(mid, &sx, &sy, &depth);
		vis = onscreen && tlua_visible(eye, mid);
		// online: players and projectiles only as the player sees them
		if (!offline && strcmp(kind, "item") && !vis)
			continue;
		lua_createtable(L, 0, 8);
		lua_pushinteger(L, pnum ? pnum : i);
		lua_setfield(L, -2, "id");
		lua_pushstring(L, kind);
		lua_setfield(L, -2, "kind");
		lua_pushnumber(L, org[0]);
		lua_setfield(L, -2, "x");
		lua_pushnumber(L, org[1]);
		lua_setfield(L, -2, "y");
		lua_pushnumber(L, org[2]);
		lua_setfield(L, -2, "z");
		lua_pushboolean(L, vis);
		lua_setfield(L, -2, "visible");
		if (f_team >= 0)
		{
			lua_pushinteger(L, i_team ? (lua_Integer)PRVM_E_INT(ed, f_team) : (lua_Integer)PRVM_E_FLOAT(ed, f_team));
			lua_setfield(L, -2, "team");
		}
		if (pnum >= 1 && pnum <= cl.maxclients && cl.scores)
		{
			char clean[MAX_SCOREBOARDNAME];
			lua_pushstring(L, TLua_CleanText(cl.scores[pnum - 1].name, clean, sizeof(clean), false));
			lua_setfield(L, -2, "name");
		}
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Events

typedef struct tlua_clev_s
{
	const char *s1, *s2, *s3;
	double a, b, c;
	int n;           // how many of a, b, c / s1.. to push (see the push functions)
	qbool hide;
} tlua_clev_t;

static int tlua_push_ab(lua_State *L, void *ud)
{
	tlua_clev_t *e = (tlua_clev_t *)ud;
	lua_pushnumber(L, e->a);
	if (e->n < 2)
		return 1;
	lua_pushnumber(L, e->b);
	if (e->n < 3)
		return 2;
	lua_pushnumber(L, e->c);
	return 3;
}

static int tlua_push_ss(lua_State *L, void *ud)
{
	tlua_clev_t *e = (tlua_clev_t *)ud;
	lua_pushstring(L, e->s1 ? e->s1 : "");
	if (e->n < 2)
		return 1;
	lua_pushstring(L, e->s2 ? e->s2 : "");
	if (e->n < 3)
		return 2;
	lua_pushstring(L, e->s3 ? e->s3 : "");
	return 3;
}

static int tlua_push_none(lua_State *L, void *ud)
{
	(void)L;
	(void)ud;
	return 0;
}

// notification: kind, name, text, args {s1, s2, s3, s4, f1, f2, f3, f4}
typedef struct tlua_notif_s { const char *kind, *name, *text, *args; qbool hide; } tlua_notif_t;
static int tlua_push_notification(lua_State *L, void *ud)
{
	tlua_notif_t *e = (tlua_notif_t *)ud;
	const char *p = e->args;
	int k = 1;
	lua_pushstring(L, e->kind);
	lua_pushstring(L, e->name);
	lua_pushstring(L, e->text);
	lua_createtable(L, 8, 0);
	while (p && k <= 8)
	{
		const char *t = strchr(p, '\t');
		size_t len = t ? (size_t)(t - p) : strlen(p);
		if (k > 4)
		{
			char num[64];
			dp_strlcpy(num, p, min(len + 1, sizeof(num)));
			lua_pushnumber(L, atof(num));
		}
		else
			lua_pushlstring(L, p, len);
		lua_rawseti(L, -2, k++);
		p = t ? t + 1 : NULL;
	}
	return 4;
}

static void tlua_result_notif_hide(lua_State *L, tlua_mod_t *m, void *ud)
{
	(void)m;
	if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1))
		((tlua_notif_t *)ud)->hide = true;
}

typedef struct tlua_touchev_s { int id; float x, y; const char *phase; } tlua_touchev_t;
static int tlua_push_touch(lua_State *L, void *ud)
{
	tlua_touchev_t *t = (tlua_touchev_t *)ud;
	lua_pushinteger(L, t->id);
	lua_pushnumber(L, t->x);
	lua_pushnumber(L, t->y);
	lua_pushstring(L, t->phase);
	return 4;
}

static void tlua_result_hide(lua_State *L, tlua_mod_t *m, void *ud)
{
	(void)m;
	if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1))
		((tlua_clev_t *)ud)->hide = true;
}

static void tlua_result_noclip(lua_State *L, tlua_mod_t *m, void *ud)
{
	(void)L;
	(void)m;
	(void)ud;
	DrawQ_ResetClipArea();
}

// csprogs.c: every line the server prints (svc_print), before the game code
// shows it; true hides it. Chat lines start with \1.
qbool TouchLua_CL_Print(const char *msg, size_t len)
{
	tlua_clev_t e;
	char text[MAX_INPUTLINE];
	qbool chat;
	if (!tlua_client.loaded || TLua_Busy() || !len)
		return false;
	chat = msg[0] == 1;
	if (!TLua_AnyHandler(&tlua_client, chat ? "chat" : "print"))
		return false;
	if (msg[len - 1] != '\n')
		return false; // a partial line: shown as it comes
	memset(&e, 0, sizeof(e));
	TLua_CleanText(msg + (chat ? 1 : 0), text, min(sizeof(text), len + 1 - (chat ? 1 : 0)), false);
	e.s1 = text;
	e.n = 1;
	TLua_Fire(&tlua_client, chat ? "chat" : "print", tlua_push_ss, tlua_result_hide, &e);
	return e.hide;
}

// csprogs.c: a centered message (svc_centerprint)
qbool TouchLua_CL_CenterPrint(const char *msg, size_t len)
{
	tlua_clev_t e;
	char text[MAX_INPUTLINE];
	if (!tlua_client.loaded || TLua_Busy() || !TLua_AnyHandler(&tlua_client, "centerprint"))
		return false;
	memset(&e, 0, sizeof(e));
	TLua_CleanText(msg, text, min(sizeof(text), len + 1), true);
	e.s1 = text;
	e.n = 1;
	TLua_Fire(&tlua_client, "centerprint", tlua_push_ss, tlua_result_hide, &e);
	return e.hide;
}

// #1740 float touch_lua_clevent(string ev, float a, float b, string s1, string s2)
static void VM_CL_touch_lua_clevent(prvm_prog_t *prog)
{
	const char *ev = PRVM_G_STRING(OFS_PARM0);
	float a = PRVM_G_FLOAT(OFS_PARM1), b = PRVM_G_FLOAT(OFS_PARM2);
	const char *s1 = PRVM_G_STRING(OFS_PARM3), *s2 = PRVM_G_STRING(OFS_PARM4);
	tlua_clev_t e;
	VM_SAFEPARMCOUNT(5, VM_CL_touch_lua_clevent);
	PRVM_G_FLOAT(OFS_RETURN) = 0;
	TLua_CL_CsqcHello();
	if (!tlua_client.loaded || TLua_Busy())
		return;
	memset(&e, 0, sizeof(e));
	if (!strncmp(ev, "notification ", 13))
	{
		char name[128], text[1024];
		const char *nl = strchr(s1, '\n'), *nm;
		size_t nlen = nl ? (size_t)(nl - s1) : strlen(s1);
		nlen = min(nlen, sizeof(name) - 1);
		memcpy(name, s1, nlen);
		name[nlen] = 0;
		nm = name;
		if (!strncmp(nm, "Notification_", 13))
			nm += 13;
		TLua_CleanText(nl ? nl + 1 : "", text, sizeof(text), true);
		if (TLua_AnyHandler(&tlua_client, "notification"))
		{
			tlua_notif_t nt;
			nt.kind = ev + 13;
			nt.name = nm;
			nt.text = text;
			nt.args = s2;
			nt.hide = false;
			TLua_Fire(&tlua_client, "notification", tlua_push_notification, tlua_result_notif_hide, &nt);
			e.hide = nt.hide;
		}
		// the kill feed: INFO_DEATH_* (the world, yourself) and INFO_WEAPON_*_MURDER_* (a frag)
		if (!strncmp(ev + 13, "info", 4) && (!strncmp(nm, "INFO_DEATH_", 11) || !strncmp(nm, "DEATH_", 6) || strstr(nm, "_MURDER")) && TLua_AnyHandler(&tlua_client, "kill"))
		{
			// the arguments: s1 is the victim, s2 the attacker in Xonotic's death notifications
			char victim[128] = "", attacker[128] = "";
			const char *t = strchr(s2, '\t');
			if (t)
			{
				const char *t2 = strchr(t + 1, '\t');
				dp_strlcpy(victim, s2, min((size_t)(t - s2) + 1, sizeof(victim)));
				dp_strlcpy(attacker, t + 1, min((size_t)((t2 ? t2 : t + 1 + strlen(t + 1)) - (t + 1)) + 1, sizeof(attacker)));
			}
			e.s1 = victim;
			e.s2 = attacker;
			e.s3 = nm;
			e.n = 3;
			TLua_Fire(&tlua_client, "kill", tlua_push_ss, NULL, &e);
		}
		PRVM_G_FLOAT(OFS_RETURN) = e.hide ? 1 : 0;
		return;
	}
	if (!TLua_AnyHandler(&tlua_client, ev))
		return;
	if (!strcmp(ev, "match"))
	{
		e.s1 = s1;
		e.s2 = s2;
		e.n = 2;
		TLua_Fire(&tlua_client, ev, tlua_push_ss, NULL, &e);
	}
	else if (!strcmp(ev, "hit"))
	{
		e.a = a;
		e.n = 1;
		TLua_Fire(&tlua_client, ev, tlua_push_ab, NULL, &e);
	}
	else if (!strcmp(ev, "pickup"))
	{
		e.s1 = s1;
		e.n = 1;
		TLua_Fire(&tlua_client, ev, tlua_push_ss, NULL, &e);
	}
	else if (!strcmp(ev, "hud_overlay") || !strcmp(ev, "scoreboard"))
	{
		e.a = a;
		e.b = b;
		e.n = 2;
		tlua_cl_drawing = true;
		TLua_Fire(&tlua_client, ev, tlua_push_ab, tlua_result_noclip, &e);
		tlua_cl_drawing = false;
		DrawQ_ResetClipArea();
	}
}

#define TLUA_CL_NUMBUILTINS 1755
static prvm_builtin_t tlua_cl_builtins[TLUA_CL_NUMBUILTINS];
void TLua_Hooks_InstallClientBuiltins(prvm_builtin_t *b);

// csprogs.c, CL_VM_Init: the client VM gets the Lua builtins
void TouchLua_CL_InstallBuiltins(prvm_prog_t *prog)
{
	int i;
	tlua_cl_hello = false;
	if (prog->numbuiltins >= 1740)
	{
		Con_Printf(CON_WARN "Lua: the client VM has %d builtins, #1740 is taken; client mods get no game events\n", prog->numbuiltins);
		return;
	}
	for (i = 0; i < TLUA_CL_NUMBUILTINS; i++)
		tlua_cl_builtins[i] = i < prog->numbuiltins ? prog->builtins[i] : NULL;
	tlua_cl_builtins[1740] = VM_CL_touch_lua_clevent;
	TLua_Hooks_InstallClientBuiltins(tlua_cl_builtins);
	prog->builtins = tlua_cl_builtins;
	prog->numbuiltins = TLUA_CL_NUMBUILTINS;
}

// what the engine sees every frame: weapon, damage, spawn, death, spectate,
// score, connect, disconnect, fingers
void TLua_CL_PollEvents(void);
void TLua_CL_PollEvents(void)
{
	static int weapon = -1, health = -1000, armor, frags = -100000, spec = -1, state = -1;
	static qbool alive;
	static int fingers[10];
	static float fpos[10][2];
	tlua_clev_t e;
	qbool ingame = cls.state == ca_connected && cls.signon == SIGNONS && cl.scores;
	int s = ingame ? 1 : 0, i;
	memset(&e, 0, sizeof(e));
	if (s != state)
	{
		if (s)
		{
			const char *addr = sv.active ? "local" : (cls.netcon ? cls.netcon->address : "");
			e.s1 = addr;
			e.n = 1;
			tlua_cl_hello_signon = cls.signon;
			TLua_Fire(&tlua_client, "connect", tlua_push_ss, NULL, &e);
		}
		else if (state == 1)
		{
			TLua_Fire(&tlua_client, "disconnect", tlua_push_none, NULL, &e);
			tlua_cl_hello = false;
		}
		state = s;
		weapon = -1;
		health = -1000;
		frags = -100000;
		spec = -1;
	}
	if (cls.state != ca_connected)
		tlua_cl_hello = false;
	if (ingame)
	{
		int w = cl.stats[STAT_ACTIVEWEAPON], h = cl.stats[STAT_HEALTH], ar = cl.stats[STAT_ARMOR];
		int sp = (cl.viewentity != cl.playerentity && cl.viewentity >= 1 && cl.viewentity <= cl.maxclients) ? cl.viewentity : 0;
		int fr = (cl.playerentity >= 1 && cl.playerentity <= cl.maxclients) ? cl.scores[cl.playerentity - 1].frags : 0;
		qbool al = h > 0 && !cl.intermission;
		if (weapon != -1 && w != weapon)
		{
			e.a = w;
			e.b = weapon;
			e.n = 2;
			TLua_Fire(&tlua_client, "weapon", tlua_push_ab, NULL, &e);
		}
		weapon = w;
		if (health != -1000 && al && alive && (h < health || ar < armor))
		{
			e.a = (health - h) + max(0, armor - ar);
			e.b = h;
			e.c = ar;
			e.n = 3;
			TLua_Fire(&tlua_client, "damage", tlua_push_ab, NULL, &e);
		}
		if (health != -1000 && al != alive)
			TLua_Fire(&tlua_client, al ? "spawn" : "death", tlua_push_none, NULL, &e);
		health = h;
		armor = ar;
		alive = al;
		if (spec != -1 && sp != spec)
		{
			e.a = sp;
			e.b = spec;
			e.n = 2;
			TLua_Fire(&tlua_client, "spectate", tlua_push_ab, NULL, &e);
		}
		spec = sp;
		if (frags != -100000 && fr != frags)
		{
			e.a = fr;
			e.b = frags;
			e.n = 2;
			TLua_Fire(&tlua_client, "score", tlua_push_ab, NULL, &e);
		}
		frags = fr;
	}
	if (TLua_AnyHandler(&tlua_client, "touch"))
		for (i = 0; i < 10; i++)
		{
			int act;
			float nx, ny;
			tlua_finger(i, &act, &nx, &ny);
			if (act || fingers[i])
			{
				const char *phase = act && !fingers[i] ? "down" : !act ? "up" : (nx != fpos[i][0] || ny != fpos[i][1]) ? "move" : NULL;
				if (phase)
				{
					tlua_touchev_t t;
					t.id = i;
					t.x = act ? nx : fpos[i][0];
					t.y = act ? ny : fpos[i][1];
					t.phase = phase;
					TLua_Fire(&tlua_client, "touch", tlua_push_touch, NULL, &t);
				}
			}
			fingers[i] = act;
			if (act)
			{
				fpos[i][0] = nx;
				fpos[i][1] = ny;
			}
		}
	tlua_widgets_input();
}

// ---------------------------------------------------------------------------

void TLua_CLAPI_ModStopped(tlua_mod_t *m)
{
	int i;
	for (i = 0; i < TLUA_MAX_WIDGETS; i++)
		if (tlua_widgets[i].used && !strcmp(tlua_widgets[i].mod, m->name))
			tlua_widget_free(m, &tlua_widgets[i]);
	for (i = 0; i < TLUA_MAX_BINDS; i++)
		if (tlua_binds[i].used && !strcmp(tlua_binds[i].mod, m->name))
			tlua_binds[i].used = false;
	tlua_images_free(m);
	if (cls.state != ca_dedicated)
		tlua_widgets_publish();
}

// lua_mod_simulate key <name> <1|0> / finger <0-9> <x> <y> <1|0>: input for
// testing a mod (in 2D units, as xt.draw), on any device
static void tlua_simulate_f(cmd_state_t *cmd)
{
	const char *what = Cmd_Argv(cmd, 1);
	if (TLua_RemoteRefused(cmd))
		return;
	if (!strcmp(what, "key") && Cmd_Argc(cmd) == 4)
	{
		int k = Key_StringToKeynum(Cmd_Argv(cmd, 2));
		if (k < 0 || k >= MAX_KEYS)
		{
			Con_Printf("lua_mod_simulate: no key %s\n", Cmd_Argv(cmd, 2));
			return;
		}
		TouchLua_KeyEvent(k, 0, atoi(Cmd_Argv(cmd, 3)) != 0);
		return;
	}
	if (!strcmp(what, "finger") && Cmd_Argc(cmd) == 6)
	{
		int slot = atoi(Cmd_Argv(cmd, 2));
		if (slot < 0 || slot > 9)
			return;
		tlua_sim_fx[slot] = (float)atof(Cmd_Argv(cmd, 3));
		tlua_sim_fy[slot] = (float)atof(Cmd_Argv(cmd, 4));
		tlua_sim_finger[slot] = atoi(Cmd_Argv(cmd, 5)) != 0;
		return;
	}
	Con_Printf("usage: lua_mod_simulate key <name> <1|0>  or  lua_mod_simulate finger <0-9> <x> <y> <1|0>\n");
}

void TLua_CLAPI_Setup(void)
{
	if (cls.state == ca_dedicated)
		return;
	TLua_RegisterCvar(&lua_hud_panels);
	Cmd_AddCommand(CF_CLIENT, "lua_mod_simulate", tlua_simulate_f, "press a key or put a finger down for client mods, to test them: lua_mod_simulate key <name> <1|0> / finger <0-9> <x> <y> <1|0>");
}
