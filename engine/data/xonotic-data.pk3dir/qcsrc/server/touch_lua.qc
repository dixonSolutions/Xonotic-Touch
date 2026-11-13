// Godnotic: game events for Lua server plugins (engine/darkplaces/touch_lua_sv.c,
// docs/MODDING.md). Each hook hands one event to the engine, which runs the plugins'
// handlers and returns their verdict. Only the Touch engine has the builtin, so
// nothing is called unless it registered the sv_lua_enable cvar itself.

float touch_lua_event(string ev, entity a, entity b, float n, string s) = #1740;
// the hook bridge (engine/darkplaces/touch_lua_hooks.c, docs/LUA_HOOKS.md)
float touch_lua_hook_begin(float hook) = #1741;
void touch_lua_hook_argf(float f) = #1742;
void touch_lua_hook_arge(entity e) = #1743;
void touch_lua_hook_argv(vector v) = #1744;
void touch_lua_hook_args(string s) = #1745;
float touch_lua_hook_call() = #1746;
float touch_lua_hook_changed(float slot) = #1747;
float touch_lua_hook_outf(float slot) = #1748;
vector touch_lua_hook_outv(float slot) = #1749;
string touch_lua_hook_outs(float slot) = #1750;
float touch_lua_hook_gen(float signature) = #1751;
float touch_lua_hook_wanted(float hook) = #1752;

bool touch_lua_checked;
bool touch_lua_present;

bool touch_lua_on()
{
	if (!touch_lua_checked)
	{
		touch_lua_checked = true;
		touch_lua_present = (cvar_type("sv_lua_enable") & CVAR_TYPEFLAG_ENGINE) != 0;
	}
	return touch_lua_present && cvar("sv_lua_enable");
}

REGISTER_MUTATOR(touch_lua, true);

#include <server/touch_lua_hooks.qc>

MUTATOR_HOOKFUNCTION(touch_lua, ClientConnect)
{
	if (touch_lua_on())
		touch_lua_event("connect", M_ARGV(0, entity), NULL, 0, "");
}

MUTATOR_HOOKFUNCTION(touch_lua, ClientDisconnect)
{
	if (touch_lua_on())
		touch_lua_event("disconnect", M_ARGV(0, entity), NULL, 0, "");
}

MUTATOR_HOOKFUNCTION(touch_lua, PlayerSpawn)
{
	if (touch_lua_on())
		touch_lua_event("spawn", M_ARGV(0, entity), NULL, 0, "");
}

MUTATOR_HOOKFUNCTION(touch_lua, PlayerDies)
{
	if (touch_lua_on())
		touch_lua_event("death", M_ARGV(2, entity), M_ARGV(1, entity), 0, Deathtype_Name(M_ARGV(3, float)));
}

MUTATOR_HOOKFUNCTION(touch_lua, Damage_Calculate)
{
	if (touch_lua_on() && IS_PLAYER(M_ARGV(2, entity)))
		M_ARGV(4, float) = touch_lua_event("damage", M_ARGV(2, entity), M_ARGV(1, entity), M_ARGV(4, float), Deathtype_Name(M_ARGV(3, float)));
}

MUTATOR_HOOKFUNCTION(touch_lua, ChatMessage)
{
	// only a message that would really be sent (ret > 0); a plugin can hold it back
	if (touch_lua_on() && M_ARGV(1, int) > 0 && IS_REAL_CLIENT(M_ARGV(0, entity)))
		if (touch_lua_event("chat", M_ARGV(0, entity), NULL, M_ARGV(4, int), M_ARGV(2, string)))
			M_ARGV(1, int) = 0;
}

MUTATOR_HOOKFUNCTION(touch_lua, ItemTouch)
{
	entity toucher = M_ARGV(1, entity);
	if (touch_lua_on() && IS_PLAYER(toucher))
		if (touch_lua_event("pickup", toucher, NULL, 0, M_ARGV(0, entity).netname))
			return MUT_ITEMTOUCH_RETURN;
	return MUT_ITEMTOUCH_CONTINUE;
}

MUTATOR_HOOKFUNCTION(touch_lua, SV_ParseClientCommand)
{
	if (touch_lua_on())
		return touch_lua_event("clientcmd", M_ARGV(0, entity), NULL, M_ARGV(2, int), M_ARGV(3, string)) != 0;
	return false;
}

MUTATOR_HOOKFUNCTION(touch_lua, MatchEnd)
{
	if (touch_lua_on())
		touch_lua_event("matchend", NULL, NULL, 0, "");
}

MUTATOR_HOOKFUNCTION(touch_lua, SV_StartFrame)
{
	if (touch_lua_on())
	{
		touch_lua_event("frame", NULL, NULL, 0, "");
		touch_lua_hooks_sync(); // the plugins' hooks in or out of their chains
	}
}
