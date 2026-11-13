/*
Godnotic: Lua server plugins on the game's mutator hooks (docs/PLUGIN_STANDARD.md,
"Hooks"; the list is docs/LUA_HOOKS.md).

Xonotic's game code calls every place a mutator may change the game through a
named hook (MUTATOR_HOOKABLE in qcsrc/server/mutators/events.qh and friends),
with typed arguments some of which are outputs. scripts/gen-lua-hooks.py reads
those declarations and generates

  touch_lua_hooks_gen.h          this file's table: names, argument types, outputs
  qcsrc/server/touch_lua_hooks.qc  one QC callback per hook

so the bridge follows upstream by itself. A hook's QC callback is in the
hook's chain only while some running plugin with the "hooks" permission has a
handler for it (touch_lua_hooks_sync, once per server frame, asks
touch_lua_hook_wanted): a hook nobody subscribes to costs nothing. When it
fires, the callback hands the arguments over one builtin call each, the engine
runs every subscribed plugin's handlers with them in a table, and hands back
the outputs a plugin with "hooks_write" changed, and whether one cancelled the
hook (only where the hook's description says returning true means something).

Limits on top of a call's usual ones: each hook call into a plugin gets
lua_hook_ms (default 5 ms), and a plugin may spend lua_hook_frame_ms (default
25 ms) in hook calls per host frame; past either it is stopped like any other
error, and the game goes on without it. Entities are handed to Lua as numbers
(a player's is their player id) and are never written back.

Builtins (server VM only; the QC checks for the engine first):
  #1741 float  touch_lua_hook_begin(float hook)
  #1742 void   touch_lua_hook_argf(float f)      float, int, bool
  #1743 void   touch_lua_hook_arge(entity e)
  #1744 void   touch_lua_hook_argv(vector v)
  #1745 void   touch_lua_hook_args(string s)
  #1746 float  touch_lua_hook_call()             bit 1: cancel, bit 2: an output changed
  #1747 float  touch_lua_hook_changed(float slot)
  #1748 float  touch_lua_hook_outf(float slot)
  #1749 vector touch_lua_hook_outv(float slot)
  #1750 string touch_lua_hook_outs(float slot)
  #1751 float  touch_lua_hook_gen(float signature)  -1 when the QC is from another hook list
  #1752 float  touch_lua_hook_wanted(float hook)
*/

#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua_hooks_gen.h"

static cvar_t lua_hook_ms = {CF_SERVER | CF_ARCHIVE, "lua_hook_ms", "5", "time limit in milliseconds of each hook call into a Lua plugin (0.1-40)"};
static cvar_t lua_hook_frame_ms = {CF_SERVER | CF_ARCHIVE, "lua_hook_frame_ms", "25", "time a Lua plugin may spend in hook calls per frame, in milliseconds (1-200); past it the plugin is stopped"};

void TLua_Guard(struct cvar_s *var);
void TLua_RegisterCvar(struct cvar_s *var);

static char tlua_key_hooks; // registry: hook name -> { handlers }

static unsigned tlua_hook_gen = 1;          // bumped on every subscription change
static qbool tlua_hook_dirty = true;
static unsigned char tlua_hook_wanted[TLUA_HOOKS_GEN_COUNT];
static unsigned tlua_hook_calls[TLUA_HOOKS_GEN_COUNT];
static double tlua_hook_time[TLUA_HOOKS_GEN_COUNT];
static qbool tlua_hook_warned;

typedef struct tlua_hkval_s
{
	double f;
	vec3_t v;
	int e;
	char s[2048];
	qbool changed;
} tlua_hkval_t;

// the hook call under way (between begin and the last out read)
static int tlua_hk_id = -1;
static int tlua_hk_n;
static tlua_hkval_t tlua_hk_vals[TLUA_HOOKS_MAXARGS];
static qbool tlua_hk_cancel, tlua_hk_changed;

int TLua_Hooks_Count(void)
{
	return TLUA_HOOKS_GEN_COUNT;
}

const char *TLua_Hooks_Name(int i)
{
	return i >= 0 && i < TLUA_HOOKS_GEN_COUNT ? tlua_hookdefs[i].name : "";
}

