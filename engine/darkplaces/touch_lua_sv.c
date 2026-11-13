/*
Xonotic Touch: the server realm of the Lua sandbox (touch_lua.c): plugins from
<userdir>/plugins on a server this engine hosts (a local match or a dedicated
server), driven by the game's own events.

The game code reports its events through one builtin the Touch QC mutator calls
(qcsrc/server/touch_lua.qc):

  float touch_lua_event(string ev, entity a, entity b, float n, string s) = #1740;

It only exists in this engine; the QC checks for the sv_lua_enable engine cvar
before calling it. Plugins never see entities, only player numbers 1..maxclients
(0 is the world / not a player), which every API call checks again.

Server plugins have no way to reach clients beyond chat and centerprints, and no
aim or view API at all; cvars and commands go through allowlists that leave out
logging, URLs, bans, passwords and files.
*/

#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "touch_lua.h"
#include "touch_lua_int.h"

#define TLUA_SV_BUILTIN_EVENT 1740
#define TLUA_SV_NUMBUILTINS (TLUA_SV_BUILTIN_EVENT + 1)

static const char *const tlua_sv_events[] = {
	"frame", "map", "connect", "disconnect", "spawn", "death", "damage",
	"chat", "pickup", "clientcmd", "matchend", "unload", NULL};

static qbool tlua_sv_cvar_writable(const char *name)
{
	static const char *const allow[] = {
		"g_balance_", "g_start_weapon_", "g_pickup_", "g_weaponarena", "g_instagib",
		"g_nix", "g_grappling_hook", "g_vampire", "g_midair", "g_jetpack", "g_dodging",
		"g_bloodloss", "g_rocket_flying", "g_nades", "g_buffs", "g_powerups",
		"g_new_toys", "g_overkill", "g_melee_only", "g_random_gravity", "g_walljump",
		"g_spawnshieldtime", "g_respawn_delay", "g_friendlyfire", "g_mirrordamage",
		"timelimit", "fraglimit", "leadlimit", "sv_gravity", NULL};
	static const char *const deny[] = {"uri", "url", "log", "ban", "http", "dest", "file", "path", "password", NULL};
	int i;
	for (i = 0; deny[i]; i++)
		if (strstr(name, deny[i]))
			return false;
	for (i = 0; allow[i]; i++)
		if (!strncmp(name, allow[i], strlen(allow[i])))
			return true;
	return false;
}

static qbool tlua_sv_command_allowed(const char *word, const char *line)
{
	static const char *const allow[] = {"say", "echo", "restart", "kick", NULL};
	static const char *const svcmd[] = {"gotomap", "endmatch", "allready", "lockteams", "unlockteams", "shuffleteams", "nextmap", NULL};
	int i;
	if (!strcmp(word, "sv_cmd"))
	{
		const char *p = line;
		char sub[32];
		size_t n = 0;
		while (*p == ' ')
			p++;
		p += 6;
		while (*p == ' ')
			p++;
		while (*p && *p != ' ' && n + 1 < sizeof(sub))
			sub[n++] = (char)tolower((unsigned char)*p++);
		sub[n] = 0;
		for (i = 0; svcmd[i]; i++)
			if (!strcmp(sub, svcmd[i]))
				return true;
		return false;
	}
	for (i = 0; allow[i]; i++)
		if (!strcmp(word, allow[i]))
			return true;
	return false;
}

// ---------------------------------------------------------------------------
// API

static client_t *tlua_sv_client(lua_State *L, int idx)
{
	lua_Integer n = luaL_checkinteger(L, idx);
	client_t *c;
	if (!sv.active || n < 1 || n > svs.maxclients)
		return NULL;
	c = svs.clients + (n - 1);
	return c->active ? c : NULL;
}

