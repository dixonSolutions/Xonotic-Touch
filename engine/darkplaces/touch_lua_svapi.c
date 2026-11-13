/*
Godnotic: API level 3 for server mods (docs/LUA_API.md): entity handles,
players as entities, the world, the match, bots, bans, chat commands.

Entity handles are userdata holding (entity number, the time the slot was
last allocated, the player's connect time for a player slot, the program's
start time). Every use checks all of them against the live entity: an entity
that was removed, a slot reused by another entity or another player, or a new
map all make the handle stale; h:valid() says so, and any other use is an
error ("stale entity handle"). A handle never holds a pointer.

Reading h.<field> reads any QC field by name (numbers, vectors as {x,y,z},
strings, entities as handles); a few names are aliases (armor, shells,
bullets, rockets, cells, fuel, name). Writing needs the Game rules tier and
is limited to a list of plain fields (health, armor and ammo, origin,
velocity, angles, gravity, scale, alpha, colormod, ...), plus a few more on
entities the mod spawned itself.

What changes the game goes through the game's own code where the game has
its own way: giving and taking items (GiveItems), respawning, teams, votes,
removing an entity (server/touch_lua.qc, touch_lua_api). Kicks, bans and
map changes are queued as the console commands the server admin would type
and run between frames.

Re-entrancy: game code an API call runs (an item spawning, a player moving
team) may fire hooks; while a mod's code runs, no hook or event is delivered
to any mod (touch_lua_hooks.c), so a mod never runs inside another mod.
*/

#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua_api_gen.h"

static char tlua_key_entmt;

typedef struct tlua_ent_s
{
	int num;
	double alloc;    // the slot's freetime when the handle was made (set at alloc and free)
	double conn;     // a player slot: the client's connect time
	double start;    // the program's start time (a new map is a new program)
} tlua_ent_t;

// ---------------------------------------------------------------------------
// The QC helper (server/touch_lua.qc: touch_lua_api) for actions the game
// code has its own way of doing

#define TLUA_QC_GIVE 1
#define TLUA_QC_RESPAWN 2
#define TLUA_QC_TEAM 3
#define TLUA_QC_TELEPORT 4
#define TLUA_QC_REMOVE 5
#define TLUA_QC_VOTE 6
#define TLUA_QC_ANGLES 7

static float tlua_qc(lua_State *L, int op, prvm_edict_t *e, float f, const vec3_t v, const char *s)
{
	prvm_prog_t *prog = SVVM_prog;
	mfunction_t *fn;
	prvm_int_t self, other;
	float time;
	float r;
	if (!sv.active || !prog->loaded || !(fn = PRVM_ED_FindFunction(prog, "touch_lua_api")))
	{
		if (L)
			luaL_error(L, "this game's server code has no touch_lua_api (an older build)");
		return 0;
	}
	self = PRVM_serverglobaledict(self);
	other = PRVM_serverglobaledict(other);
	time = PRVM_serverglobalfloat(time);
	PRVM_serverglobalfloat(time) = sv.time;
	PRVM_G_FLOAT(OFS_PARM0) = op;
	PRVM_G_INT(OFS_PARM1) = e ? PRVM_EDICT_TO_PROG(e) : 0;
	PRVM_G_FLOAT(OFS_PARM2) = f;
	if (v)
		VectorCopy(v, PRVM_G_VECTOR(OFS_PARM3));
	else
		VectorClear(PRVM_G_VECTOR(OFS_PARM3));
	PRVM_G_INT(OFS_PARM4) = PRVM_SetTempString(prog, s ? s : "", strlen(s ? s : ""));
	prog->argc = 5;
	prog->ExecuteProgram(prog, fn - prog->functions, "touch_lua_api");
	r = PRVM_G_FLOAT(OFS_RETURN);
	PRVM_serverglobaledict(self) = self;
	PRVM_serverglobaledict(other) = other;
	PRVM_serverglobalfloat(time) = time;
	return r;
}

// ---------------------------------------------------------------------------
// Handles

static prvm_edict_t *tlua_ent_live(const tlua_ent_t *h)
{
	prvm_prog_t *prog = SVVM_prog;
	prvm_edict_t *ed;
	if (!sv.active || !prog->loaded || h->num < 1 || h->num >= prog->num_edicts || prog->starttime != h->start)
		return NULL;
	ed = PRVM_EDICT_NUM(h->num);
	if (ed->free || ed->freetime != h->alloc)
		return NULL;
	if (h->num <= svs.maxclients)
	{
		client_t *c = svs.clients + h->num - 1;
		if (!c->active || c->connecttime != h->conn)
			return NULL;
	}
	return ed;
}

static void tlua_ent_entmt(lua_State *L);

// pushes a handle to entity n, or nil when there is none
static void tlua_ent_push(lua_State *L, int n)
{
	prvm_prog_t *prog = SVVM_prog;
	prvm_edict_t *ed;
	tlua_ent_t *h;
	if (!sv.active || !prog->loaded || n < 1 || n >= prog->num_edicts || (ed = PRVM_EDICT_NUM(n))->free
		|| (n <= svs.maxclients && !svs.clients[n - 1].active))
	{
		lua_pushnil(L);
		return;
	}
	h = (tlua_ent_t *)lua_newuserdatauv(L, sizeof(*h), 0);
	h->num = n;
	h->alloc = ed->freetime;
	h->conn = n <= svs.maxclients ? svs.clients[n - 1].connecttime : 0;
	h->start = prog->starttime;
	tlua_ent_entmt(L);
	lua_setmetatable(L, -2);
}

static tlua_ent_t *tlua_ent_ud(lua_State *L, int idx)
{
	tlua_ent_t *h = NULL;
	if (lua_type(L, idx) == LUA_TUSERDATA && lua_getmetatable(L, idx))
	{
		lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_entmt);
		if (lua_rawequal(L, -1, -2))
			h = (tlua_ent_t *)lua_touserdata(L, idx);
		lua_pop(L, 2);
	}
	return h;
}