void TLua_Hooks_Changed(void)
{
	tlua_hook_dirty = true;
}

static qbool tlua_hook_sub(tlua_mod_t *m, int i)
{
	return m->state == TLUA_RUNNING && m->L && (m->hookmask[i >> 5] & (1u << (i & 31))) && TLua_Has(m, TLUA_PERM_HOOKS);
}

static void tlua_hook_recompute(void)
{
	int i, j;
	qbool any = false;
	if (!tlua_hook_dirty)
		return;
	tlua_hook_dirty = false;
	for (i = 0; i < TLUA_HOOKS_GEN_COUNT; i++)
	{
		unsigned char w = 0;
		for (j = 0; j < tlua_server.nummods && !w; j++)
			if (tlua_hook_sub(&tlua_server.mods[j], i))
				w = 1;
		if (w != tlua_hook_wanted[i])
			any = true;
		tlua_hook_wanted[i] = w;
	}
	if (any)
		tlua_hook_gen++;
}

static int tlua_hook_find(const char *name)
{
	int i;
	for (i = 0; i < TLUA_HOOKS_GEN_COUNT; i++)
		if (!strcmp(tlua_hookdefs[i].name, name))
			return i;
	return -1;
}

// ---------------------------------------------------------------------------
// Lua API (server realm)

// xt.hook(name, fn): fn(h) runs whenever the game calls that hook
static int tlua_hook(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	const char *name = luaL_checkstring(L, 1);
	int i;
	TLua_Require(L, TLUA_PERM_HOOKS);
	luaL_checktype(L, 2, LUA_TFUNCTION);
	i = tlua_hook_find(name);
	if (i < 0)
		return luaL_error(L, "xt.hook: no hook '%s' (sv_lua_hooks all, docs/LUA_HOOKS.md)", name);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_hooks);
	if (lua_type(L, -1) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_hooks);
	}
	if (lua_getfield(L, -1, name) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, name);
	}
	lua_pushvalue(L, 2);
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
	m->hookmask[i >> 5] |= 1u << (i & 31);
	TLua_Hooks_Changed();
	return 0;
}

// xt.entity(n): what a hook's entity number is
static int tlua_entity(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	lua_Integer n = luaL_checkinteger(L, 1);
	prvm_edict_t *ed;
	float *o;
	char clean[128];
	TLua_Require(L, TLUA_PERM_HOOKS);
	if (!sv.active || !prog->loaded || n < 1 || n >= prog->num_edicts)
		return 0;
	ed = PRVM_EDICT_NUM(n);
	if (ed->free)
		return 0;
	lua_createtable(L, 0, 8);
	lua_pushinteger(L, n);
	lua_setfield(L, -2, "id");
	lua_pushstring(L, TLua_CleanText(PRVM_GetString(prog, PRVM_serveredictstring(ed, classname)), clean, sizeof(clean), false));
	lua_setfield(L, -2, "classname");
	lua_pushstring(L, TLua_CleanText(PRVM_GetString(prog, PRVM_serveredictstring(ed, netname)), clean, sizeof(clean), false));
	lua_setfield(L, -2, "netname");
	lua_pushboolean(L, n <= svs.maxclients);
	lua_setfield(L, -2, "player");
	lua_pushinteger(L, PRVM_serveredictedict(ed, owner));
	lua_setfield(L, -2, "owner");
	lua_pushnumber(L, PRVM_serveredictfloat(ed, health));
	lua_setfield(L, -2, "health");
	lua_pushinteger(L, (lua_Integer)PRVM_serveredictfloat(ed, team));
	lua_setfield(L, -2, "team");
	o = PRVM_serveredictvector(ed, origin);
	lua_createtable(L, 0, 3);
	lua_pushnumber(L, o[0]);
	lua_setfield(L, -2, "x");
	lua_pushnumber(L, o[1]);
	lua_setfield(L, -2, "y");
	lua_pushnumber(L, o[2]);
	lua_setfield(L, -2, "z");
	lua_setfield(L, -2, "origin");
	return 1;
}

