/*
Godnotic: Lua mods on the game code's mutator hooks, in both programs
(docs/PLUGIN_STANDARD.md, "Hooks"; the list is docs/LUA_HOOKS.md).

Xonotic's game code calls every place a mutator may change the game through a
named hook (MUTATOR_HOOKABLE in qcsrc/server/mutators/events.qh, the client's
client/mutators/events.qh and friends), with typed arguments some of which are
outputs. scripts/gen-lua-hooks.py reads those declarations and generates

  touch_lua_hooks_gen.h / touch_lua_clhooks_gen.h   the engine's tables
  qcsrc/server/touch_lua_hooks.qc, qcsrc/client/touch_lua_hooks.qc
                                                    one QC callback per hook

so the bridge follows upstream by itself. A hook's QC callback is in the
hook's chain only while a running mod has a handler for it (the QC's
*_hooks_sync, once per frame, asks touch_lua_hook_wanted): a hook nobody
subscribes to costs nothing. When it fires, the callback hands every argument
over in one builtin call (two or three for hooks with more than 7), the engine
runs each subscribed mod's handlers with them in one table, and hands back the
outputs a mod changed and whether one cancelled the hook (only where the hook's
description says returning true means something). On the server changing and
cancelling need the Game rules tier; on the client only the drawing hooks are
writable (they change this player's own screen) and need nothing more.

Limits on top of a call's usual ones: each hook call into a mod gets
lua_hook_ms (default 10 ms), and a mod may spend lua_hook_frame_ms (default
50 ms) in hook calls per host frame; past either it is stopped like any other
error, and the game goes on without it. Entities are handed to Lua as numbers
(a player's is their player id) and are never written back.

Re-entrancy: while any mod's code runs (TLua_Busy), a hook the game fires
(because a mod's API call ran game code) is not delivered: touch_lua_hook_run
answers 0 at once and leaves the call in progress alone.

Builtins (both VMs; the QC checks for the engine first):
  #1751 float  touch_lua_hook_gen(float signature)  -1 when the QC is from another hook list
  #1752 float  touch_lua_hook_wanted(float hook)
  #1753 float  touch_lua_hook_run(float hook, ...)   up to 7 arguments; bit 1 cancel, bit 2 changed
  #1754 void   touch_lua_hook_more(...)              arguments 8 and on, before run (8 per call)
  #1747 float  touch_lua_hook_changed(float slot)
  #1748 float  touch_lua_hook_outf(float slot)
  #1749 vector touch_lua_hook_outv(float slot)
  #1750 string touch_lua_hook_outs(float slot)
The server VM keeps #1741-#1746 (the API 2 one-argument-per-call protocol) for
game code built before #1753 existed.
*/

#include "quakedef.h"
#include "progsvm.h"
#include "prvm_cmds.h"
#include "csprogs.h"
#include "touch_lua.h"
#include "touch_lua_int.h"
#include "touch_lua_hooks_gen.h"
#include "touch_lua_clhooks_gen.h"
#include "touch_lua_api_gen.h"

static cvar_t lua_hook_ms = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_hook_ms", "10", "time limit in milliseconds of each hook call into a Lua mod (0.1-100)"};
static cvar_t lua_hook_frame_ms = {CF_CLIENT | CF_SERVER | CF_ARCHIVE, "lua_hook_frame_ms", "50", "time a Lua mod may spend in hook calls per frame, in milliseconds (1-500); past it the mod is stopped"};

void TLua_Guard(struct cvar_s *var);
void TLua_RegisterCvar(struct cvar_s *var);

typedef struct tlua_hkval_s
{
	double f;
	vec3_t v;
	int e;
	char s[2048];
	qbool changed;
} tlua_hkval_t;

typedef struct tlua_hookset_s
{
	tlua_realm_t *realm;
	const tlua_hookdef_t *defs;
	int count, signature;
	const char *list;              // "server" / "client" for messages
	unsigned gen;                  // bumped on every subscription change
	qbool dirty, warned;
	unsigned char wanted[TLUA_MAX_HOOKS];
	unsigned calls[TLUA_MAX_HOOKS];
	double time[TLUA_MAX_HOOKS];
	char key;                      // registry: hook name -> { handlers } (its address)
	// the call under way: between run and the last out read
	int id, n, pending;
	tlua_hkval_t vals[TLUA_HOOKS_MAXARGS];
	qbool cancel, changed;
	unsigned char writable[TLUA_MAX_HOOKS]; // client: hooks writable with the standard tier
} tlua_hookset_t;

