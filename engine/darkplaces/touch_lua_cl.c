/*
Godnotic: the client realm of the Lua sandbox (touch_lua.c): mods from
<userdir>/mods that draw over the HUD, see keys, add console commands.

What a client mod can learn about the match is limited to what the scoreboard
and the player's own HUD already show: its own health, armor, weapon and
frags, and every player's name, frags and team colour. There is no call that
returns another player's position, an entity list, or a trace, so a mod cannot
see anyone through walls; and there is no call that sets view angles or
presses +attack, so a mod cannot aim or fire (the offline practice tools in
touch_aim.c / touch_tactics.c stay behind their own gate and are not exposed).
*/

#include "quakedef.h"
#include "cl_screen.h"
#include "touch_lua.h"
#include "touch_lua_int.h"

qbool tlua_cl_drawing;
static qbool tlua_keystate[MAX_KEYS]; // game keys down, for xt.keydown

static const char *const tlua_cl_events[] = {"frame", "hud", "draw", "key", "menu", "map", "unload", NULL};

static qbool tlua_prefixed(const char *name, const char *const *list)
{
	int i;
	for (i = 0; list[i]; i++)
		if (!strncmp(name, list[i], strlen(list[i])))
			return true;
	return false;
}

// cvars a client mod may set: looks, sound and HUD; nothing about networking,
// files, logging, binds, aim or the touch assists
static qbool tlua_cl_cvar_writable(const char *name)
{
	static const char *const allow[] = {
		"crosshair", "hud_", "con_notify", "con_chat", "viewsize", "fov",
		"cl_bob", "showfps", "cl_showfps", "showtime", "showdate", "sensitivity",
		"volume", "bgmvolume", "r_drawviewmodel", "cl_gunalign", "scr_conalpha",
		"scr_centertime", "cl_hidewaypoints", "cl_zoomfactor", "cl_zoomspeed",
		NULL};
	return tlua_prefixed(name, allow);
}

// Console commands a client mod may run (xt.exec, with the exec permission).
// Nothing that writes files (screenshot), changes binds or settings, quits
// or connects, opens a text input or the menu over the game (messagemode,
// togglemenu: on a touchscreen there may be no way back), or loads files by
// name without a limit (play; xt.sound is the bounded way):
//   echo                    text in the console
//   say, say_team           chat as the player: one line every 2 s, all mods together
//   weapnext, weapprev, weaplast, weapbest   switch weapon
//   impulse <number>        the game's weapon and action impulses (the server decides)
//   ready                   toggle ready in warmup
//   +showscores, -showscores the scoreboard
//   kill                    respawn (suicide)
static qbool tlua_cl_command_allowed(const char *word, const char *line)
{
	static const char *const allow[] = {
		"echo", "say", "say_team", "weapnext", "weapprev", "weaplast", "weapbest",
		"ready", "+showscores", "-showscores", "kill", "impulse", NULL};
	static double lastsay;
	int i;
	for (i = 0; allow[i]; i++)
		if (!strcmp(word, allow[i]))
			break;
	if (!allow[i])
		return false;
	if (!strcmp(word, "impulse"))
	{
		// exactly one number
		const char *p = line;
		while (*p == ' ')
			p++;
		p += 7;
		while (*p == ' ')
			p++;
		if (!*p)
			return false;
		for (; *p >= '0' && *p <= '9'; p++)
			;
		while (*p == ' ')
			p++;
		return !*p;
	}
	if (!strncmp(word, "say", 3))
	{
		// no chat spam: one line every two seconds from all mods together
		if (host.realtime - lastsay < 2)
			return false;
		lastsay = host.realtime;
	}
	return true;
}

// Pictures and sounds a mod loads stay in the engine's caches for the whole
// session (the picture cache has a fixed number of slots, and a full one
// hands every later caller the wrong picture), so all mods together may name
// only so many different files.
#define TLUA_MAX_ASSETS 64
static char tlua_assets[TLUA_MAX_ASSETS * 2][100];
static int tlua_numassets;

