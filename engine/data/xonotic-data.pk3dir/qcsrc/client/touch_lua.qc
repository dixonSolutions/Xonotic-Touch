// Godnotic: what Lua client mods get from this game's own client code
// (engine/darkplaces/touch_lua_clapi.c, docs/LUA_API.md).
//
// - the client program's mutator hooks (client/touch_lua_hooks.qc, generated)
// - events only the game code knows: notifications (the kill feed, centered
//   messages; a mod may hide one), hits, pickups, the match state, the HUD pass
//
// On a server that sends its own client code none of this runs, and client
// mods see xt.csqc() == "foreign". Every call checks for the engine first.

#include "touch_lua.qh"

float touch_lua_clevent(string ev, float a, float b, string s1, string s2) = #1740;
float touch_lua_hook_gen(float signature) = #1751;
float touch_lua_hook_wanted(float hook) = #1752;
float touch_lua_hook_run(float hook, ...) = #1753;
void touch_lua_hook_more(...) = #1754;
float touch_lua_hook_changed(float slot) = #1747;
float touch_lua_hook_outf(float slot) = #1748;
vector touch_lua_hook_outv(float slot) = #1749;
string touch_lua_hook_outs(float slot) = #1750;

bool touch_luacl_checked;
bool touch_luacl_present;

bool touch_luacl_on()
{
	if (!touch_luacl_checked)
	{
		touch_luacl_checked = true;
		touch_luacl_present = (cvar_type("cl_lua_enable") & CVAR_TYPEFLAG_ENGINE) != 0;
	}
	return touch_luacl_present && cvar("cl_lua_enable");
}

#include <client/touch_lua_hooks.qc>

REGISTER_MUTATOR(touch_luacl, true);

// common/notifications/all.qc (compiled after this file)
string Local_Notification_sprintf(string input, string args, string s1, string s2, string s3, string s4, int f1, float f2, float f3, float f4);

// common/notifications/all.qc calls this (one // Touch: line) for every
// notification this client shows; true hides it
bool touch_luacl_notify(MSG net_type, entity notif, string s1, string s2, string s3, string s4, float f1, float f2, float f3, float f4)
{
	if (!touch_luacl_on())
		return false;
	string kind;
	switch (net_type)
	{
		case MSG_INFO: kind = "info"; break;
		case MSG_CENTER: kind = "center"; break;
		case MSG_ANNCE: kind = "announce"; break;
		case MSG_MULTI: kind = "multi"; break;
		case MSG_CHOICE: kind = "choice"; break;
		default: kind = "other"; break;
	}
	string text = "";
	if (net_type == MSG_INFO || net_type == MSG_CENTER)
		text = Local_Notification_sprintf(notif.nent_string, notif.nent_args, s1, s2, s3, s4, f1, f2, f3, f4);
	// the arguments as one tab-separated string: s1..s4 then f1..f4
	string args = strcat(s1, "\t", s2, "\t", s3, "\t", s4, "\t", strcat(ftos(f1), "\t", ftos(f2), "\t", ftos(f3), "\t", ftos(f4)));
	return touch_lua_clevent(strcat("notification ", kind), 0, 0, strcat(Get_Notif_Name(notif), "\n", text), args) != 0;
}

string touch_luacl_state;
float touch_luacl_hit_prev;
float touch_luacl_pickup_prev;
float touch_luacl_damage_prev;

void TouchLua_CL_Frame()
{
	if (!touch_luacl_on())
		return;
	if (!touch_lua_notify_hook)
		touch_lua_notify_hook = touch_luacl_notify;
	touch_luacl_hooks_sync();

	string st = "playing";
	if (intermission)
		st = "ended";
	else if (warmup_stage)
		st = "warmup";
	else if (time < STAT(GAMESTARTTIME))
		st = "countdown";
	else if (STAT(OVERTIMES) > 0)
		st = "overtime";
	if (st != touch_luacl_state)
	{
		string old = touch_luacl_state;
		touch_luacl_state = st;
		touch_lua_clevent("match", 0, 0, st, old);
	}

	float hit = STAT(HIT_TIME);
	if (hit != touch_luacl_hit_prev)
	{
		float total = STAT(HITSOUND_DAMAGE_DEALT_TOTAL);
		if (touch_luacl_hit_prev)
			touch_lua_clevent("hit", total - touch_luacl_damage_prev, 0, "", "");
		touch_luacl_hit_prev = hit;
		touch_luacl_damage_prev = total;
	}

	float pick = STAT(LAST_PICKUP);
	if (pick != touch_luacl_pickup_prev)
	{
		if (touch_luacl_pickup_prev && last_pickup_item)
			touch_lua_clevent("pickup", 0, 0, last_pickup_item.netname, "");
		touch_luacl_pickup_prev = pick;
	}

	touch_lua_clevent("hud_overlay", vid_conwidth, vid_conheight, "", "");
	if (scoreboard_active)
		touch_lua_clevent("scoreboard", vid_conwidth, vid_conheight, "", "");
}