static tlua_hookset_t tlua_sv_hooks, tlua_cl_hooks;

static tlua_hookset_t *tlua_set_of(tlua_realm_t *r)
{
	return r == &tlua_server ? &tlua_sv_hooks : &tlua_cl_hooks;
}

static tlua_hookset_t *tlua_set_of_prog(prvm_prog_t *prog)
{
	return prog == CLVM_prog ? &tlua_cl_hooks : &tlua_sv_hooks;
}

int TLua_Hooks_Count(void) { return TLUA_HOOKS_GEN_COUNT; }

const char *TLua_Hooks_Name(int i)
{
	return i >= 0 && i < TLUA_HOOKS_GEN_COUNT ? tlua_hookdefs[i].name : "";
}

int TLua_Hooks_CountRealm(tlua_realm_t *r) { return tlua_set_of(r)->count; }
const char *TLua_Hooks_NameRealm(tlua_realm_t *r, int i)
{
	tlua_hookset_t *hs = tlua_set_of(r);
	return i >= 0 && i < hs->count ? hs->defs[i].name : "";
}

void TLua_Hooks_Changed(void)
{
	tlua_sv_hooks.dirty = true;
	tlua_cl_hooks.dirty = true;
}

static qbool tlua_hook_sub(tlua_mod_t *m, int i)
{
	return m->state == TLUA_RUNNING && m->L && (m->hookmask[i >> 5] & (1u << (i & 31)));
}

static void tlua_hook_recompute(tlua_hookset_t *hs)
{
	int i, j;
	qbool any = false;
	if (!hs->dirty)
		return;
	hs->dirty = false;
	for (i = 0; i < hs->count; i++)
	{
		unsigned char w = 0;
		for (j = 0; j < hs->realm->nummods && !w; j++)
			if (tlua_hook_sub(&hs->realm->mods[j], i))
				w = 1;
		if (w != hs->wanted[i])
			any = true;
		hs->wanted[i] = w;
	}
	if (any)
		hs->gen++;
}

static int tlua_hook_find(tlua_hookset_t *hs, const char *name)
{
	int i;
	for (i = 0; i < hs->count; i++)
		if (!strcmp(hs->defs[i].name, name))
			return i;
	return -1;
}

// ---------------------------------------------------------------------------
// Lua API