// xt.hooknames(): every hook this game has
static int tlua_hooknames(lua_State *L)
{
	int i;
	TLua_Require(L, TLUA_PERM_HOOKS);
	lua_createtable(L, TLUA_HOOKS_GEN_COUNT, 0);
	for (i = 0; i < TLUA_HOOKS_GEN_COUNT; i++)
	{
		lua_pushstring(L, tlua_hookdefs[i].name);
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

void TLua_Hooks_OpenAPI(lua_State *L)
{
	TLua_SetField(L, "hook", tlua_hook);
	TLua_SetField(L, "entity", tlua_entity);
	TLua_SetField(L, "hooknames", tlua_hooknames);
}

// ---------------------------------------------------------------------------
// A call

typedef struct tlua_hkcall_s
{
	const tlua_hookdef_t *def;
	qbool write;
} tlua_hkcall_t;

static void tlua_hk_push(lua_State *L, const tlua_hookarg_t *a, const tlua_hkval_t *v)
{
	switch (a->t)
	{
	case 'e':
		lua_pushinteger(L, v->e);
		break;
	case 'b':
		lua_pushboolean(L, v->f != 0);
		break;
	case 'i':
		lua_pushinteger(L, (lua_Integer)v->f);
		break;
	case 'v':
		lua_createtable(L, 0, 3);
		lua_pushnumber(L, v->v[0]);
		lua_setfield(L, -2, "x");
		lua_pushnumber(L, v->v[1]);
		lua_setfield(L, -2, "y");
		lua_pushnumber(L, v->v[2]);
		lua_setfield(L, -2, "z");
		break;
	case 's':
		lua_pushstring(L, v->s);
		break;
	default:
		lua_pushnumber(L, v->f);
		break;
	}
}

static qbool tlua_finite(double d)
{
	return d == d && d < 1e30 && d > -1e30;
}

// reads a handler's value for one output at the stack top (popped);
// true when it is usable and differs
static qbool tlua_hk_read(lua_State *L, const tlua_hookarg_t *a, tlua_hkval_t *v)
{
	qbool changed = false;
	switch (a->t)
	{
	case 'b':
		if (lua_type(L, -1) == LUA_TBOOLEAN || lua_type(L, -1) == LUA_TNUMBER)
		{
			double f = lua_type(L, -1) == LUA_TBOOLEAN ? lua_toboolean(L, -1) : (lua_tonumber(L, -1) != 0);
			if (f != v->f)
				v->f = f, changed = true;
		}
		break;
	case 'f':
	case 'i':
		if (lua_type(L, -1) == LUA_TNUMBER)
		{
			double f = lua_tonumber(L, -1);
			if (a->t == 'i')
				f = floor(f);
			if (tlua_finite(f) && f != v->f)
				v->f = f, changed = true;
		}
		break;
	case 'v':
		if (lua_type(L, -1) == LUA_TTABLE)
		{
			static const char *const k[3] = {"x", "y", "z"};
			int j;
			for (j = 0; j < 3; j++)
			{
				double f;
				if (lua_getfield(L, -1, k[j]) != LUA_TNUMBER)
				{
					lua_pop(L, 1);
					lua_rawgeti(L, -1, j + 1);
				}
				f = lua_tonumber(L, -1);
				if (lua_type(L, -1) == LUA_TNUMBER && tlua_finite(f) && (float)f != v->v[j])
					v->v[j] = (float)f, changed = true;
				lua_pop(L, 1);
			}
		}
		break;
	case 's':
		if (lua_type(L, -1) == LUA_TSTRING)
		{
			char clean[sizeof(v->s)];
			TLua_CleanText(lua_tostring(L, -1), clean, sizeof(clean), false);
			if (strcmp(clean, v->s))
				dp_strlcpy(v->s, clean, sizeof(v->s)), changed = true;
		}
		break;
	}
	lua_pop(L, 1);
	return changed;
}

static int tlua_hk_tramp(lua_State *L)
{
	tlua_hkcall_t *c = (tlua_hkcall_t *)lua_touserdata(L, 1);
	const tlua_hookdef_t *d = c->def;
	lua_Integer i, n;
	int h, list, k;
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_hooks) != LUA_TTABLE)
		return 0;
	if (lua_getfield(L, -1, d->name) != LUA_TTABLE)
		return 0;
	list = lua_gettop(L);
	lua_createtable(L, 0, d->nargs * 2 + 1);
	h = lua_gettop(L);
	lua_pushstring(L, d->name);
	lua_setfield(L, h, "name");
	for (k = 0; k < d->nargs; k++)
	{
		char key[8];
		dpsnprintf(key, sizeof(key), "%c%d", d->args[k].t, d->args[k].idx);
		tlua_hk_push(L, &d->args[k], &tlua_hk_vals[k]);
		if (d->args[k].alias)
		{
			lua_pushvalue(L, -1);
			lua_setfield(L, h, d->args[k].alias);
		}
		lua_setfield(L, h, key);
	}
	n = (lua_Integer)lua_rawlen(L, list);
	for (i = 1; i <= n; i++)
	{
		if (lua_rawgeti(L, list, i) != LUA_TFUNCTION)
		{
			lua_pop(L, 1);
			continue;
		}
		lua_pushvalue(L, h);
		lua_call(L, 1, 1);
		if (c->write && d->cancel && lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1))
			tlua_hk_cancel = true;
		lua_pop(L, 1);
	}
	if (!c->write)
		return 0;
	// outputs: the plain name first (the one handlers use), then the position key
	for (k = 0; k < d->nargs; k++)
	{
		char key[8];
		qbool got = false;
		if (!d->args[k].writable)
			continue;
		if (d->args[k].alias)
		{
			lua_getfield(L, h, d->args[k].alias);
			got = tlua_hk_read(L, &d->args[k], &tlua_hk_vals[k]);
		}
		if (!got)
		{
			dpsnprintf(key, sizeof(key), "%c%d", d->args[k].t, d->args[k].idx);
			lua_getfield(L, h, key);
			got = tlua_hk_read(L, &d->args[k], &tlua_hk_vals[k]);
		}
		if (got)
		{
			tlua_hk_vals[k].changed = true;
			tlua_hk_changed = true;
		}
	}
	return 0;
}