static qbool tlua_cl_asset_ok(const char *path)
{
	int i;
	for (i = 0; i < tlua_numassets; i++)
		if (!strcmp(tlua_assets[i], path))
			return true;
	if (tlua_numassets >= (int)(sizeof(tlua_assets) / sizeof(tlua_assets[0])))
		return false;
	{
		// pictures and sounds have separate budgets
		int n = 0, pic = !strncmp(path, "gfx/", 4);
		for (i = 0; i < tlua_numassets; i++)
			if ((strncmp(tlua_assets[i], "gfx/", 4) == 0) == pic)
				n++;
		if (n >= TLUA_MAX_ASSETS)
			return false;
	}
	dp_strlcpy(tlua_assets[tlua_numassets++], path, sizeof(tlua_assets[0]));
	return true;
}

// ---------------------------------------------------------------------------
// xt.draw: only inside the "hud" event

static void tlua_cl_needdraw(lua_State *L)
{
	if (!tlua_cl_drawing)
		luaL_error(L, "xt.draw works only inside the \"hud\" event");
}

static float tlua_optcolor(lua_State *L, int idx)
{
	return (float)bound(0, luaL_optnumber(L, idx, 1), 1);
}

static int tlua_draw_fill(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_DISPLAY);
	tlua_cl_needdraw(L);
	DrawQ_Fill((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4),
		tlua_optcolor(L, 5), tlua_optcolor(L, 6), tlua_optcolor(L, 7), tlua_optcolor(L, 8), 0);
	return 0;
}

static int tlua_draw_text(lua_State *L)
{
	size_t len;
	const char *s;
	float size, w;
	TLua_Require(L, TLUA_PERM_DISPLAY);
	tlua_cl_needdraw(L);
	s = luaL_checklstring(L, 3, &len);
	size = (float)bound(1, luaL_optnumber(L, 4, 12), 256);
	if (len > 1024)
		len = 1024;
	w = DrawQ_String((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), s, len, size, size,
		tlua_optcolor(L, 5), tlua_optcolor(L, 6), tlua_optcolor(L, 7), tlua_optcolor(L, 8), 0, NULL, false, FONT_DEFAULT);
	lua_pushnumber(L, w);
	return 1;
}

static int tlua_draw_textwidth(lua_State *L)
{
	size_t len;
	const char *s = luaL_checklstring(L, 1, &len);
	float size = (float)bound(1, luaL_optnumber(L, 2, 12), 256);
	TLua_Require(L, TLUA_PERM_DISPLAY);
	if (len > 1024)
		len = 1024;
	lua_pushnumber(L, DrawQ_TextWidth(s, len, size, size, false, FONT_DEFAULT));
	return 1;
}

static int tlua_draw_pic(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1), *p;
	cachepic_t *pic;
	TLua_Require(L, TLUA_PERM_DISPLAY);
	tlua_cl_needdraw(L);
	if (strncmp(path, "gfx/", 4) || strstr(path, "..") || strlen(path) > 96)
		return luaL_error(L, "xt.draw.pic: the path must start with gfx/");
	for (p = path; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '/' && *p != '_' && *p != '-' && *p != '.')
			return luaL_error(L, "xt.draw.pic: bad character in path");
	if (!tlua_cl_asset_ok(path))
		return 0; // over the budget of different pictures
	pic = Draw_CachePic_Flags(path, CACHEPICFLAG_NOTPERSISTENT | CACHEPICFLAG_FAILONMISSING);
	if (!pic)
		return 0;
	DrawQ_Pic((float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), pic, (float)luaL_checknumber(L, 4), (float)luaL_checknumber(L, 5),
		tlua_optcolor(L, 6), tlua_optcolor(L, 7), tlua_optcolor(L, 8), tlua_optcolor(L, 9), 0);
	lua_pushboolean(L, 1);
	return 1;
}

// ---------------------------------------------------------------------------
// What the player can already see

static qbool tlua_cl_ingame(void)
{
	return cls.state == ca_connected && cls.signon == SIGNONS && cl.scores;
}

static int tlua_screen(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_DISPLAY);
	lua_pushnumber(L, vid_conwidth.value);
	lua_pushnumber(L, vid_conheight.value);
	return 2;
}

static int tlua_ingame(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushboolean(L, tlua_cl_ingame());
	return 1;
}

static int tlua_islocal(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushboolean(L, sv.active);
	return 1;
}

static int tlua_map(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushstring(L, tlua_cl_ingame() ? cl.worldbasename : "");
	return 1;
}