// xt.hook(name, fn): fn(h) runs whenever the game calls that hook
int TLua_Hooks_Subscribe(lua_State *L)
{
	tlua_mod_t *m = TLua_Self(L);
	tlua_hookset_t *hs = tlua_set_of(m->realm);
	const char *name = luaL_checkstring(L, 1);
	int i;
	luaL_checktype(L, 2, LUA_TFUNCTION);
	i = tlua_hook_find(hs, name);
	if (i < 0)
		return luaL_error(L, "xt.hook: no %s hook '%s' (xt.hooknames(), docs/LUA_HOOKS.md)", hs->list, name);
	lua_rawgetp(L, LUA_REGISTRYINDEX, &hs->key);
	if (lua_type(L, -1) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &hs->key);
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

// xt.hooknames(): every hook of this realm
int TLua_Hooks_Names(lua_State *L)
{
	tlua_hookset_t *hs = tlua_set_of(TLua_Self(L)->realm);
	int i;
	lua_createtable(L, hs->count, 0);
	for (i = 0; i < hs->count; i++)
	{
		lua_pushstring(L, hs->defs[i].name);
		lua_rawseti(L, -2, i + 1);
	}
	return 1;
}

// xt.entity(n): what a hook's entity number is (server)
static int tlua_entity(lua_State *L)
{
	prvm_prog_t *prog = SVVM_prog;
	lua_Integer n = luaL_checkinteger(L, 1);
	prvm_edict_t *ed;
	float *o;
	char clean[128];
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

void TLua_Hooks_OpenAPI(lua_State *L)
{
	TLua_SetField(L, "entity", tlua_entity);
}

// ---------------------------------------------------------------------------
// A call

typedef struct tlua_hkcall_s
{
	tlua_hookset_t *hs;
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

// h[4] (Insanotic's ev[4]): the argument at the game's index 4, found through
// a metatable only when a handler asks, so ordinary calls set two keys per
// argument, not three
static char tlua_key_hkmt;
static int tlua_hk_index(lua_State *L)
{
	lua_Integer idx;
	const char *name;
	int set, i, k;
	if (!lua_isinteger(L, 2))
		return 0;
	idx = lua_tointeger(L, 2);
	lua_getfield(L, 1, "name");
	name = lua_tostring(L, -1);
	if (!name)
		return 0;
	for (set = 0; set < 2; set++)
	{
		tlua_hookset_t *hs = set ? &tlua_cl_hooks : &tlua_sv_hooks;
		for (i = 0; i < hs->count; i++)
			if (!strcmp(hs->defs[i].name, name))
				for (k = 0; k < hs->defs[i].nargs; k++)
					if (hs->defs[i].args[k].idx == idx)
					{
						lua_getfield(L, 1, hs->defs[i].args[k].key);
						return 1;
					}
	}
	return 0;
}

static int tlua_hk_tramp(lua_State *L)
{
	tlua_hkcall_t *c = (tlua_hkcall_t *)lua_touserdata(L, 1);
	tlua_hookset_t *hs = c->hs;
	const tlua_hookdef_t *d = c->def;
	lua_Integer i, n;
	int h, list, k;
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &hs->key) != LUA_TTABLE)
		return 0;
	if (lua_getfield(L, -1, d->name) != LUA_TTABLE)
		return 0;
	list = lua_gettop(L);
	// The keys are string constants of the generated table: Lua's string
	// cache finds them by address, so no key is hashed or built per call.
	lua_createtable(L, 0, d->nargs * 2 + 1);
	h = lua_gettop(L);
	if (lua_rawgetp(L, LUA_REGISTRYINDEX, &tlua_key_hkmt) != LUA_TTABLE)
	{
		lua_pop(L, 1);
		lua_createtable(L, 0, 1);
		lua_pushcfunction(L, tlua_hk_index);
		lua_setfield(L, -2, "__index");
		lua_pushvalue(L, -1);
		lua_rawsetp(L, LUA_REGISTRYINDEX, &tlua_key_hkmt);
	}
	lua_setmetatable(L, h);
	lua_pushstring(L, d->name);
	lua_setfield(L, h, "name");
	for (k = 0; k < d->nargs; k++)
	{
		const tlua_hookarg_t *a = &d->args[k];
		tlua_hk_push(L, a, &hs->vals[k]);
		if (a->alias)
		{
			lua_pushvalue(L, -1);
			lua_setfield(L, h, a->alias);
		}
		lua_setfield(L, h, a->key);
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
			hs->cancel = true;
		lua_pop(L, 1);
	}
	if (!c->write)
		return 0;
	// outputs: the plain name first (the one handlers use), then the position
	// key, then the bare index
	for (k = 0; k < d->nargs; k++)
	{
		const tlua_hookarg_t *a = &d->args[k];
		qbool got = false;
		if (!a->writable)
			continue;
		if (a->alias)
		{
			lua_getfield(L, h, a->alias);
			got = tlua_hk_read(L, a, &hs->vals[k]);
		}
		if (!got)
		{
			lua_getfield(L, h, a->key);
			got = tlua_hk_read(L, a, &hs->vals[k]);
		}
		if (!got)
		{
			lua_rawgeti(L, h, a->idx);
			got = tlua_hk_read(L, a, &hs->vals[k]);
		}
		if (got)
		{
			hs->vals[k].changed = true;
			hs->changed = true;
		}
	}
	return 0;
}

static void tlua_hk_run(tlua_hookset_t *hs, int id)
{
	const tlua_hookdef_t *d = &hs->defs[id];
	double framebudget = bound(1, lua_hook_frame_ms.value, 500) / 1000.0;
	double callbudget = bound(0.1, lua_hook_ms.value, 100) / 1000.0;
	int j;
	for (j = 0; j < hs->realm->nummods; j++)
	{
		tlua_mod_t *m = &hs->realm->mods[j];
		tlua_hkcall_t c;
		double t0;
		if (!tlua_hook_sub(m, id))
			continue;
		if (m->hookframe != host.framecount)
		{
			m->hookframe = host.framecount;
			m->hookspent = 0;
		}
		c.hs = hs;
		c.def = d;
		c.write = hs->realm == &tlua_server ? TLua_Has(m, TLUA_TIER_GAMERULES) : hs->writable[id] != 0;
		t0 = Sys_DirtyTime();
		TLua_Run(m, tlua_hk_tramp, &c, callbudget);
		m->hookspent += Sys_DirtyTime() - t0;
		if (m->state == TLUA_RUNNING && m->hookspent > framebudget)
		{
			char why[160];
			dpsnprintf(why, sizeof(why), "hook budget: %.1f ms in hook calls in one frame (lua_hook_frame_ms %g), last in %s",
				m->hookspent * 1000.0, lua_hook_frame_ms.value, d->name);
			TLua_Fail(m, why);
			TLua_UpdateStatus(hs->realm);
		}
	}
}

// ---------------------------------------------------------------------------
// Builtins

static void tlua_hk_load(prvm_prog_t *prog, tlua_hookset_t *hs, const tlua_hookarg_t *a, tlua_hkval_t *v, int parm)
{
	int ofs = OFS_PARM0 + parm * 3;
	v->changed = false;
	switch (a->t)
	{
	case 'e':
		v->e = PRVM_G_EDICTNUM(ofs);
		if (hs == &tlua_cl_hooks && v->e < 0)
			v->e = 0;
		break;
	case 'v':
		VectorCopy(PRVM_G_VECTOR(ofs), v->v);
		break;
	case 's':
		dp_strlcpy(v->s, PRVM_G_STRING(ofs), sizeof(v->s));
		break;
	default:
		v->f = PRVM_G_FLOAT(ofs);
		break;
	}
}

// arguments 8 and on of the next touch_lua_hook_run (they come first)
static int tlua_hk_more_n[2];
static float tlua_hk_more_raw[2][TLUA_HOOKS_MAXARGS][3];
static int tlua_hk_more_str[2][TLUA_HOOKS_MAXARGS];

static void VM_touch_lua_hook_more(prvm_prog_t *prog)
{
	int side = prog == CLVM_prog ? 1 : 0, i;
	for (i = 0; i < prog->argc && tlua_hk_more_n[side] < TLUA_HOOKS_MAXARGS; i++)
	{
		int k = tlua_hk_more_n[side]++;
		VectorCopy(PRVM_G_VECTOR(OFS_PARM0 + i * 3), tlua_hk_more_raw[side][k]);
		tlua_hk_more_str[side][k] = PRVM_G_INT(OFS_PARM0 + i * 3);
	}
}

static void VM_touch_lua_hook_run(prvm_prog_t *prog)
{
	tlua_hookset_t *hs = tlua_set_of_prog(prog);
	int side = prog == CLVM_prog ? 1 : 0;
	int id = (int)PRVM_G_FLOAT(OFS_PARM0), k, more = tlua_hk_more_n[side];
	const tlua_hookdef_t *d;
	double t0;
	tlua_hk_more_n[side] = 0;
	PRVM_G_FLOAT(OFS_RETURN) = 0;
	// re-entered from inside a mod's call (its API call ran game code): not
	// delivered, and the call in progress is left alone
	if (id < 0 || id >= hs->count || TLua_Busy())
		return;
	tlua_hook_recompute(hs);
	if (!hs->wanted[id])
		return;
	d = &hs->defs[id];
	if (prog->argc - 1 + more != d->nargs || prog->argc - 1 > 7)
		return;
	hs->id = id;
	hs->cancel = hs->changed = false;
	for (k = 0; k < d->nargs; k++)
	{
		if (k < 7)
			tlua_hk_load(prog, hs, &d->args[k], &hs->vals[k], k + 1);
		else
		{
			// from touch_lua_hook_more: the raw parameter words, read by type
			const tlua_hookarg_t *a = &d->args[k];
			tlua_hkval_t *v = &hs->vals[k];
			float *raw = tlua_hk_more_raw[side][k - 7];
			v->changed = false;
			if (a->t == 'v')
				VectorCopy(raw, v->v);
			else if (a->t == 's')
				dp_strlcpy(v->s, PRVM_GetString(prog, tlua_hk_more_str[side][k - 7]), sizeof(v->s));
			else if (a->t == 'e')
				v->e = PRVM_NUM_FOR_EDICT(PRVM_PROG_TO_EDICT(tlua_hk_more_str[side][k - 7]));
			else
				v->f = raw[0];
		}
	}
	hs->n = d->nargs;
	t0 = Sys_DirtyTime();
	tlua_hk_run(hs, id);
	hs->calls[id]++;
	hs->time[id] += Sys_DirtyTime() - t0;
	PRVM_G_FLOAT(OFS_RETURN) = (hs->cancel ? 1 : 0) + (hs->changed ? 2 : 0);
	// the outs stay readable until the next run
}

static tlua_hkval_t *tlua_hk_out(prvm_prog_t *prog)
{
	tlua_hookset_t *hs = tlua_set_of_prog(prog);
	int k = (int)PRVM_G_FLOAT(OFS_PARM0);
	if (hs->id < 0 || k < 0 || k >= hs->defs[hs->id].nargs || !hs->defs[hs->id].args[k].writable)
		return NULL;
	return &hs->vals[k];
}

static void VM_touch_lua_hook_changed(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_touch_lua_hook_changed);
	v = tlua_hk_out(prog);
	PRVM_G_FLOAT(OFS_RETURN) = v && v->changed;
}

