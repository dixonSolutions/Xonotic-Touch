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
// API 3: a hook's arguments in one call (up to 7; more before it, 8 at a time)
float touch_lua_hook_run(float hook, ...) = #1753;
void touch_lua_hook_more(...) = #1754;

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

// API 3: what server mods ask the game to do its own way
// (engine/darkplaces/touch_lua_svapi.c calls these by name)
float touch_lua_api(float op, entity e, float f, vector v, string s)
{
	switch (op)
	{
		case 1: // give / take: GiveItems words, "plus 50 rockets", "no devastator"
		{
			if (!IS_PLAYER(e))
				return 0;
			float n = tokenize_console(s);
			return GiveItems(e, 0, n);
		}
		case 2: // respawn
			if (!IS_CLIENT(e))
				return 0;
			PutClientInServer(e);
			return 1;
		case 3: // move to team 1..4
			if (!IS_CLIENT(e) || !teamplay)
				return 0;
			return MoveToTeam(e, f, 6);
		case 4: // teleport
			if (!e)
				return 0;
			setorigin(e, v);
			if (IS_PLAYER(e))
				e.velocity = '0 0 0';
			return 1;
		case 5: // remove an entity a mod spawned
			if (!e || IS_CLIENT(e))
				return 0;
			delete(e);
			return 1;
		case 6: // start a vote (e: the caller, NULL: the server)
		{
			string line = strcat("vote call ", s);
			float n = tokenize_console(line);
			VoteCommand(CMD_REQUEST_COMMAND, e, n, line);
			return 1;
		}
		case 7: // angles (a player's view too)
			if (!e)
				return 0;
			e.angles = v;
			if (IS_PLAYER(e))
				e.fixangle = true;
			return 1;
	}
	return 0;
}

string touch_lua_api_gametype()
{
	return MapInfo_Type_ToString(MapInfo_CurrentGametype());
}

// mod channel commands have the engine's own limits per player (touch_lua_chan.c):
// the game's command flood control must not drop a mod's hello or answers
MUTATOR_HOOKFUNCTION(touch_lua, ClientCommand_FloodControl)
{
	string c = M_ARGV(1, string);
	return c == "xtmsg" || c == "xthello" || c == "xtpage";
}