static void tlua_hk_run(int id)
{
	const tlua_hookdef_t *d = &tlua_hookdefs[id];
	double framebudget = bound(1, lua_hook_frame_ms.value, 200) / 1000.0;
	double callbudget = bound(0.1, lua_hook_ms.value, 40) / 1000.0;
	int j;
	for (j = 0; j < tlua_server.nummods; j++)
	{
		tlua_mod_t *m = &tlua_server.mods[j];
		tlua_hkcall_t c;
		double t0;
		if (!tlua_hook_sub(m, id))
			continue;
		if (m->hookframe != host.framecount)
		{
			m->hookframe = host.framecount;
			m->hookspent = 0;
		}
		c.def = d;
		c.write = TLua_Has(m, TLUA_PERM_HOOKS_WRITE);
		t0 = Sys_DirtyTime();
		TLua_Run(m, tlua_hk_tramp, &c, callbudget);
		m->hookspent += Sys_DirtyTime() - t0;
		if (m->state == TLUA_RUNNING && m->hookspent > framebudget)
		{
			char why[160];
			dpsnprintf(why, sizeof(why), "hook budget: %.1f ms in hook calls in one frame (lua_hook_frame_ms %g), last in %s",
				m->hookspent * 1000.0, lua_hook_frame_ms.value, d->name);
			TLua_Fail(m, why);
			TLua_UpdateStatus(&tlua_server);
		}
	}
}

// ---------------------------------------------------------------------------
// Builtins

static void VM_SV_touch_lua_hook_begin(prvm_prog_t *prog)
{
	int id = (int)PRVM_G_FLOAT(OFS_PARM0);
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_begin);
	PRVM_G_FLOAT(OFS_RETURN) = 0;
	tlua_hk_id = -1;
	if (id < 0 || id >= TLUA_HOOKS_GEN_COUNT || TLua_Busy())
		return;
	tlua_hook_recompute();
	if (!tlua_hook_wanted[id])
		return;
	tlua_hk_id = id;
	tlua_hk_n = 0;
	tlua_hk_cancel = tlua_hk_changed = false;
	PRVM_G_FLOAT(OFS_RETURN) = 1;
}