static void VM_touch_lua_hook_outf(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_touch_lua_hook_outf);
	v = tlua_hk_out(prog);
	PRVM_G_FLOAT(OFS_RETURN) = v ? (float)v->f : 0;
}

static void VM_touch_lua_hook_outv(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_touch_lua_hook_outv);
	v = tlua_hk_out(prog);
	if (v)
		VectorCopy(v->v, PRVM_G_VECTOR(OFS_RETURN));
	else
		VectorClear(PRVM_G_VECTOR(OFS_RETURN));
}

static void VM_touch_lua_hook_outs(prvm_prog_t *prog)
{
	tlua_hkval_t *v;
	VM_SAFEPARMCOUNT(1, VM_touch_lua_hook_outs);
	v = tlua_hk_out(prog);
	PRVM_G_INT(OFS_RETURN) = PRVM_SetTempString(prog, v ? v->s : "", strlen(v ? v->s : ""));
}

static void VM_touch_lua_hook_gen(prvm_prog_t *prog)
{
	tlua_hookset_t *hs = tlua_set_of_prog(prog);
	VM_SAFEPARMCOUNT(1, VM_touch_lua_hook_gen);
	if ((int)PRVM_G_FLOAT(OFS_PARM0) != hs->signature)
	{
		if (!hs->warned)
			Con_Printf(CON_WARN "Lua: the %s game code's hook list does not match this engine's (run scripts/gen-lua-hooks.py and rebuild both); mods get no %s hooks\n", hs->list, hs->list);
		hs->warned = true;
		PRVM_G_FLOAT(OFS_RETURN) = -1;
		return;
	}
	if (hs == &tlua_cl_hooks)
		TLua_CL_CsqcHello();
	tlua_hook_recompute(hs);
	PRVM_G_FLOAT(OFS_RETURN) = (float)(hs->gen & 0xffffff);
}