static int tlua_gametime(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushnumber(L, cl.time);
	return 1;
}

static void tlua_pushscore(lua_State *L, int i)
{
	scoreboard_t *s = &cl.scores[i];
	char clean[MAX_SCOREBOARDNAME];
	lua_createtable(L, 0, 5);
	lua_pushstring(L, TLua_CleanText(s->name, clean, sizeof(clean), false));
	lua_setfield(L, -2, "name");
	lua_pushinteger(L, s->frags);
	lua_setfield(L, -2, "frags");
	lua_pushinteger(L, s->colors & 15);
	lua_setfield(L, -2, "color");
	lua_pushinteger(L, i + 1);
	lua_setfield(L, -2, "slot");
	lua_pushboolean(L, i + 1 == cl.playerentity);
	lua_setfield(L, -2, "me");
}

static int tlua_players(lua_State *L)
{
	int i, n = 0;
	TLua_Require(L, TLUA_PERM_PLAYERS);
	lua_newtable(L);
	if (!tlua_cl_ingame())
		return 1;
	for (i = 0; i < cl.maxclients; i++)
	{
		if (!cl.scores[i].name[0])
			continue;
		tlua_pushscore(L, i);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int tlua_me(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_PLAYERS);
	if (!tlua_cl_ingame())
		return 0;
	lua_createtable(L, 0, 8);
	lua_pushinteger(L, cl.stats[STAT_HEALTH]);
	lua_setfield(L, -2, "health");
	lua_pushinteger(L, cl.stats[STAT_ARMOR]);
	lua_setfield(L, -2, "armor");
	lua_pushinteger(L, cl.stats[STAT_ACTIVEWEAPON]);
	lua_setfield(L, -2, "weapon");
	lua_pushboolean(L, cl.stats[STAT_HEALTH] > 0 && !cl.intermission);
	lua_setfield(L, -2, "alive");
	if (cl.playerentity >= 1 && cl.playerentity <= cl.maxclients)
	{
		char clean[MAX_SCOREBOARDNAME];
		scoreboard_t *s = &cl.scores[cl.playerentity - 1];
		lua_pushstring(L, TLua_CleanText(s->name, clean, sizeof(clean), false));
		lua_setfield(L, -2, "name");
		lua_pushinteger(L, s->frags);
		lua_setfield(L, -2, "frags");
		lua_pushinteger(L, s->colors & 15);
		lua_setfield(L, -2, "color");
	}
	return 1;
}

static int tlua_draw_clip(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_DISPLAY);
	tlua_cl_needdraw(L);
	DrawQ_SetClipArea((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4));
	return 0;
}

static int tlua_draw_noclip(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_DISPLAY);
	tlua_cl_needdraw(L);
	DrawQ_ResetClipArea();
	return 0;
}

