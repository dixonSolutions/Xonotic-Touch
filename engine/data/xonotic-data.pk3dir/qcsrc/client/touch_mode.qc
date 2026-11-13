#include "touch_mode.qh"

#include <client/main.qh>
#include <client/touch_layout.qh>
#include <client/touch_defs.qh>
#include <client/touch_input.qh>
#include <client/touch_init.qh>
#include <client/touch_theme.qh>
#include <client/touch_widget.qh>
#include <common/ent_cs.qh>
#include <common/stats.qh>
#include <common/gametypes/gametype/ctf/ctf.qh>
#include <common/gametypes/gametype/keepaway/keepaway.qh>

float touch_mode_time = -1;
int touch_mode_cached, touch_mode_flags_cached, touch_mode_ctl_cached;

// An engine without touch_hudmode.c (an older build): the same rules from
// this CSQC's own globals, so the HUD still works, only without the shared
// source.
int Touch_Mode_Local(void)
{
	if (mv_active)
		return TOUCHMODE_MAPVOTE;
	if (intermission)
		return TOUCHMODE_INTERMISSION;
	if (spectatee_status)
	{
		if (entcs_GetSpecState(player_localnum) == ENTCS_SPEC_IN_SCOREBOARD)
			return spectatee_status > 0 ? TOUCHMODE_OUT : TOUCHMODE_OUT_FREE;
		return spectatee_status == -1 ? TOUCHMODE_OBSERVING : TOUCHMODE_FOLLOWING;
	}
	return STAT(HEALTH) > 0 ? TOUCHMODE_PLAYING : TOUCHMODE_DEAD;
}

void Touch_Mode_Read(void)
{
	if (touch_mode_time == time)
		return;
	touch_mode_time = time;
	if (!cvar_type("_touch_hud_mode"))
	{
		touch_mode_cached = Touch_Mode_Local();
		touch_mode_flags_cached = 0;
		touch_mode_ctl_cached = -1; // every control, as before
		return;
	}
	string m = cvar_string("_touch_hud_mode");
	switch (m)
	{
		case "playing": touch_mode_cached = TOUCHMODE_PLAYING; break;
		case "dead": touch_mode_cached = TOUCHMODE_DEAD; break;
		case "spectating": touch_mode_cached = TOUCHMODE_OBSERVING; break;
		case "following": touch_mode_cached = TOUCHMODE_FOLLOWING; break;
		case "out": touch_mode_cached = TOUCHMODE_OUT; break;
		case "out_free": touch_mode_cached = TOUCHMODE_OUT_FREE; break;
		case "intermission": touch_mode_cached = TOUCHMODE_INTERMISSION; break;
		case "mapvote": touch_mode_cached = TOUCHMODE_MAPVOTE; break;
		default: touch_mode_cached = TOUCHMODE_OFF; break;
	}
	touch_mode_flags_cached = cvar("_touch_hud_flags");
	touch_mode_ctl_cached = cvar("_touch_hud_controls");
	// The engine publishes from its draw, after this CSQC has drawn: in the
	// first frame of a match it still says "off". Play until it catches up.
	if (touch_mode_cached == TOUCHMODE_OFF)
	{
		touch_mode_cached = Touch_Mode_Local();
		touch_mode_ctl_cached = -1;
	}
}

int Touch_Mode(void)
{
	Touch_Mode_Read();
	return touch_mode_cached;
}

int Touch_ModeFlags(void)
{
	Touch_Mode_Read();
	return touch_mode_flags_cached;
}

bool Touch_ModeHas(int ctl)
{
	Touch_Mode_Read();
	return (touch_mode_ctl_cached & ctl) != 0;
}

string Touch_ModeTarget(void)
{
	return cvar_string("_touch_hud_target");
}

float touch_dp_time = -1, touch_dp_cached;
float Touch_DpUnits(void)
{
	// asked by every control several times a frame: worked out once a frame
	if (touch_dp_time == time)
		return touch_dp_cached;
	touch_dp_time = time;
	float vh = cvar("vid_height"), vw = cvar("vid_width");
	float short_px = min(vw, vh);
	float dpi = cvar("vid_touchscreen_ydpi");
	float d;
	if (!cvar("vid_touchscreen") && !cvar("vid_touchscreen_detected"))
		d = 1; // a desk with no touchscreen: a mouse needs no finger-sized targets
	else if (dpi > 1 && dpi != 160)
		d = dpi / 160;
	else if (short_px >= 900)
		d = short_px / 420;
	else
		d = 1;
	d = bound(1, d, 4);
	vector ui = Touch_UISize();
	touch_dp_cached = d * (vh > 0 ? ui.y / vh : 1);
	return touch_dp_cached;
}

float touch_hint_round_sent = -1, touch_hint_carry_sent = -1, touch_hint_queued_sent = -1;

void Touch_Mode_PublishHints(void)
{
	if (!cvar_type("_touch_hint_round"))
		return;
	float rs = STAT(ROUNDSTARTTIME);
	float round_live = (rs > 0 && time >= rs && !intermission) ? 1 : 0;
	int s = STAT(OBJECTIVE_STATUS);
	float carry = 0;
	if (ISGAMETYPE(CTF))
		carry = ((s & CTF_RED_FLAG_CARRYING) == CTF_RED_FLAG_CARRYING || (s & CTF_BLUE_FLAG_CARRYING) == CTF_BLUE_FLAG_CARRYING
			|| (s & CTF_YELLOW_FLAG_CARRYING) == CTF_YELLOW_FLAG_CARRYING || (s & CTF_PINK_FLAG_CARRYING) == CTF_PINK_FLAG_CARRYING
			|| (s & CTF_NEUTRAL_FLAG_CARRYING) == CTF_NEUTRAL_FLAG_CARRYING) ? 1 : 0;
	else if (ISGAMETYPE(KEEPAWAY))
		carry = (s & KA_CARRYING) ? 1 : 0;
	float queued = (teamplay && spectatee_status && entcs_GetWantsJoin(player_localnum)) ? 1 : 0;
	if (round_live != touch_hint_round_sent)
	{
		touch_hint_round_sent = round_live;
		cvar_set("_touch_hint_round", ftos(round_live));
	}
	if (carry != touch_hint_carry_sent)
	{
		touch_hint_carry_sent = carry;
		cvar_set("_touch_hint_carry", ftos(carry));
	}
	if (queued != touch_hint_queued_sent)
	{
		touch_hint_queued_sent = queued;
		cvar_set("_touch_hint_queued", ftos(queued));
	}
}