static int tlua_sv_players(lua_State *L)
{
	int i, n = 0;
	TLua_Require(L, TLUA_PERM_PLAYERS);
	lua_newtable(L);
	if (!sv.active)
		return 1;
	for (i = 0; i < svs.maxclients; i++)
	{
		if (!svs.clients[i].active)
			continue;
		lua_pushinteger(L, i + 1);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static int tlua_sv_player(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	client_t *c;
	prvm_edict_t *ed;
	char clean[64];
	float *o;
	TLua_Require(L, TLUA_PERM_PLAYERS);
	c = tlua_sv_client(L, 1);
	if (!c || !c->edict)
		return 0;
	ed = c->edict;
	lua_createtable(L, 0, 10);
	lua_pushinteger(L, (lua_Integer)(c - svs.clients) + 1);
	lua_setfield(L, -2, "id");
	lua_pushstring(L, TLua_CleanText(c->name, clean, sizeof(clean), false));
	lua_setfield(L, -2, "name");
	lua_pushboolean(L, c->netconnection == NULL);
	lua_setfield(L, -2, "bot");
	lua_pushinteger(L, (lua_Integer)PRVM_serveredictfloat(ed, frags));
	lua_setfield(L, -2, "frags");
	lua_pushboolean(L, PRVM_serveredictfloat(ed, frags) == -666); // Xonotic's FRAGS_SPECTATOR
	lua_setfield(L, -2, "spectator");
	lua_pushinteger(L, (lua_Integer)PRVM_serveredictfloat(ed, team));
	lua_setfield(L, -2, "team");
	lua_pushinteger(L, (lua_Integer)PRVM_serveredictfloat(ed, health));
	lua_setfield(L, -2, "health");
	lua_pushinteger(L, (lua_Integer)PRVM_serveredictfloat(ed, armorvalue));
	lua_setfield(L, -2, "armor");
	lua_pushboolean(L, PRVM_serveredictfloat(ed, deadflag) == 0 && PRVM_serveredictfloat(ed, health) > 0);
	lua_setfield(L, -2, "alive");
	o = PRVM_serveredictvector(ed, origin);
	lua_createtable(L, 3, 0);
	lua_pushnumber(L, o[0]);
	lua_rawseti(L, -2, 1);
	lua_pushnumber(L, o[1]);
	lua_rawseti(L, -2, 2);
	lua_pushnumber(L, o[2]);
	lua_rawseti(L, -2, 3);
	lua_setfield(L, -2, "origin");
	return 1;
}

static const char *tlua_sv_text(lua_State *L, int idx, char *out, size_t outsize)
{
	return TLua_CleanText(luaL_checkstring(L, idx), out, outsize, true);
}

static void tlua_sv_send(client_t *c, int svc, const char *text)
{
	if (!c->netconnection)
		return;
	MSG_WriteChar(&c->netconnection->message, svc);
	MSG_WriteString(&c->netconnection->message, text);
}

static int tlua_sv_tell(lua_State *L)
{
	char text[1024];
	client_t *c = tlua_sv_client(L, 1);
	TLua_Require(L, TLUA_PERM_MESSAGES);
	tlua_sv_text(L, 2, text, sizeof(text) - 1);
	if (c)
	{
		dp_strlcat(text, "\n", sizeof(text));
		tlua_sv_send(c, svc_print, text);
	}
	return 0;
}

static int tlua_sv_centerprint(lua_State *L)
{
	char text[1024];
	client_t *c = tlua_sv_client(L, 1);
	TLua_Require(L, TLUA_PERM_MESSAGES);
	tlua_sv_text(L, 2, text, sizeof(text));
	if (c)
		tlua_sv_send(c, svc_centerprint, text);
	return 0;
}

static int tlua_sv_broadcast(lua_State *L)
{
	char text[1024];
	TLua_Require(L, TLUA_PERM_MESSAGES);
	tlua_sv_text(L, 1, text, sizeof(text) - 1);
	dp_strlcat(text, "\n", sizeof(text));
	if (sv.active)
		SV_BroadcastPrint(text);
	return 0;
}

static int tlua_sv_map(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushstring(L, sv.active ? sv.worldbasename : "");
	return 1;
}

static int tlua_sv_time(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushnumber(L, sv.time);
	return 1;
}

static int tlua_sv_maxplayers(lua_State *L)
{
	TLua_Require(L, TLUA_PERM_STATE);
	lua_pushinteger(L, svs.maxclients);
	return 1;
}

static void tlua_sv_openapi(lua_State *L)
{
	TLua_SetField(L, "players", tlua_sv_players);
	TLua_SetField(L, "player", tlua_sv_player);
	TLua_SetField(L, "tell", tlua_sv_tell);
	TLua_SetField(L, "centerprint", tlua_sv_centerprint);
	TLua_SetField(L, "broadcast", tlua_sv_broadcast);
	TLua_SetField(L, "map", tlua_sv_map);
	TLua_SetField(L, "time", tlua_sv_time);
	TLua_SetField(L, "maxplayers", tlua_sv_maxplayers);
}

tlua_realm_t tlua_server = {
	"plugin", "plugins", "sv_lua_", CF_SERVER, tlua_sv_events,
	{{{0}}}, 0, false, NULL, NULL,
	tlua_sv_openapi, tlua_sv_cvar_writable, tlua_sv_command_allowed
};

void TLua_SV_Setup(void)
{
}

// ---------------------------------------------------------------------------
// Events

typedef struct tlua_svev_s
{
	int a, b;
	double n;
	const char *s;
	const char *s2;
	qbool block;
} tlua_svev_t;

static int tlua_push_a(lua_State *L, void *ud)
{
	lua_pushinteger(L, ((tlua_svev_t *)ud)->a);
	return 1;
}

static int tlua_push_death(lua_State *L, void *ud)
{
	tlua_svev_t *e = (tlua_svev_t *)ud;
	lua_pushinteger(L, e->a);
	lua_pushinteger(L, e->b);
	lua_pushstring(L, e->s);
	return 3;
}

static int tlua_push_damage(lua_State *L, void *ud)
{
	tlua_svev_t *e = (tlua_svev_t *)ud;
	lua_pushinteger(L, e->a);
	lua_pushinteger(L, e->b);
	lua_pushnumber(L, e->n);
	lua_pushstring(L, e->s);
	return 4;
}

static int tlua_push_chat(lua_State *L, void *ud)
{
	tlua_svev_t *e = (tlua_svev_t *)ud;
	lua_pushinteger(L, e->a);
	lua_pushstring(L, e->s);
	lua_pushboolean(L, e->n != 0);
	return 3;
}

static int tlua_push_as(lua_State *L, void *ud)
{
	tlua_svev_t *e = (tlua_svev_t *)ud;
	lua_pushinteger(L, e->a);
	lua_pushstring(L, e->s);
	return 2;
}

static int tlua_push_cmd(lua_State *L, void *ud)
{
	tlua_svev_t *e = (tlua_svev_t *)ud;
	lua_pushinteger(L, e->a);
	lua_pushstring(L, e->s2);
	lua_pushstring(L, e->s);
	return 3;
}

static int tlua_push_s(lua_State *L, void *ud)
{
	lua_pushstring(L, ((tlua_svev_t *)ud)->s);
	return 1;
}

static int tlua_push_n(lua_State *L, void *ud)
{
	lua_pushnumber(L, ((tlua_svev_t *)ud)->n);
	return 1;
}

static void tlua_result_damage(lua_State *L, tlua_mod_t *m, void *ud)
{
	tlua_svev_t *e = (tlua_svev_t *)ud;
	(void)m;
	if (lua_type(L, -1) == LUA_TNUMBER)
	{
		double d = lua_tonumber(L, -1);
		if (d == d) // not NaN
			e->n = bound(0, d, 100000);
	}
}

static void tlua_result_false_blocks(lua_State *L, tlua_mod_t *m, void *ud)
{
	(void)m;
	if (lua_type(L, -1) == LUA_TBOOLEAN && !lua_toboolean(L, -1))
		((tlua_svev_t *)ud)->block = true;
}

static void tlua_result_true_handles(lua_State *L, tlua_mod_t *m, void *ud)
{
	(void)m;
	if (lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1))
		((tlua_svev_t *)ud)->block = true;
}

static qbool tlua_sv_ready(void)
{
	if (svs.threaded || !sv.active || !tlua_server.enable || !tlua_server.enable->integer)
	{
		if (tlua_server.loaded)
			TLua_UnloadAll(&tlua_server, "sv_lua_enable 0");
		return false;
	}
	if (!tlua_server.loaded)
		TLua_LoadAll(&tlua_server);
	return true;
}

// SV_StartFrame (QC): the map event on a new map, then the frame event
static void tlua_sv_frame(void)
{
	static double lasttime;
	static char lastmap[MAX_QPATH];
	tlua_svev_t e;
	if (!tlua_sv_ready())
		return;
	memset(&e, 0, sizeof(e));
	if (strcmp(sv.worldbasename, lastmap) || sv.time < lasttime)
	{
		dp_strlcpy(lastmap, sv.worldbasename, sizeof(lastmap));
		lasttime = sv.time;
		e.s = lastmap;
		TLua_Fire(&tlua_server, "map", tlua_push_s, NULL, &e);
		return;
	}
	if (sv.time > lasttime)
	{
		e.n = sv.time - lasttime;
		lasttime = sv.time;
		TLua_Fire(&tlua_server, "frame", tlua_push_n, NULL, &e);
	}
}

static float tlua_sv_event(const char *ev, int a, int b, float n, const char *s)
{
	tlua_svev_t e;
	char name[64];
	if (!strcmp(ev, "frame"))
	{
		tlua_sv_frame();
		return 0;
	}
	if (!tlua_sv_ready() || !TLua_AnyHandler(&tlua_server, ev))
		return !strcmp(ev, "damage") ? n : 0;
	memset(&e, 0, sizeof(e));
	e.a = a;
	e.b = b;
	e.n = n;
	e.s = s;
	if (!strcmp(ev, "connect") || !strcmp(ev, "disconnect") || !strcmp(ev, "spawn"))
		TLua_Fire(&tlua_server, ev, tlua_push_a, NULL, &e);
	else if (!strcmp(ev, "death"))
		TLua_Fire(&tlua_server, ev, tlua_push_death, NULL, &e);
	else if (!strcmp(ev, "damage"))
	{
		TLua_Fire(&tlua_server, ev, tlua_push_damage, tlua_result_damage, &e);
		return (float)e.n;
	}
	else if (!strcmp(ev, "chat"))
		TLua_Fire(&tlua_server, ev, tlua_push_chat, tlua_result_false_blocks, &e);
	else if (!strcmp(ev, "pickup"))
		TLua_Fire(&tlua_server, ev, tlua_push_as, tlua_result_false_blocks, &e);
	else if (!strcmp(ev, "clientcmd"))
	{
		// s is the whole command line: split off the command name
		size_t k = 0;
		const char *p = s;
		while (*p == ' ')
			p++;
		while (*p && *p != ' ' && k + 1 < sizeof(name))
			name[k++] = *p++;
		name[k] = 0;
		while (*p == ' ')
			p++;
		e.s2 = name;
		e.s = p;
		TLua_Fire(&tlua_server, ev, tlua_push_cmd, tlua_result_true_handles, &e);
	}
	else if (!strcmp(ev, "matchend"))
		TLua_Fire(&tlua_server, ev, NULL, NULL, &e);
	return e.block ? 1 : 0;
}

static void VM_SV_touch_lua_event(prvm_prog_t *prog)
{
	int a = PRVM_G_EDICTNUM(OFS_PARM1);
	int b = PRVM_G_EDICTNUM(OFS_PARM2);
	VM_SAFEPARMCOUNT(5, VM_SV_touch_lua_event);
	if (a < 1 || a > svs.maxclients)
		a = 0;
	if (b < 1 || b > svs.maxclients)
		b = 0;
	PRVM_G_FLOAT(OFS_RETURN) = tlua_sv_event(PRVM_G_STRING(OFS_PARM0), a, b, PRVM_G_FLOAT(OFS_PARM3), PRVM_G_STRING(OFS_PARM4));
}

static prvm_builtin_t tlua_sv_builtins[TLUA_SV_NUMBUILTINS];

void TouchLua_SV_InstallBuiltins(prvm_prog_t *prog)
{
	int i;
	TLua_InitOnce();
	if (prog->numbuiltins >= TLUA_SV_BUILTIN_EVENT)
	{
		Con_Printf(CON_WARN "Lua: the server VM has %d builtins, #%d is taken; server plugins get no game events\n", prog->numbuiltins, TLUA_SV_BUILTIN_EVENT);
		return;
	}
	for (i = 0; i < TLUA_SV_NUMBUILTINS; i++)
		tlua_sv_builtins[i] = i < prog->numbuiltins ? prog->builtins[i] : NULL;
	tlua_sv_builtins[TLUA_SV_BUILTIN_EVENT] = VM_SV_touch_lua_event;
	prog->builtins = tlua_sv_builtins;
	prog->numbuiltins = TLUA_SV_NUMBUILTINS;
}

void TLua_SV_CheckStopped(void)
{
	if (tlua_server.loaded && (!sv.active || svs.threaded))
		TLua_UnloadAll(&tlua_server, "server stopped");
}