// fingers on the screen (and the mouse pointer as slot 10), in 2D coordinates
static int tlua_fingers(lua_State *L)
{
	int slot, n = 0;
	TLua_Require(L, TLUA_PERM_INPUT);
	lua_newtable(L);
	for (slot = 0; slot < 10; slot++)
	{
		int active;
		float nx, ny;
		VID_GetTouchFinger(slot, &active, &nx, &ny);
		if (!active)
			continue;
		lua_createtable(L, 0, 3);
		lua_pushinteger(L, slot);
		lua_setfield(L, -2, "id");
		lua_pushnumber(L, nx * vid_conwidth.value);
		lua_setfield(L, -2, "x");
		lua_pushnumber(L, ny * vid_conheight.value);
		lua_setfield(L, -2, "y");
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int tlua_mouse(lua_State *L)
{
	float x = vid.mode.width ? in_windowmouse_x * vid_conwidth.value / vid.mode.width : 0;
	float y = vid.mode.height ? in_windowmouse_y * vid_conheight.value / vid.mode.height : 0;
	TLua_Require(L, TLUA_PERM_INPUT);
	lua_pushnumber(L, x);
	lua_pushnumber(L, y);
	return 2;
}

static int tlua_keydown(lua_State *L)
{
	int k = Key_StringToKeynum(luaL_checkstring(L, 1));
	TLua_Require(L, TLUA_PERM_INPUT);
	lua_pushboolean(L, k >= 0 && k < MAX_KEYS && tlua_keystate[k]);
	return 1;
}

static int tlua_sound(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1), *p;
	static double window;
	static int count;
	TLua_Require(L, TLUA_PERM_DISPLAY);
	if (strncmp(path, "sound/", 6) || strstr(path, "..") || strlen(path) > 96)
		return luaL_error(L, "xt.sound: the path must start with sound/");
	for (p = path; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '/' && *p != '_' && *p != '-' && *p != '.')
			return luaL_error(L, "xt.sound: bad character in path");
	if (host.realtime - window > 1)
	{
		window = host.realtime;
		count = 0;
	}
	if (++count > 16) // no more than 16 sounds a second from all mods
		return 0;
	if (!tlua_cl_asset_ok(path))
		return 0; // over the budget of different sounds
	lua_pushboolean(L, S_LocalSound(path));
	return 1;
}

static void tlua_cl_openapi(lua_State *L)
{
	TLua_SetField(L, "screen", tlua_screen);
	TLua_SetField(L, "ingame", tlua_ingame);
	TLua_SetField(L, "islocal", tlua_islocal);
	TLua_SetField(L, "map", tlua_map);
	TLua_SetField(L, "time", tlua_gametime);
	TLua_SetField(L, "players", tlua_players);
	TLua_SetField(L, "me", tlua_me);
	TLua_SetField(L, "fingers", tlua_fingers);
	TLua_SetField(L, "mouse", tlua_mouse);
	TLua_SetField(L, "keydown", tlua_keydown);
	TLua_SetField(L, "sound", tlua_sound);
	lua_newtable(L);
	TLua_SetField(L, "fill", tlua_draw_fill);
	TLua_SetField(L, "text", tlua_draw_text);
	TLua_SetField(L, "textwidth", tlua_draw_textwidth);
	TLua_SetField(L, "pic", tlua_draw_pic);
	TLua_SetField(L, "clip", tlua_draw_clip);
	TLua_SetField(L, "noclip", tlua_draw_noclip);
	lua_setfield(L, -2, "draw");
}

tlua_realm_t tlua_client = {
	"mod", "mods", "lua_", CF_CLIENT, tlua_cl_events,
	{{{0}}}, 0, false, NULL, NULL,
	tlua_cl_openapi, tlua_cl_cvar_writable, tlua_cl_command_allowed
};

void TLua_CL_Setup(void)
{
	char path[1024];
	if (cls.state == ca_dedicated)
		return;
	// create the folder so players can find where mods go
	dpsnprintf(path, sizeof(path), "%smods/", fs_userdir);
	FS_CreatePath(path);
}

// ---------------------------------------------------------------------------
// Events

typedef struct tlua_args_s
{
	const char *s;
	double n1, n2;
	qbool b;
	qbool consumed;
} tlua_args_t;

static int tlua_push_n(lua_State *L, void *ud)
{
	lua_pushnumber(L, ((tlua_args_t *)ud)->n1);
	return 1;
}

static int tlua_push_nn(lua_State *L, void *ud)
{
	lua_pushnumber(L, ((tlua_args_t *)ud)->n1);
	lua_pushnumber(L, ((tlua_args_t *)ud)->n2);
	return 2;
}

static int tlua_push_s(lua_State *L, void *ud)
{
	lua_pushstring(L, ((tlua_args_t *)ud)->s);
	return 1;
}

static int tlua_push_b(lua_State *L, void *ud)
{
	lua_pushboolean(L, ((tlua_args_t *)ud)->b);
	return 1;
}

static int tlua_push_key(lua_State *L, void *ud)
{
	tlua_args_t *a = (tlua_args_t *)ud;
	char ch[2];
	lua_pushstring(L, a->s);
	lua_pushboolean(L, a->b);
	ch[0] = (a->n1 >= 32 && a->n1 < 127) ? (char)a->n1 : 0;
	ch[1] = 0;
	lua_pushstring(L, ch);
	return 3;
}

// after each handler of a draw event: no clip area leaks into the next mod
static void tlua_result_noclip(lua_State *L, tlua_mod_t *m, void *ud)
{
	(void)L;
	(void)m;
	(void)ud;
	DrawQ_ResetClipArea();
}

static void tlua_result_consume(lua_State *L, tlua_mod_t *m, void *ud)
{
	(void)m;
	if (lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1))
		((tlua_args_t *)ud)->consumed = true;
}