static void VM_touch_lua_hook_wanted(prvm_prog_t *prog)
{
	tlua_hookset_t *hs = tlua_set_of_prog(prog);
	int id = (int)PRVM_G_FLOAT(OFS_PARM0);
	VM_SAFEPARMCOUNT(1, VM_touch_lua_hook_wanted);
	PRVM_G_FLOAT(OFS_RETURN) = id >= 0 && id < hs->count && hs->wanted[id];
}

// ---- the API 2 protocol (server only): one argument per builtin call -----
static int tlua_hk2_id = -1, tlua_hk2_n;

static void VM_SV_touch_lua_hook_begin(prvm_prog_t *prog)
{
	int id = (int)PRVM_G_FLOAT(OFS_PARM0);
	VM_SAFEPARMCOUNT(1, VM_SV_touch_lua_hook_begin);
	PRVM_G_FLOAT(OFS_RETURN) = 0;
	if (id < 0 || id >= tlua_sv_hooks.count || TLua_Busy())
		return;
	tlua_hk2_id = -1;
	tlua_hook_recompute(&tlua_sv_hooks);
	if (!tlua_sv_hooks.wanted[id])
		return;
	tlua_hk2_id = id;
	tlua_hk2_n = 0;
	PRVM_G_FLOAT(OFS_RETURN) = 1;
}