// a live entity from a handle or a number (player id, hook entity number)
static prvm_edict_t *tlua_ent_arg(lua_State *L, int idx, int *num)
{
	tlua_ent_t *h = tlua_ent_ud(L, idx);
	prvm_edict_t *ed;
	if (num)
		*num = 0;
	if (h)
	{
		if (!(ed = tlua_ent_live(h)))
			luaL_error(L, "stale entity handle: entity %d was removed or reused (check h:valid())", h->num);
		if (num)
			*num = h->num;
		return ed;
	}
	if (lua_type(L, idx) == LUA_TNUMBER)
	{
		prvm_prog_t *prog = SVVM_prog;
		lua_Integer n = lua_tointeger(L, idx);
		if (!sv.active || !prog->loaded || n < 1 || n >= prog->num_edicts || PRVM_EDICT_NUM(n)->free || (n <= svs.maxclients && !svs.clients[n - 1].active))
			luaL_error(L, "no entity %d", (int)n);
		if (num)
			*num = (int)n;
		return PRVM_EDICT_NUM(n);
	}
	luaL_argerror(L, idx, "an entity handle or number");
	return NULL;
}

static void tlua_pushvec3(lua_State *L, const float *v)
{
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, v[0]);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, v[1]);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, v[2]);
	lua_setfield(L, -2, "z");
}

static void tlua_checkvec3(lua_State *L, int idx, vec3_t out)
{
	static const char *const k[3] = {"x", "y", "z"};
	int i;
	luaL_checktype(L, idx, LUA_TTABLE);
	for (i = 0; i < 3; i++)
	{
		double d;
		if (lua_getfield(L, idx, k[i]) != LUA_TNUMBER)
		{
			lua_pop(L, 1);
			lua_rawgeti(L, idx, i + 1);
		}
		d = luaL_optnumber(L, -1, 0);
		if (!(d == d) || d > 1e6 || d < -1e6)
			luaL_error(L, "a vector component is not a usable number");
		out[i] = (float)d;
		lua_pop(L, 1);
	}
}

static const char *tlua_alias(const char *k)
{
	static const char *const al[][2] = {
		{"armor", "armorvalue"}, {"shells", "ammo_shells"}, {"bullets", "ammo_nails"}, {"nails", "ammo_nails"},
		{"rockets", "ammo_rockets"}, {"cells", "ammo_cells"}, {"fuel", "ammo_fuel"}, {"name", "netname"}, {NULL, NULL}};
	int i;
	for (i = 0; al[i][0]; i++)
		if (!strcmp(k, al[i][0]))
			return al[i][1];
	return k;
}

static qbool tlua_field_writable(const char *f, qbool own)
{
	static const char *const any[] = {"health", "armorvalue", "ammo_shells", "ammo_nails", "ammo_rockets", "ammo_cells", "ammo_fuel",
		"origin", "velocity", "angles", "gravity", "scale", "alpha", "colormod", "glowmod", "effects", "max_health", "avelocity", NULL};
	static const char *const ownonly[] = {"target", "targetname", "message", "netname", "wait", "speed", "delay", "count", "cnt", "dmg", "nextthink", NULL};
	int i;
	for (i = 0; any[i]; i++)
		if (!strcmp(f, any[i]))
			return true;
	for (i = 0; own && ownonly[i]; i++)
		if (!strcmp(f, ownonly[i]))
			return true;
	return false;
}

// spawned by this mod and still alive
#define TLUA_SPAWNS 64
typedef struct tlua_spawned_s { qbool used; tlua_realm_t *realm; char mod[TLUA_NAME_LEN + 1]; tlua_ent_t h; } tlua_spawned_t;
static tlua_spawned_t tlua_spawned[TLUA_SPAWNS * 4];

static tlua_spawned_t *tlua_spawned_find(tlua_mod_t *m, const tlua_ent_t *h)
{
	int i;
	for (i = 0; i < TLUA_SPAWNS * 4; i++)
	{
		tlua_spawned_t *s = &tlua_spawned[i];
		if (s->used && s->realm == m->realm && !strcmp(s->mod, m->name) && s->h.num == h->num && s->h.alloc == h->alloc && s->h.start == h->start)
			return s;
	}
	return NULL;
}

static int tlua_ent_index(lua_State *L);
static int tlua_ent_newindex(lua_State *L);

static int tlua_ent_eq(lua_State *L)
{
	tlua_ent_t *a = tlua_ent_ud(L, 1), *b = tlua_ent_ud(L, 2);
	lua_pushboolean(L, a && b && a->num == b->num && a->alloc == b->alloc && a->conn == b->conn && a->start == b->start);
	return 1;
}

static int tlua_ent_tostring(lua_State *L)
{
	tlua_ent_t *h = tlua_ent_ud(L, 1);
	prvm_edict_t *ed = h ? tlua_ent_live(h) : NULL;
	if (!ed)
		lua_pushfstring(L, "entity %d (stale)", h ? h->num : 0);
	else
	{
		prvm_prog_t *prog = SVVM_prog;
		lua_pushfstring(L, "entity %d (%s)", h->num, PRVM_GetString(prog, PRVM_serveredictstring(ed, classname)));
	}
	return 1;
}

static void tlua_ent_entmt(lua_State *L)
{
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_entmt) == LUA_TTABLE)
		return;
	lua_pop(L, 1);
	lua_createtable(L, 0, 5);
	TLua_SetField(L, "__index", tlua_ent_index);
	TLua_SetField(L, "__newindex", tlua_ent_newindex);
	TLua_SetField(L, "__eq", tlua_ent_eq);
	TLua_SetField(L, "__tostring", tlua_ent_tostring);
	lua_pushliteral(L, "entity");
	lua_setfield(L, -2, "__metatable");
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_entmt);
}

// ---------------------------------------------------------------------------
// Methods