static void tlua_cl_frame(void)
{
	static double lastrealtime, lastscan;
	static qbool menuopen;
	static char lastmap[MAX_QPATH];
	tlua_args_t a;
	qbool open;
	const char *map;
	open = key_dest == key_menu || key_dest == key_menu_grabbed;
	if (open && host.realtime - lastscan > 2)
	{
		// the Mods dialog lists _cl_lua_mods: pick up folders added meanwhile
		lastscan = host.realtime;
		TLua_Rescan(&tlua_client);
	}
	if (!tlua_client.loaded)
	{
		if (tlua_client.enable->integer)
			TLua_LoadAll(&tlua_client);
		lastrealtime = host.realtime;
		return;
	}
	if (!tlua_client.enable->integer)
	{
		TLua_UnloadAll(&tlua_client, "cl_lua_enable 0");
		return;
	}
	memset(&a, 0, sizeof(a));
	map = tlua_cl_ingame() ? cl.worldbasename : "";
	if (strcmp(map, lastmap))
	{
		dp_strlcpy(lastmap, map, sizeof(lastmap));
		a.s = lastmap;
		TLua_Fire(&tlua_client, "map", tlua_push_s, NULL, &a);
	}
	if (open != menuopen)
	{
		menuopen = open;
		a.b = open;
		TLua_Fire(&tlua_client, "menu", tlua_push_b, NULL, &a);
	}
	a.n1 = host.realtime - lastrealtime;
	lastrealtime = host.realtime;
	TLua_Fire(&tlua_client, "frame", tlua_push_n, NULL, &a);
}

static void tlua_cl_draw(const char *event)
{
	tlua_args_t a;
	if (!TLua_AnyHandler(&tlua_client, event))
		return;
	memset(&a, 0, sizeof(a));
	a.n1 = vid_conwidth.value;
	a.n2 = vid_conheight.value;
	tlua_cl_drawing = true;
	TLua_Fire(&tlua_client, event, tlua_push_nn, tlua_result_noclip, &a);
	tlua_cl_drawing = false;
	DrawQ_ResetClipArea();
}

void TouchLua_ClientFrame(void)
{
	TLua_InitOnce();
	TLua_FrameStart();
	// a loading screen redraw inside a call into a mod (a picture or sound it
	// loads) comes back here: leave that call alone
	if (TLua_Busy())
		return;
	tlua_cl_drawing = false;
	TLua_SV_CheckStopped();
	if (cls.state == ca_dedicated)
		return;
	tlua_cl_frame();
	if (!tlua_client.loaded)
		return;
	if (tlua_cl_ingame())
		tlua_cl_draw("hud");
	tlua_cl_draw("draw");
}

qbool TouchLua_KeyEvent(int key, int ascii, qbool down)
{
	static qbool held[MAX_KEYS]; // keys whose press a mod took: their release goes nowhere too
	tlua_args_t a;
	char name[64];
	qbool took;
	if (key < 0 || key >= MAX_KEYS)
		return false;
	tlua_keystate[key] = down;
	if (TLua_Busy()) // a key read while a mod's call loads something
		return false;
	if (!tlua_client.loaded || !TLua_AnyHandler(&tlua_client, "key"))
	{
		took = !down && held[key];
		held[key] = false;
		return took;
	}
	memset(&a, 0, sizeof(a));
	a.s = Key_KeynumToString(key, name, sizeof(name));
	a.b = down;
	a.n1 = ascii;
	TLua_Fire(&tlua_client, "key", tlua_push_key, tlua_result_consume, &a);
	if (down && a.consumed)
	{
		// the way out stays open: keys bound to the console or the menu are never taken
		const char *bind = Key_GetBind(key, -1);
		if (bind && (strstr(bind, "toggleconsole") || strstr(bind, "togglemenu") || strstr(bind, "menu_")))
			a.consumed = false;
	}
	if (down)
	{
		held[key] = a.consumed;
		return a.consumed;
	}
	// a release is only kept from the binding when its press was: otherwise
	// a +command could stay held
	took = held[key];
	held[key] = false;
	return took;
}