static tlua_hkval_t *tlua_hk_next(void)
{
	tlua_hkval_t *v;
	if (tlua_hk_id < 0 || tlua_hk_n >= tlua_hookdefs[tlua_hk_id].nargs)
		return NULL;
	v = &tlua_hk_vals[tlua_hk_n++];
	v->changed = false;
	return v;
}

static void VM_SV_touch_lua_hook_argf(prvm_prog_t *prog)
{
	tlua_hkval_t *v = tlua_hk_next();
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_argf);
	if (v)
		v->f = PRVM_G_FLOAT(OFS_PARM0);
}

static void VM_SV_touch_lua_hook_arge(prvm_prog_t *prog)
{
	tlua_hkval_t *v = tlua_hk_next();
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_arge);
	if (v)
		v->e = PRVM_G_EDICTNUM(OFS_PARM0);
}

static void VM_SV_touch_lua_hook_argv(prvm_prog_t *prog)
{
	tlua_hkval_t *v = tlua_hk_next();
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_argv);
	if (v)
		VectorCopy(PRVM_G_VECTOR(OFS_PARM0), v->v);
}

static void VM_SV_touch_lua_hook_args(prvm_prog_t *prog)
{
	tlua_hkval_t *v = tlua_hk_next();
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_args);
	if (v)
		dp_strlcpy(v->s, PRVM_G_STRING(OFS_PARM0), sizeof(v->s));
}

static void VM_SV_touch_lua_hook_call(prvm_prog_t *prog)
{
	int id = tlua_hk_id;
	double t0;
	VM_SAFEPARMCOUNT(0, VM_SV_touch_lua_hook_call);
	PRVM_G_FLOAT(OFS_RETURN) = 0;
	if (id < 0 || tlua_hk_n != tlua_hookdefs[id].nargs)
	{
		tlua_hk_id = -1;
		return;
	}
	t0 = Sys_DirtyTime();
	tlua_hk_run(id);
	tlua_hook_calls[id]++;
	tlua_hook_time[id] += Sys_DirtyTime() - t0;
	PRVM_G_FLOAT(OFS_RETURN) = (tlua_hk_cancel ? 1 : 0) + (tlua_hk_changed ? 2 : 0);
	// the outs stay readable until the next begin
}

static tlua_hkval_t *tlua_hk_out(prvm_prog_t *prog)
{
	int k = (int)PRVM_G_FLOAT(OFS_PARM0);
	if (tlua_hk_id < 0 || k < 0 || k >= tlua_hookdefs[tlua_hk_id].nargs || !tlua_hookdefs[tlua_hk_id].args[k].writable)
		return NULL;
	return &tlua_hk_vals[k];
}

static void VM_SV_touch_lua_hook_changed(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_changed);
	v = tlua_hk_out(prog);
	PRVM_G_FLOAT(OFS_RETURN) = v && v->changed;
}

static void VM_SV_touch_lua_hook_outf(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_outf);
	v = tlua_hk_out(prog);
	PRVM_G_FLOAT(OFS_RETURN) = v ? (float)v->f : 0;
}

static void VM_SV_touch_lua_hook_outv(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_outv);
	v = tlua_hk_out(prog);
	if (v)
		VectorCopy(v->v, PRVM_G_VECTOR(OFS_RETURN));
	else
		VectorClear(PRVM_G_VECTOR(OFS_RETURN));
}

static void VM_SV_touch_lua_hook_outs(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_outs);
	v = tlua_hk_out(prog);
	PRVM_G_INT(OFS_RETURN) = PRVM_SetTempString(prog, v ? v->s : "", strlen(v ? v->s : ""));
}

static void VM_SV_touch_lua_hook_gen(prvm_prog_t *prog)
{
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_gen);
	if ((int)PRVM_G_FLOAT(OFS_PARM0) != TLUA_HOOKS_GEN_SIGNATURE)
	{
		if (!tlua_hook_warned)
			Con_Printf(CON_WARN "Lua: the game code's hook list does not match this engine's (run scripts/gen-lua-hooks.py and rebuild both); plugins get no hooks\n");
		tlua_hook_warned = true;
		PRVM_G_FLOAT(OFS_RETURN) = -1;
		return;
	}
	tlua_hook_recompute();
	PRVM_G_FLOAT(OFS_RETURN) = (float)(tlua_hook_gen & 0xffffff);
}