static prvm_edict_t *tlua_self_player(lua_State *L, int *num)
{
	int n;
	prvm_edict_t *ed = tlua_ent_arg(L, 1, &n);
	if (n < 1 || n > svs.maxclients)
		luaL_error(L, "not a player");
	if (num)
		*num = n;
	return ed;
}

static int tlua_m_valid(lua_State *L)
{
	tlua_ent_t *h = tlua_ent_ud(L, 1);
	lua_pushboolean(L, h && tlua_ent_live(h));
	return 1;
}

static int tlua_m_num(lua_State *L)
{
	int n;
	tlua_ent_arg(L, 1, &n);
	lua_pushinteger(L, n);
	return 1;
}

static int tlua_m_is_player(lua_State *L)
{
	int n;
	tlua_ent_arg(L, 1, &n);
	lua_pushboolean(L, n >= 1 && n <= svs.maxclients);
	return 1;
}

static int tlua_m_is_bot(lua_State *L)
{
	int n;
	tlua_ent_arg(L, 1, &n);
	lua_pushboolean(L, n >= 1 && n <= svs.maxclients && !svs.clients[n - 1].netconnection);
	return 1;
}

static int tlua_m_get(lua_State *L)
{
	lua_settop(L, 2);
	return tlua_ent_index(L);
}

static int tlua_m_set(lua_State *L)
{
	lua_settop(L, 3);
	tlua_ent_newindex(L);
	lua_settop(L, 1);
	return 1;
}

static int tlua_m_remove(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	tlua_ent_t *h = tlua_ent_ud(L, 1);
	tlua_spawned_t *s;
	prvm_edict_t *ed;
	TLua_Require(L, TLUA_TIER_GAMERULES);
	if (!h)
		return luaL_argerror(L, 1, "an entity handle");
	if (!(ed = tlua_ent_live(h)))
		return luaL_error(L, "stale entity handle: entity %d was removed or reused", h->num);
	if (!(s = tlua_spawned_find(m, h)))
		return luaL_error(L, "h:remove(): only entities this mod spawned");
	tlua_qc(L, TLUA_QC_REMOVE, ed, 0, NULL, NULL);
	s->used = false;
	return 0;
}

// p:tell / p:centerprint go through xt.tell / xt.centerprint (their limits)
static int tlua_m_forward(lua_State *L, const char *fn)
{
	int n;
	tlua_self_player(L, &n);
	lua_getglobal(L, "xt");
	lua_getfield(L, -1, fn);
	lua_pushinteger(L, n);
	lua_pushvalue(L, 2);
	lua_call(L, 2, 0);
	return 0;
}
static int tlua_m_tell2(lua_State *L) { return tlua_m_forward(L, "tell"); }
static int tlua_m_centerprint(lua_State *L) { return tlua_m_forward(L, "centerprint"); }

static qbool tlua_word_ok(const char *s)
{
	if (!*s || strlen(s) > 32)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '_')
			return false;
	return true;
}

// p:give(item [, amount]), p:take(item [, amount]): GiveItems words
// ("rockets", "health", "armor", "devastator", "allweapons", ...)
static int tlua_m_give_take(lua_State *L, qbool give)
{
	prvm_edict_t *ed;
	const char *item = luaL_checkstring(L, 2);
	char line[128];
	TLua_Require(L, TLUA_TIER_GAMERULES);
	ed = tlua_self_player(L, NULL);
	if (!tlua_word_ok(item))
		return luaL_error(L, "an item word (rockets, health, devastator, ...)");
	if (lua_isnoneornil(L, 3))
		dpsnprintf(line, sizeof(line), give ? "%s" : "no %s", item);
	else
	{
		double amount = luaL_checknumber(L, 3);
		if (!(amount >= 0) || amount > 100000)
			return luaL_error(L, "an amount from 0 to 100000");
		dpsnprintf(line, sizeof(line), "%s %g %s", give ? "plus" : "minus", amount, item);
	}
	lua_pushboolean(L, tlua_qc(L, TLUA_QC_GIVE, ed, 0, NULL, line) != 0);
	return 1;
}
static int tlua_m_give(lua_State *L) { return tlua_m_give_take(L, true); }
static int tlua_m_take(lua_State *L) { return tlua_m_give_take(L, false); }

static int tlua_m_teleport(lua_State *L)
{
	prvm_edict_t *ed;
	vec3_t v, a;
	TLua_Require(L, TLUA_TIER_GAMERULES);
	ed = tlua_ent_arg(L, 1, NULL);
	tlua_checkvec3(L, 2, v);
	tlua_qc(L, TLUA_QC_TELEPORT, ed, 0, v, NULL);
	if (lua_type(L, 3) == LUA_TTABLE)
	{
		tlua_checkvec3(L, 3, a);
		tlua_qc(L, TLUA_QC_ANGLES, ed, 0, a, NULL);
	}
	return 0;
}

static int tlua_m_respawn(lua_State *L)
{
	prvm_edict_t *ed;
	TLua_Require(L, TLUA_TIER_GAMERULES);
	ed = tlua_self_player(L, NULL);
	lua_pushboolean(L, tlua_qc(L, TLUA_QC_RESPAWN, ed, 0, NULL, NULL) != 0);
	return 1;
}

static int tlua_m_setteam(lua_State *L)
{
	prvm_edict_t *ed;
	lua_Integer t = luaL_checkinteger(L, 2);
	TLua_Require(L, TLUA_TIER_GAMERULES);
	ed = tlua_self_player(L, NULL);
	if (t < 1 || t > 4)
		return luaL_error(L, "p:setteam(1..4): red, blue, yellow, pink");
	lua_pushboolean(L, tlua_qc(L, TLUA_QC_TEAM, ed, (float)t, NULL, NULL) != 0);
	return 1;
}

static void tlua_reason(lua_State *L, int idx, char *out, size_t size)
{
	char *p;
	TLua_CleanText(luaL_optstring(L, idx, ""), out, size, false);
	for (p = out; *p; p++)
		if (*p == '"' || *p == ';' || *p == '$' || *p == '\\')
			*p = ' ';
}