static tlua_hkval_t *tlua_hk2_next(void)
{
	if (tlua_hk2_id < 0 || tlua_hk2_n >= tlua_sv_hooks.defs[tlua_hk2_id].nargs || TLua_Busy())
		return NULL;
	return &tlua_sv_hooks.vals[tlua_hk2_n++];
}

static void VM_SV_touch_lua_hook_argf(prvm_prog_t *prog) { tlua_hkval_t *v = tlua_hk2_next(); if (v) { v->changed = false; v->f = PRVM_G_FLOAT(OFS_PARM0); } }
static void VM_SV_touch_lua_hook_arge(prvm_prog_t *prog) { tlua_hkval_t *v = tlua_hk2_next(); if (v) { v->changed = false; v->e = PRVM_G_EDICTNUM(OFS_PARM0); } }
static void VM_SV_touch_lua_hook_argv(prvm_prog_t *prog) { tlua_hkval_t *v = tlua_hk2_next(); if (v) { v->changed = false; VectorCopy(PRVM_G_VECTOR(OFS_PARM0), v->v); } }
static void VM_SV_touch_lua_hook_args(prvm_prog_t *prog) { tlua_hkval_t *v = tlua_hk2_next(); if (v) { v->changed = false; dp_strlcpy(v->s, PRVM_G_STRING(OFS_PARM0), sizeof(v->s)); } }

static void VM_SV_touch_lua_hook_call(prvm_prog_t *prog)
{
	tlua_hookset_t *hs = &tlua_sv_hooks;
	int id = tlua_hk2_id;
	double t0;
	PRVM_G_FLOAT(OFS_RETURN) = 0;
	if (id < 0 || TLua_Busy() || tlua_hk2_n != hs->defs[id].nargs)
	{
		tlua_hk2_id = -1;
		return;
	}
	hs->id = id;
	hs->cancel = hs->changed = false;
	t0 = Sys_DirtyTime();
	tlua_hk_run(hs, id);
	hs->calls[id]++;
	hs->time[id] += Sys_DirtyTime() - t0;
	PRVM_G_FLOAT(OFS_RETURN) = (hs->cancel ? 1 : 0) + (hs->changed ? 2 : 0);
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
	b[1747] = VM_touch_lua_hook_changed;
	b[1748] = VM_touch_lua_hook_outf;
	b[1749] = VM_touch_lua_hook_outv;
	b[1750] = VM_touch_lua_hook_outs;
	b[1751] = VM_touch_lua_hook_gen;
	b[1752] = VM_touch_lua_hook_wanted;
	b[1753] = VM_touch_lua_hook_run;
	b[1754] = VM_touch_lua_hook_more;
}

void TLua_Hooks_InstallClientBuiltins(prvm_builtin_t *b);
void TLua_Hooks_InstallClientBuiltins(prvm_builtin_t *b)
{
	b[1747] = VM_touch_lua_hook_changed;
	b[1748] = VM_touch_lua_hook_outf;
	b[1749] = VM_touch_lua_hook_outv;
	b[1750] = VM_touch_lua_hook_outs;
	b[1751] = VM_touch_lua_hook_gen;
	b[1752] = VM_touch_lua_hook_wanted;
	b[1753] = VM_touch_lua_hook_run;
	b[1754] = VM_touch_lua_hook_more;
}

// ---------------------------------------------------------------------------
// sv_lua_hooks / lua_hooks [all]: the hooks, who subscribes, what they cost