static void VM_SV_touch_lua_hook_wanted(prvm_prog_t *prog)
{
	int id = (int)PRVM_G_FLOAT(OFS_PARM0);
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_wanted);
	PRVM_G_FLOAT(OFS_RETURN) = id >= 0 && id < TLUA_HOOKS_GEN_COUNT && tlua_hook_wanted[id];
}

void TLua_Hooks_InstallBuiltins(prvm_builtin_t *b);
void TLua_Hooks_InstallBuiltins(prvm_builtin_t *b)
{
	b[1741] = VM_SV_touch_lua_hook_begin;
	b[1742] = VM_SV_touch_lua_hook_argf;
	b[1743] = VM_SV_touch_lua_hook_arge;
	b[1744] = VM_SV_touch_lua_hook_argv;
	b[1745] = VM_SV_touch_lua_hook_args;
	b[1746] = VM_SV_touch_lua_hook_call;
	b[1747] = VM_SV_touch_lua_hook_changed;
	b[1748] = VM_SV_touch_lua_hook_outf;
	b[1749] = VM_SV_touch_lua_hook_outv;
	b[1750] = VM_SV_touch_lua_hook_outs;
	b[1751] = VM_SV_touch_lua_hook_gen;
	b[1752] = VM_SV_touch_lua_hook_wanted;
}

// ---------------------------------------------------------------------------
// sv_lua_hooks [all]: the hooks, who subscribes, what they cost

static void tlua_hooks_f(cmd_state_t *cmd)
{
	qbool all = Cmd_Argc(cmd) >= 2 && !strcmp(Cmd_Argv(cmd, 1), "all");
	int i, j, shown = 0;
	tlua_hook_recompute();
	Con_Printf("%d mutator hooks (docs/LUA_HOOKS.md); %s:\n", TLUA_HOOKS_GEN_COUNT, all ? "all of them" : "the ones subscribed or called (sv_lua_hooks all lists every one)");
	for (i = 0; i < TLUA_HOOKS_GEN_COUNT; i++)
	{
		char subs[256] = "";
		const tlua_hookdef_t *d = &tlua_hookdefs[i];
		qbool writes = false;
		int k;
		for (k = 0; k < d->nargs; k++)
			writes |= d->args[k].writable;
		for (j = 0; j < tlua_server.nummods; j++)
			if (tlua_server.mods[j].hookmask[i >> 5] & (1u << (i & 31)))
			{
				if (subs[0])
					dp_strlcat(subs, ",", sizeof(subs));
				dp_strlcat(subs, tlua_server.mods[j].name, sizeof(subs));
			}
		if (!all && !subs[0] && !tlua_hook_calls[i])
			continue;
		shown++;
		Con_Printf("  %-34s %s%s%s", d->name, writes ? "out " : "", d->cancel ? "cancel " : "", tlua_hook_wanted[i] ? "^2on^7 " : "");
		if (subs[0])
			Con_Printf(" by %s", subs);
		if (tlua_hook_calls[i])
			Con_Printf(" (%u calls, %.2f us each)", tlua_hook_calls[i], tlua_hook_time[i] * 1e6 / tlua_hook_calls[i]);
		Con_Printf("\n");
	}
	if (!shown)
		Con_Printf("  none subscribed\n");
}

static void tlua_hooks_reset_f(cmd_state_t *cmd)
{
	(void)cmd;
	memset(tlua_hook_calls, 0, sizeof(tlua_hook_calls));
	memset(tlua_hook_time, 0, sizeof(tlua_hook_time));
}

void TLua_Hooks_Setup(void)
{
	TLua_RegisterCvar(&lua_hook_ms);
	TLua_RegisterCvar(&lua_hook_frame_ms);
	TLua_Guard(&lua_hook_ms);
	TLua_Guard(&lua_hook_frame_ms);
	Cmd_AddCommand(CF_SERVER, "sv_lua_hooks", tlua_hooks_f, "list the game's mutator hooks Lua plugins subscribe to, with call counts and cost (all: every hook)");
	Cmd_AddCommand(CF_SERVER, "sv_lua_hooks_resetstats", tlua_hooks_reset_f, "zero the hook call counters sv_lua_hooks shows");
}