static int tlua_m_kick(lua_State *L)
{
	int n;
	char reason[128], line[200];
	TLua_Require(L, TLUA_TIER_GAMERULES);
	tlua_self_player(L, &n);
	tlua_reason(L, 2, reason, sizeof(reason));
	dpsnprintf(line, sizeof(line), "kick # %d %s\n", n, reason);
	Cbuf_AddText(cmd_local, line);
	return 0;
}

static int tlua_ban(lua_State *L, int n, double minutes, int reasonidx)
{
	char reason[128], line[256];
	tlua_reason(L, reasonidx, reason, sizeof(reason));
	if (!(minutes > 0) || minutes > 60 * 24 * 365)
		return luaL_error(L, "minutes from 1 to a year");
	dpsnprintf(line, sizeof(line), "sv_cmd kickban #%d %d 4 \"%s\"\n", n, (int)(minutes * 60), reason[0] ? reason : "banned");
	Cbuf_AddText(cmd_local, line);
	return 0;
}

static int tlua_m_ban(lua_State *L)
{
	int n;
	TLua_Require(L, TLUA_TIER_GAMERULES);
	tlua_self_player(L, &n);
	return tlua_ban(L, n, luaL_checknumber(L, 2), 3);
}

static const luaL_Reg tlua_ent_methods[] = {
	{"valid", tlua_m_valid}, {"num", tlua_m_num}, {"is_player", tlua_m_is_player}, {"is_bot", tlua_m_is_bot},
	{"get", tlua_m_get}, {"set", tlua_m_set}, {"remove", tlua_m_remove},
	{"tell", tlua_m_tell2}, {"centerprint", tlua_m_centerprint}, {"give", tlua_m_give}, {"take", tlua_m_take},
	{"teleport", tlua_m_teleport}, {"respawn", tlua_m_respawn}, {"setteam", tlua_m_setteam}, {"kick", tlua_m_kick}, {"ban", tlua_m_ban},
	{NULL, NULL}
};

static int tlua_ent_index(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	const char *k = luaL_checkstring(L, 2);
	const luaL_Reg *r;
	prvm_edict_t *ed;
	mdef_t *d;
	const char *f;
	int n;
	for (r = tlua_ent_methods; r->name; r++)
		if (!strcmp(r->name, k))
		{
			lua_pushcfunction(L, r->func);
			return 1;
		}
	ed = tlua_ent_arg(L, 1, &n);
	if (!strcmp(k, "alive"))
	{
		lua_pushboolean(L, PRVM_serveredictfloat(ed, deadflag) == 0 && PRVM_serveredictfloat(ed, health) > 0);
		return 1;
	}
	if (!strcmp(k, "id"))
	{
		lua_pushinteger(L, n);
		return 1;
	}
	if (!strcmp(k, "bot"))
	{
		lua_pushboolean(L, n >= 1 && n <= svs.maxclients && !svs.clients[n - 1].netconnection);
		return 1;
	}
	f = tlua_alias(k);
	if (!(d = PRVM_ED_FindField(prog, f)))
		return 0;
	switch (d->type & ~DEF_SAVEGLOBAL)
	{
	case ev_float:
		lua_pushnumber(L, PRVM_E_FLOAT(ed, d->ofs));
		return 1;
	case 8: // ev_integer (gmqcc's int)
		lua_pushinteger(L, PRVM_E_INT(ed, d->ofs));
		return 1;
	case ev_vector:
		tlua_pushvec3(L, &PRVM_E_FLOAT(ed, d->ofs));
		return 1;
	case ev_string:
	{
		char clean[1024];
		lua_pushstring(L, TLua_CleanText(PRVM_GetString(prog, PRVM_E_INT(ed, d->ofs)), clean, sizeof(clean), false));
		return 1;
	}
	case ev_entity:
		tlua_ent_push(L, PRVM_NUM_FOR_EDICT(PRVM_PROG_TO_EDICT(PRVM_E_INT(ed, d->ofs))));
		return 1;
	default:
		return 0; // functions, fields, pointers: not for mods
	}
}