static void tlua_hooks_f(cmd_state_t *cmd)
{
	tlua_hookset_t *hs = !strncmp(Cmd_Argv(cmd, 0), "sv_", 3) ? &tlua_sv_hooks : &tlua_cl_hooks;
	qbool all = Cmd_Argc(cmd) >= 2 && !strcmp(Cmd_Argv(cmd, 1), "all");
	int i, j, shown = 0;
	tlua_hook_recompute(hs);
	Con_Printf("%d %s hooks (docs/LUA_HOOKS.md); %s:\n", hs->count, hs->list, all ? "all of them" : "the ones subscribed or called (add \"all\" to list every one)");
	for (i = 0; i < hs->count; i++)
	{
		char subs[256] = "";
		const tlua_hookdef_t *d = &hs->defs[i];
		qbool writes = false;
		int k;
		for (k = 0; k < d->nargs; k++)
			writes |= d->args[k].writable;
		for (j = 0; j < hs->realm->nummods; j++)
			if (hs->realm->mods[j].hookmask[i >> 5] & (1u << (i & 31)))
			{
				if (subs[0])
					dp_strlcat(subs, ",", sizeof(subs));
				dp_strlcat(subs, hs->realm->mods[j].name, sizeof(subs));
			}
		if (!all && !subs[0] && !hs->calls[i])
			continue;
		shown++;
		Con_Printf("  %-34s %s%s%s", d->name, writes ? "out " : "", d->cancel ? "cancel " : "", hs->wanted[i] ? "^2on^7 " : "");
		if (subs[0])
			Con_Printf(" by %s", subs);
		if (hs->calls[i])
			Con_Printf(" (%u calls, %.2f us each)", hs->calls[i], hs->time[i] * 1e6 / hs->calls[i]);
		Con_Printf("\n");
	}
	if (!shown)
		Con_Printf("  none subscribed\n");
}

static void tlua_hooks_reset_f(cmd_state_t *cmd)
{
	tlua_hookset_t *hs = !strncmp(Cmd_Argv(cmd, 0), "sv_", 3) ? &tlua_sv_hooks : &tlua_cl_hooks;
	memset(hs->calls, 0, sizeof(hs->calls));
	memset(hs->time, 0, sizeof(hs->time));
}

static void tlua_hookset_init(tlua_hookset_t *hs, tlua_realm_t *r, const tlua_hookdef_t *defs, int count, int sig, const char *list)
{
	hs->realm = r;
	hs->defs = defs;
	hs->count = count;
	hs->signature = sig;
	hs->list = list;
	hs->gen = 1;
	hs->dirty = true;
	hs->id = -1;
}

void TLua_Hooks_Setup(void)
{
	int i, j;
	tlua_hookset_init(&tlua_sv_hooks, &tlua_server, tlua_hookdefs, TLUA_HOOKS_GEN_COUNT, TLUA_HOOKS_GEN_SIGNATURE, "server");
	tlua_hookset_init(&tlua_cl_hooks, &tlua_client, tlua_clhookdefs, TLUA_CLHOOKS_GEN_COUNT, TLUA_CLHOOKS_GEN_SIGNATURE, "client");
	for (i = 0; i < tlua_cl_hooks.count; i++)
		for (j = 0; tlua_cl_hooks_writable[j]; j++)
			if (!strcmp(tlua_cl_hooks.defs[i].name, tlua_cl_hooks_writable[j]))
				tlua_cl_hooks.writable[i] = 1;
	TLua_RegisterCvar(&lua_hook_ms);
	TLua_RegisterCvar(&lua_hook_frame_ms);
	TLua_Guard(&lua_hook_ms);
	TLua_Guard(&lua_hook_frame_ms);
	Cmd_AddCommand(CF_SERVER, "sv_lua_hooks", tlua_hooks_f, "list the server game's mutator hooks Lua mods subscribe to, with call counts and cost (all: every hook)");
	Cmd_AddCommand(CF_SERVER, "sv_lua_hooks_resetstats", tlua_hooks_reset_f, "zero the hook call counters sv_lua_hooks shows");
	Cmd_AddCommand(CF_CLIENT, "lua_hooks", tlua_hooks_f, "list the client game's (CSQC) mutator hooks Lua mods subscribe to, with call counts and cost (all: every hook)");
	Cmd_AddCommand(CF_CLIENT, "lua_hooks_resetstats", tlua_hooks_reset_f, "zero the hook call counters lua_hooks shows");
}