// ---------------------------------------------------------------------------
// Battle extras: MOVE TO SPECTATORS (touch_tospec_*, off until placed: the
// MENU sheet has it too), READY in warmup, RESPAWN while dead.
// ---------------------------------------------------------------------------
float touch_tospec_armed_until;
float touch_chrome_touch_time;
int touch_chrome_mode_seen = -1;

bool Touch_ToSpec_Shown(void)
{
	return Touch_ModeHas(TOUCHCTL_TOSPEC) && cvar("touch_tospec_visible");
}

bool Touch_ToSpec_Hit(vector p)
{
	return Touch_ToSpec_Shown() && Touch_HitRect(p, cvar_or("touch_tospec_x", 0.075), cvar_or("touch_tospec_y", 0.290),
		cvar_or("touch_tospec_size", 0.060), cvar_or("touch_tospec_aspect", 2.4), 1.15);
}

vector Touch_Ready_Center(void)
{
	vector ui = Touch_UISize();
	float u = Touch_DpUnits();
	return eX * (ui.x * 0.5) + eY * (ui.y - TOUCHDP_PILL_HEIGHT * 0.5 * u - 10 * u);
}

vector Touch_Ready_Half(void)
{
	float hy = TOUCHDP_PILL_HEIGHT * 0.5 * Touch_DpUnits();
	return eX * (hy * 2.6) + eY * hy;
}

bool Touch_Ready_Hit(vector p)
{
	return Touch_ModeHas(TOUCHCTL_READY) && Touch_HitRectPx(p, Touch_Ready_Center(), Touch_Ready_Half(), 1.1);
}

// A tap: lands and lifts on it. MOVE TO SPECTATORS asks for a second tap when
// the engine says it would forfeit the round or the objective carried.
void Touch_Extra_OnUp(int role, vector down, vector up)
{
	vector d = up - down;
	d.z = 0;
	if (vlen(d) > 24)
		return;
	if (role == TOUCH_ROLE_READY && Touch_Ready_Hit(up))
		localcmd("ready\n");
	else if (role == TOUCH_ROLE_TOSPEC && Touch_ToSpec_Hit(up))
	{
		if (strstrofs(cvar_string("_touch_hud_actions"), "spectate:confirm", 0) >= 0 && time >= touch_tospec_armed_until)
		{
			touch_tospec_armed_until = time + 3;
			return;
		}
		touch_tospec_armed_until = 0;
		localcmd("touch_action spectate confirm\n");
	}
}

void Touch_Extras_Draw(float alpha)
{
	if (Touch_ToSpec_Shown())
	{
		bool armed = time < touch_tospec_armed_until;
		Touch_Widget_ChromePill(Touch_WidgetCenter(cvar_or("touch_tospec_x", 0.075), cvar_or("touch_tospec_y", 0.290), 0),
			Touch_WidgetHalfExtents(cvar_or("touch_tospec_size", 0.060), cvar_or("touch_tospec_aspect", 2.4)),
			(armed ? "SURE?" : "SPECTATE"), armed || Touch_RoleOwned(TOUCH_ROLE_TOSPEC), alpha, (armed ? TOUCH_C_WARNING : TOUCH_C_ACCENT));
	}
	if (Touch_ModeHas(TOUCHCTL_READY))
		Touch_Widget_Capsule(Touch_Ready_Center(), Touch_Ready_Half(), "READY", Touch_RoleOwned(TOUCH_ROLE_READY), false, alpha, TOUCH_C_SUCCESS);
}

void Touch_DrawRespawn(float alpha)
{
	vector c = Touch_WidgetCenter(autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size);
	vector half = Touch_WidgetHalfExtents(autocvar_touch_jump_size, autocvar_touch_jump_aspect * 1.25);
	Touch_Widget_Capsule(c, half, "RESPAWN", Touch_RoleOwned(TOUCH_ROLE_RESPAWN), false, alpha, TOUCH_C_SUCCESS);
}

float Touch_ChromeAlpha(float alpha)
{
	int m = Touch_Mode();
	if (m != touch_chrome_mode_seen)
	{
		touch_chrome_mode_seen = m;
		touch_chrome_touch_time = time;
	}
	if (Touch_RoleOwned(TOUCH_ROLE_PAUSE) || Touch_RoleOwned(TOUCH_ROLE_CHAT) || Touch_RoleOwned(TOUCH_ROLE_SCORES))
		touch_chrome_touch_time = time;
	float idle_after = cvar_or("touch_chrome_idle", 6);
	if (m != TOUCHMODE_PLAYING || idle_after <= 0)
		return alpha;
	float idle = time - touch_chrome_touch_time - idle_after;
	if (idle <= 0)
		return alpha;
	float t = cvar("touch_reduce_motion") ? 1 : bound(0, idle / 0.4, 1);
	return alpha * (1 - 0.45 * t);
}