static int tlua_ent_newindex(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	tlua_mod_t *m = TLua_Self(L);
	const char *k = luaL_checkstring(L, 2);
	tlua_ent_t *h = tlua_ent_ud(L, 1);
	prvm_edict_t *ed;
	mdef_t *d;
	const char *f;
	int n;
	TLua_Require(L, TLUA_TIER_GAMERULES);
	ed = tlua_ent_arg(L, 1, &n);
	f = tlua_alias(k);
	if (!tlua_field_writable(f, h && tlua_spawned_find(m, h)))
		return luaL_error(L, "entity field '%s' cannot be written by mods (docs/LUA_API.md, Entity handles)", k);
	if (!(d = PRVM_ED_FindField(prog, f)))
		return luaL_error(L, "no field '%s' in this game", k);
	switch (d->type & ~DEF_SAVEGLOBAL)
	{
	case ev_float:
	{
		double v = luaL_checknumber(L, 3);
		if (!(v == v) || v > 1e7 || v < -1e7)
			return luaL_error(L, "%s: not a usable number", k);
		PRVM_E_FLOAT(ed, d->ofs) = (float)v;
		break;
	}
	case ev_vector:
	{
		vec3_t v;
		tlua_checkvec3(L, 3, v);
		if (!strcmp(f, "origin"))
		{
			tlua_qc(L, TLUA_QC_TELEPORT, ed, 0, v, NULL); // setorigin: relinked
			return 0;
		}
		if (!strcmp(f, "angles") && n >= 1 && n <= svs.maxclients)
		{
			tlua_qc(L, TLUA_QC_ANGLES, ed, 0, v, NULL); // a player's view turns too
			return 0;
		}
		VectorCopy(v, &PRVM_E_FLOAT(ed, d->ofs));
		break;
	}
	case ev_string:
	{
		char clean[256];
		TLua_CleanText(luaL_checkstring(L, 3), clean, sizeof(clean), false);
		PRVM_ED_ParseEpair(prog, ed, d, clean, false); // a string the program owns, as a map's
		break;
	}
	default:
		return luaL_error(L, "%s: only number, vector and string fields", k);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// xt.ent, xt.ents.*

int TLuaAPI_sv_ent(lua_State *L)
{
	if (tlua_ent_ud(L, 1))
	{
		tlua_ent_t *h = tlua_ent_ud(L, 1);
		if (tlua_ent_live(h))
			lua_pushvalue(L, 1);
		else
			lua_pushnil(L);
		return 1;
	}
	tlua_ent_push(L, (int)luaL_checkinteger(L, 1));
	return 1;
}

int TLuaAPI_sv_ents_find(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	char cname[128] = "", field[64] = "", value[256] = "";
	vec3_t near;
	double radius = -1;
	int limit = 256, i, n = 0;
	mdef_t *fd = NULL;
	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_getfield(L, 1, "class") == LUA_TSTRING)
		dp_strlcpy(cname, lua_tostring(L, -1), sizeof(cname));
	lua_pop(L, 1);
	if (lua_getfield(L, 1, "field") == LUA_TSTRING)
		dp_strlcpy(field, tlua_alias(lua_tostring(L, -1)), sizeof(field));
	lua_pop(L, 1);
	if (lua_getfield(L, 1, "value") != LUA_TNIL)
		dp_strlcpy(value, luaL_tolstring(L, -1, NULL), sizeof(value)), lua_pop(L, 1);
	lua_pop(L, 1);
	if (lua_getfield(L, 1, "near") == LUA_TTABLE)
	{
		tlua_checkvec3(L, lua_gettop(L), near);
		lua_getfield(L, 1, "radius");
		radius = luaL_optnumber(L, -1, 1024);
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	if (lua_getfield(L, 1, "limit") == LUA_TNUMBER)
		limit = (int)bound(1, lua_tointeger(L, -1), 256);
	lua_pop(L, 1);
	if (field[0] && !(fd = PRVM_ED_FindField(prog, field)))
		return luaL_error(L, "xt.ents.find: no field '%s'", field);
	lua_newtable(L);
	if (!sv.active || !prog->loaded)
		return 1;
	for (i = 1; i < prog->num_edicts && n < limit; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		if (ed->free || (i <= svs.maxclients && !svs.clients[i - 1].active))
			continue;
		if ((i & 63) == 0)
			TouchLua_WorkStep(L);
		if (cname[0] && strcmp(PRVM_GetString(prog, PRVM_serveredictstring(ed, classname)), cname))
			continue;
		if (fd)
		{
			char v[256];
			switch (fd->type & ~DEF_SAVEGLOBAL)
			{
			case ev_float: dpsnprintf(v, sizeof(v), "%g", PRVM_E_FLOAT(ed, fd->ofs)); break;
			case ev_string: dp_strlcpy(v, PRVM_GetString(prog, PRVM_E_INT(ed, fd->ofs)), sizeof(v)); break;
			default: v[0] = 0; break;
			}
			if (strcmp(v, value) && !(fd->type == ev_float && atof(value) == atof(v)))
				continue;
		}
		if (radius >= 0)
		{
			float *o = PRVM_serveredictvector(ed, origin);
			vec3_t dv;
			VectorSubtract(o, near, dv);
			if (DotProduct(dv, dv) > radius * radius)
				continue;
		}
		tlua_ent_push(L, i);
		lua_rawseti(L, -2, ++n);
	}
	return 1;
}

static qbool tlua_spawn_class_ok(const char *c)
{
	static const char *const ok[] = {"target_position", "info_teleport_destination", "info_notnull", "func_pointparticles", "trigger_push", "trigger_teleport", NULL};
	int i;
	if (!tlua_word_ok(c))
		return false;
	if (!strncmp(c, "item_", 5) || !strncmp(c, "weapon_", 7))
		return true;
	for (i = 0; ok[i]; i++)
		if (!strcmp(c, ok[i]))
			return true;
	return false;
}

int TLuaAPI_sv_ents_spawn(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	tlua_mod_t *m = TLua_Self(L);
	const char *cname = luaL_checkstring(L, 1);
	static double window;
	static int count;
	char text[2048], kv[300];
	const char *data;
	prvm_edict_t *ed;
	int i, mine = 0, num;
	tlua_spawned_t *slot = NULL;
	if (!sv.active || !prog->loaded)
		return luaL_error(L, "xt.ents.spawn: no map is running");
	if (!tlua_spawn_class_ok(cname))
		return luaL_error(L, "xt.ents.spawn: '%s' is not a class mods may spawn (item_*, weapon_*, target_position, info_teleport_destination, info_notnull, func_pointparticles, trigger_push, trigger_teleport)", cname);
	for (i = 0; i < TLUA_SPAWNS * 4; i++)
	{
		tlua_spawned_t *s = &tlua_spawned[i];
		if (s->used && !tlua_ent_live(&s->h))
			s->used = false;
		if (s->used && s->realm == m->realm && !strcmp(s->mod, m->name))
			mine++;
		else if (!s->used && !slot)
			slot = s;
	}
	if (mine >= TLUA_SPAWNS || !slot)
		return luaL_error(L, "xt.ents.spawn: %d live entities per mod", TLUA_SPAWNS);
	if (host.realtime - window > 1)
	{
		window = host.realtime;
		count = 0;
	}
	if (++count > 16)
		return luaL_error(L, "xt.ents.spawn: 16 a second");
	// the entity as the map would declare it: "key" "value" pairs
	dpsnprintf(text, sizeof(text), "\"classname\" \"%s\"\n", cname);
	if (lua_type(L, 2) == LUA_TTABLE)
	{
		lua_pushnil(L);
		while (lua_next(L, 2))
		{
			const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
			char val[256];
			if (!key || !tlua_word_ok(key) || !strcmp(key, "classname") || !strcmp(key, "model") || !strcmp(key, "noise") || !strncmp(key, "noise", 5))
				return luaL_error(L, "xt.ents.spawn: field '%s' is not allowed", key ? key : "?");
			if (lua_type(L, -1) == LUA_TTABLE)
			{
				vec3_t v;
				tlua_checkvec3(L, lua_gettop(L), v);
				dpsnprintf(val, sizeof(val), "%g %g %g", v[0], v[1], v[2]);
			}
			else if (lua_type(L, -1) == LUA_TNUMBER || lua_type(L, -1) == LUA_TSTRING)
			{
				char *p;
				TLua_CleanText(lua_tostring(L, -1), val, sizeof(val), false);
				for (p = val; *p; p++)
					if (*p == '"' || *p == '\\' || *p == '{' || *p == '}')
						*p = ' ';
			}
			else if (lua_type(L, -1) == LUA_TBOOLEAN)
				dp_strlcpy(val, lua_toboolean(L, -1) ? "1" : "0", sizeof(val));
			else
				return luaL_error(L, "xt.ents.spawn: field '%s' is not a number, string or vector", key);
			dpsnprintf(kv, sizeof(kv), "\"%s\" \"%s\"\n", key, val);
			if (strlen(text) + strlen(kv) + 4 >= sizeof(text))
				return luaL_error(L, "xt.ents.spawn: too many fields");
			dp_strlcat(text, kv, sizeof(text));
			lua_pop(L, 1);
		}
	}
	dp_strlcat(text, "}\n", sizeof(text));
	ed = PRVM_ED_Alloc(prog);
	num = PRVM_NUM_FOR_EDICT(ed);
	data = PRVM_ED_ParseEdict(prog, text, ed, false);
	(void)data;
	PRVM_ED_CallPrespawnFunction(prog, ed);
	if (!PRVM_ED_CallSpawnFunction(prog, ed, NULL, NULL))
	{
		lua_pushnil(L);
		return 1;
	}
	PRVM_ED_CallPostspawnFunction(prog, ed);
	if (ed->free)
	{
		lua_pushnil(L); // the game code removed it at once (a gametype or mutator that does not allow it)
		return 1;
	}
	tlua_ent_push(L, num);
	slot->used = true;
	slot->realm = m->realm;
	dp_strlcpy(slot->mod, m->name, sizeof(slot->mod));
	slot->h = *(tlua_ent_t *)lua_touserdata(L, -1);
	return 1;
}

// ---------------------------------------------------------------------------
// xt.world.*

int TLuaAPI_sv_world_trace(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	vec3_t a, b;
	trace_t tr;
	prvm_edict_t *ignore = NULL;
	if (!sv.active || !prog->loaded)
		return 0;
	tlua_checkvec3(L, 1, a);
	tlua_checkvec3(L, 2, b);
	if (!lua_isnoneornil(L, 3))
		ignore = tlua_ent_arg(L, 3, NULL);
	tr = SV_TraceLine(a, b, MOVE_NORMAL, ignore, SUPERCONTENTS_SOLID | SUPERCONTENTS_BODY, 0, 0, collision_extendmovelength.value);
	lua_createtable(L, 0, 6);
	lua_pushnumber(L, tr.fraction);
	lua_setfield(L, -2, "fraction");
	{
		vec3_t endpos;
		VectorCopy(tr.endpos, endpos);
		tlua_pushvec3(L, endpos);
	}
	lua_setfield(L, -2, "endpos");
	{
		vec3_t nrm;
		VectorCopy(tr.plane.normal, nrm);
		tlua_pushvec3(L, nrm);
	}
	lua_setfield(L, -2, "normal");
	lua_pushboolean(L, tr.startsolid);
	lua_setfield(L, -2, "startsolid");
	lua_pushboolean(L, tr.allsolid);
	lua_setfield(L, -2, "allsolid");
	if (tr.ent && tr.ent != prog->edicts)
	{
		tlua_ent_push(L, PRVM_NUM_FOR_EDICT((prvm_edict_t *)tr.ent));
		lua_setfield(L, -2, "ent");
	}
	return 1;
}

int TLuaAPI_sv_world_contents(lua_State *L)
{
	vec3_t p;
	int c;
	tlua_checkvec3(L, 1, p);
	if (!sv.active)
		return 0;
	c = SV_PointSuperContents(p);
	lua_createtable(L, 0, 5);
	lua_pushboolean(L, (c & SUPERCONTENTS_SOLID) != 0);
	lua_setfield(L, -2, "solid");
	lua_pushboolean(L, (c & SUPERCONTENTS_WATER) != 0);
	lua_setfield(L, -2, "water");
	lua_pushboolean(L, (c & SUPERCONTENTS_SLIME) != 0);
	lua_setfield(L, -2, "slime");
	lua_pushboolean(L, (c & SUPERCONTENTS_LAVA) != 0);
	lua_setfield(L, -2, "lava");
	lua_pushboolean(L, (c & SUPERCONTENTS_SKY) != 0);
	lua_setfield(L, -2, "sky");
	return 1;
}

static qbool tlua_rate(double *window, int *count, int max)
{
	if (host.realtime - *window > 1)
	{
		*window = host.realtime;
		*count = 0;
	}
	return ++*count <= max;
}

static qbool tlua_path_ok(const char *p, const char *prefix)
{
	if (strncmp(p, prefix, strlen(prefix)) || strstr(p, "..") || strlen(p) > 96)
		return false;
	for (; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("/_-.", *p))
			return false;
	return true;
}

int TLuaAPI_sv_world_sound(lua_State *L)
{
	static double window;
	static int count;
	vec3_t p;
	const char *sample = luaL_checkstring(L, 2);
	float vol = (float)bound(0, luaL_optnumber(L, 3, 1), 1), att = (float)bound(0, luaL_optnumber(L, 4, 1), 4);
	tlua_checkvec3(L, 1, p);
	if (!tlua_path_ok(sample, "") || !strchr(sample, '.'))
		return luaL_error(L, "xt.world.sound: a sound path of the game (weapons/rocket_impact.wav)");
	if (!sv.active || !tlua_rate(&window, &count, 8))
		return 0;
	SV_SoundIndex(sample, 1);
	SV_StartPointSound(p, sample, (int)(vol * 255), att, 1);
	lua_pushboolean(L, 1);
	return 1;
}

int TLuaAPI_sv_world_effect(lua_State *L)
{
	static double window;
	static int count;
	const char *name = luaL_checkstring(L, 1);
	vec3_t p;
	int idx, n = (int)bound(1, luaL_optinteger(L, 3, 1), 16);
	tlua_checkvec3(L, 2, p);
	if (!tlua_word_ok(name) || !sv.active || !tlua_rate(&window, &count, 16))
		return 0;
	idx = SV_ParticleEffectIndex(name);
	if (idx <= 0)
		return 0;
	MSG_WriteByte(&sv.datagram, svc_pointparticles);
	MSG_WriteShort(&sv.datagram, idx);
	MSG_WriteVector(&sv.datagram, p, sv.protocol);
	MSG_WriteVector(&sv.datagram, vec3_origin, sv.protocol);
	MSG_WriteShort(&sv.datagram, n);
	lua_pushboolean(L, 1);
	return 1;
}

int TLuaAPI_sv_world_gametype(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	mfunction_t *fn;
	if (!sv.active || !prog->loaded || !(fn = PRVM_ED_FindFunction(prog, "touch_lua_api_gametype")))
	{
		lua_pushstring(L, "");
		return 1;
	}
	prog->ExecuteProgram(prog, fn - prog->functions, "touch_lua_api_gametype");
	lua_pushstring(L, PRVM_G_STRING(OFS_RETURN));
	return 1;
}

// ---------------------------------------------------------------------------
// xt.match.*, xt.bots.*, xt.ban.*

static qbool tlua_mapname_ok(const char *s)
{
	if (!*s || strlen(s) > 64)
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-')
			return false;
	return true;
}

int TLuaAPI_sv_match_vote(lua_State *L)
{
	const char *c = luaL_checkstring(L, 1);
	const char *p;
	prvm_edict_t *by = NULL;
	if (strlen(c) > 200)
		return luaL_error(L, "xt.match.vote: too long");
	for (p = c; *p; p++)
		if ((unsigned char)*p < 32 || *p == ';' || *p == '"' || *p == '$' || *p == '\\')
			return luaL_error(L, "xt.match.vote: ; \" $ \\ and control characters are not allowed");
	if (!lua_isnoneornil(L, 2))
	{
		int n;
		by = tlua_ent_arg(L, 2, &n);
		if (n < 1 || n > svs.maxclients)
			return luaL_error(L, "xt.match.vote: by is a player");
	}
	tlua_qc(L, TLUA_QC_VOTE, by, 0, NULL, c);
	return 0;
}

static int tlua_queue(lua_State *L, const char *line)
{
	(void)L;
	Cbuf_AddText(cmd_local, line);
	return 0;
}

int TLuaAPI_sv_match_changemap(lua_State *L)
{
	const char *map = luaL_checkstring(L, 1);
	char line[128];
	if (!tlua_mapname_ok(map))
		return luaL_error(L, "xt.match.changemap: a map name");
	dpsnprintf(line, sizeof(line), "sv_cmd gotomap %s\n", map);
	return tlua_queue(L, line);
}

int TLuaAPI_sv_match_endmatch(lua_State *L) { return tlua_queue(L, "sv_cmd endmatch\n"); }
int TLuaAPI_sv_match_restart(lua_State *L) { return tlua_queue(L, "restart\n"); }

int TLuaAPI_sv_match_setcvar(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	const char *value = lua_type(L, 2) == LUA_TBOOLEAN ? (lua_toboolean(L, 2) ? "1" : "0") : luaL_checkstring(L, 2);
	cvar_t *v = Cvar_FindVar(&cvars_all, name, ~0);
	if (!m->realm->cvar_writable(name) || TLua_CvarSecret(name) || !TLua_CleanValue(value) || !v || (v->flags & (CF_PRIVATE | CF_READONLY)))
	{
		lua_pushboolean(L, 0);
		return 1;
	}
	Cvar_SetQuick(v, value);
	lua_pushboolean(L, 1);
	return 1;
}

static int tlua_bots(lua_State *L, int delta)
{
	cvar_t *bn = Cvar_FindVar(&cvars_all, "bot_number", ~0);
	int n = bn ? bn->integer : 0;
	char v[32];
	if (!bn)
		return luaL_error(L, "this game has no bot_number");
	n = bound(0, n + delta, svs.maxclients - 1);
	dpsnprintf(v, sizeof(v), "%d", n);
	Cvar_SetQuick(bn, v);
	lua_pushinteger(L, n);
	return 1;
}

int TLuaAPI_sv_bots_add(lua_State *L)
{
	int n = (int)bound(1, luaL_optinteger(L, 1, 1), 64);
	if (!lua_isnoneornil(L, 2))
	{
		char v[32];
		dpsnprintf(v, sizeof(v), "%d", (int)bound(0, luaL_checkinteger(L, 2), 10));
		Cvar_Set(&cvars_all, "skill", v);
	}
	return tlua_bots(L, n);
}

int TLuaAPI_sv_bots_remove(lua_State *L) { return tlua_bots(L, -(int)bound(1, luaL_optinteger(L, 1, 1), 64)); }

int TLuaAPI_sv_bots_skill(lua_State *L)
{
	if (!lua_isnoneornil(L, 1))
	{
		char v[32];
		TLua_Require(L, TLUA_TIER_GAMERULES);
		dpsnprintf(v, sizeof(v), "%d", (int)bound(0, luaL_checkinteger(L, 1), 10));
		Cvar_Set(&cvars_all, "skill", v);
	}
	lua_pushnumber(L, Cvar_VariableValue(&cvars_all, "skill", ~0));
	return 1;
}

int TLuaAPI_sv_ban_add(lua_State *L)
{
	int n;
	prvm_edict_t *ed = tlua_ent_arg(L, 1, &n);
	(void)ed;
	if (n < 1 || n > svs.maxclients)
		return luaL_error(L, "xt.ban.add: a player");
	return tlua_ban(L, n, luaL_checknumber(L, 2), 3);
}

int TLuaAPI_sv_ban_remove(lua_State *L)
{
	char line[64];
	dpsnprintf(line, sizeof(line), "sv_cmd unban %d\n", (int)bound(0, luaL_checkinteger(L, 1), 1024));
	return tlua_queue(L, line);
}

// ---------------------------------------------------------------------------
// Chat commands: "!name args" in chat and "cmd name args"

#define TLUA_CHATCMDS 128
typedef struct tlua_chatcmd_s { qbool used; char mod[TLUA_NAME_LEN + 1]; char name[33]; char help[96]; int ref; } tlua_chatcmd_t;
static tlua_chatcmd_t tlua_chatcmds[TLUA_CHATCMDS];

int TLuaAPI_sv_chatcommand(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	int i, mine = 0;
	tlua_chatcmd_t *slot = NULL;
	const char *p;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (!*name || strlen(name) > 32)
		return luaL_error(L, "xt.chatcommand: a-z 0-9, up to 32");
	for (p = name; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
			return luaL_error(L, "xt.chatcommand: a-z 0-9, up to 32");
	for (i = 0; i < TLUA_CHATCMDS; i++)
	{
		tlua_chatcmd_t *c = &tlua_chatcmds[i];
		if (c->used && !strcmp(c->name, name))
		{
			if (strcmp(c->mod, m->name))
				return luaL_error(L, "xt.chatcommand: !%s belongs to %s", name, c->mod);
			TLua_Unref(m, c->ref);
			c->used = false;
		}
		if (c->used && !strcmp(c->mod, m->name))
			mine++;
		if (!c->used && !slot)
			slot = c;
	}
	if (mine >= 32 || !slot)
		return luaL_error(L, "xt.chatcommand: 32 per mod");
	slot->used = true;
	dp_strlcpy(slot->mod, m->name, sizeof(slot->mod));
	dp_strlcpy(slot->name, name, sizeof(slot->name));
	TLua_CleanText(luaL_optstring(L, 3, ""), slot->help, sizeof(slot->help), false);
	slot->ref = TLua_Ref(L, 2);
	return 0;
}

typedef struct tlua_ccall_s { int player; const char *args; } tlua_ccall_t;
static int tlua_ccall_push(lua_State *L, void *ud)
{
	tlua_ccall_t *c = (tlua_ccall_t *)ud;
	lua_pushinteger(L, c->player);
	lua_pushstring(L, c->args);
	return 2;
}

// a line: "name args"; true when a mod's command took it
static qbool tlua_chatcmd_run(int player, const char *line)
{
	char name[64];
	size_t n = 0;
	int i;
	while (*line == ' ')
		line++;
	while (*line && *line != ' ' && n + 1 < sizeof(name))
		name[n++] = (char)tolower((unsigned char)*line++);
	name[n] = 0;
	while (*line == ' ')
		line++;
	for (i = 0; i < TLUA_CHATCMDS; i++)
	{
		tlua_chatcmd_t *c = &tlua_chatcmds[i];
		tlua_mod_t *m;
		tlua_ccall_t cc;
		char args[512];
		if (!c->used || strcmp(c->name, name))
			continue;
		m = TLua_Find(&tlua_server, c->mod);
		if (!m || m->state != TLUA_RUNNING)
			return false;
		TLua_CleanText(line, args, sizeof(args), false);
		cc.player = player;
		cc.args = args;
		TLua_CallRef(m, c->ref, tlua_ccall_push, &cc, NULL);
		return true;
	}
	return false;
}

// touch_lua_sv.c: a chat line ("!name ...") or a client command ("name ...")
qbool TLua_SVAPI_ChatLine(int player, const char *text)
{
	if (TLua_Busy() || !text || text[0] != '!' || player < 1)
		return false;
	return tlua_chatcmd_run(player, text + 1);
}

qbool TLua_SVAPI_ClientCommand(int player, const char *line)
{
	if (TLua_Busy() || player < 1)
		return false;
	return tlua_chatcmd_run(player, line);
}

// ---------------------------------------------------------------------------

void TLua_SVAPI_ModStopped(tlua_mod_t *m)
{
	int i;
	for (i = 0; i < TLUA_CHATCMDS; i++)
		if (tlua_chatcmds[i].used && !strcmp(tlua_chatcmds[i].mod, m->name))
		{
			TLua_Unref(m, tlua_chatcmds[i].ref);
			tlua_chatcmds[i].used = false;
		}
	// entities the mod spawned go with it
	for (i = 0; i < TLUA_SPAWNS * 4; i++)
	{
		tlua_spawned_t *s = &tlua_spawned[i];
		prvm_edict_t *ed;
		if (!s->used || s->realm != m->realm || strcmp(s->mod, m->name))
			continue;
		s->used = false;
		if ((ed = tlua_ent_live(&s->h)) && !TLua_Busy())
			tlua_qc(NULL, TLUA_QC_REMOVE, ed, 0, NULL, NULL);
	}
}

void TLua_SVAPI_Frame(void);
void TLua_SVAPI_Frame(void)
{
	void TLua_Chan_ServerFrame(void);
	TLua_Chan_ServerFrame();
	if (cls.state == ca_dedicated)
		TLua_API_Frame(); // no client frame: HTTP answers and the rest from here
}

void TLua_SVAPI_Setup(void)
{
}
