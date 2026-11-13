#include "touch_spectate.qh"

#include <client/touch_defs.qh>
#include <client/main.qh>
#include <client/touch_draw.qh>
#include <client/touch_input.qh>
#include <client/touch_layout.qh>
#include <client/touch_mode.qh>
#include <client/touch_theme.qh>
#include <client/touch_widget.qh>

// ---------------------------------------------------------------------------
// Spectating, stripped to what a spectator uses (engine/darkplaces/
// touch_spectate.h is the table both HUDs follow): the move stick and the
// UP / DOWN rocker while free-flying, and one bar, previous | follow or free |
// next, bottom right. The server's spectator code reads +attack as "follow" /
// "next player", weapprev as "previous player", +attack2 as "back to free-fly",
// +moveup / +movedown as flying, and +jump as "join": nothing here sends
// +jump. Joining is the swipe handle at the bottom middle, which the engine
// draws and reads under every CSQC (touch_hud.c).
// ---------------------------------------------------------------------------

bool touch_spec_up_held, touch_spec_down_held;
bool touch_spec_key_up, touch_spec_key_down;

int Touch_Spec_State(void)
{
	switch (Touch_Mode())
	{
		case TOUCHMODE_OBSERVING: return TOUCH_SPEC_OBSERVING;
		case TOUCHMODE_FOLLOWING: return TOUCH_SPEC_FOLLOWING;
		case TOUCHMODE_OUT: return TOUCH_SPEC_OUT;
		case TOUCHMODE_OUT_FREE: return TOUCH_SPEC_OUT_FREE;
		default: return TOUCH_SPEC_NONE;
	}
}

// Free-flying: the stick, UP and DOWN fly the camera.
bool Touch_Spec_Flying(int st)
{
	return st == TOUCH_SPEC_OBSERVING || st == TOUCH_SPEC_OUT_FREE;
}

// TouchSpec_BarAction
int Touch_Spec_BarAction(int st, int part)
{
	if (Touch_Spec_Flying(st))
		return TOUCH_SPEC_ACT_FOLLOW;
	if (st == TOUCH_SPEC_FOLLOWING)
		return part == TOUCH_SPEC_BAR_PREV ? TOUCH_SPEC_ACT_PREV : part == TOUCH_SPEC_BAR_MAIN ? TOUCH_SPEC_ACT_FREE : TOUCH_SPEC_ACT_NEXT;
	if (st == TOUCH_SPEC_OUT)
		return part == TOUCH_SPEC_BAR_PREV ? TOUCH_SPEC_ACT_PREV : part == TOUCH_SPEC_BAR_MAIN ? TOUCH_SPEC_ACT_NONE : TOUCH_SPEC_ACT_NEXT;
	return TOUCH_SPEC_ACT_NONE;
}

string Touch_Spec_BarLabel(int st, int part)
{
	int act = Touch_Spec_BarAction(st, part);
	if (act == TOUCH_SPEC_ACT_NONE)
		return (part == TOUCH_SPEC_BAR_MAIN && st != TOUCH_SPEC_NONE) ? "WATCHING" : "";
	if (part == TOUCH_SPEC_BAR_PREV)
		return "<";
	if (part == TOUCH_SPEC_BAR_NEXT)
		return ">";
	if (act == TOUCH_SPEC_ACT_FREE)
		return "FREE";
	return "FOLLOW";
}

// The bar, as touch_hud.c spec_bar_part: a 46 dp row (touch_specbar_*).
vector Touch_Spec_BarHalf(int part)
{
	float hy = max(TOUCHDP_SECONDARY * 0.5 * Touch_DpUnits(),
		Touch_ScreenMin() * cvar_or("touch_specbar_size", 0.085) * 0.5 * Touch_WidgetScale());
	if (part == TOUCH_SPEC_BAR_MAIN)
		return eX * (hy * 2.2) + eY * hy;
	return eX * hy + eY * hy;
}

vector Touch_Spec_BarCenter(int part)
{
	vector c = Touch_WidgetCenter(cvar_or("touch_specbar_x", 0.815), cvar_or("touch_specbar_y", 0.885), 0);
	if (part == TOUCH_SPEC_BAR_MAIN)
		return c;
	float hy = Touch_Spec_BarHalf(TOUCH_SPEC_BAR_MAIN).y;
	return c + eX * ((part == TOUCH_SPEC_BAR_PREV ? -1 : 1) * (hy * 2.2 + hy * 0.25 + hy));
}

// The fly rocker: UP over DOWN, beside the stick on its inner side.
vector Touch_Spec_FlyCenter(bool up)
{
	vector m = Touch_WidgetCenter(autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size);
	float mr = Touch_WidgetRadius(autocvar_touch_move_size);
	float r = TOUCHDP_MIN_TARGET * 0.5 * Touch_DpUnits();
	return m + eX * ((autocvar_touch_handedness ? -1 : 1) * (mr + r * 1.6)) + eY * ((up ? -1 : 1) * r * 1.15);
}

float Touch_Spec_FlyRadius(void)
{
	return TOUCHDP_MIN_TARGET * 0.5 * Touch_DpUnits();
}

int Touch_Spec_ActionRole(int act)
{
	switch (act)
	{
		case TOUCH_SPEC_ACT_FOLLOW:
		case TOUCH_SPEC_ACT_NEXT: return TOUCH_ROLE_SPEC_ATTACK;
		case TOUCH_SPEC_ACT_PREV: return TOUCH_ROLE_SPEC_PREV;
		case TOUCH_SPEC_ACT_FREE: return TOUCH_ROLE_SPEC_FREE;
		default: return TOUCH_ROLE_IGNORED;
	}
}

int Touch_Spec_AssignRole(vector p)
{
	int st = Touch_Spec_State();
	if (st == TOUCH_SPEC_NONE || touch_desk)
		return TOUCH_ROLE_NONE;
	if (Touch_InEdgeDeadzone(p))
		return TOUCH_ROLE_IGNORED;
	for (int part = 0; part < TOUCH_SPEC_BAR_PARTS; ++part)
		if (Touch_HitRectPx(p, Touch_Spec_BarCenter(part), Touch_Spec_BarHalf(part), 1.1))
			return Touch_Spec_ActionRole(Touch_Spec_BarAction(st, part));
	if (Touch_Spec_Flying(st))
	{
		float r = Touch_Spec_FlyRadius() * 1.05;
		if (Touch_PointInCircle(p, Touch_Spec_FlyCenter(true), r))
			return TOUCH_ROLE_SPEC_UP;
		if (Touch_PointInCircle(p, Touch_Spec_FlyCenter(false), r))
			return TOUCH_ROLE_SPEC_DOWN;
		// The stick flies the camera while observing; following, the view is theirs.
		if (Touch_InMoveZone(p)
			|| (autocvar_touch_move_visible && Touch_HitCircle(p, autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size, 1.0)))
			return Touch_RoleOwned(TOUCH_ROLE_MOVE) ? TOUCH_ROLE_IGNORED : TOUCH_ROLE_MOVE;
	}
	return Touch_RoleOwned(TOUCH_ROLE_LOOK) ? TOUCH_ROLE_IGNORED : TOUCH_ROLE_LOOK;
}

bool Touch_Spec_FingerFrame(int role, bool first)
{
	if (role == TOUCH_ROLE_SPEC_ATTACK)
		touch_spec_attack = true;
	else if (role == TOUCH_ROLE_SPEC_FREE)
		touch_spec_attack2 = true;
	else if (role == TOUCH_ROLE_SPEC_UP)
		touch_spec_up_held = true;
	else if (role == TOUCH_ROLE_SPEC_DOWN)
		touch_spec_down_held = true;
	else if (role == TOUCH_ROLE_SPEC_PREV)
	{
		if (first)
			localcmd("weapprev\n");
	}
	else
		return false;
	return true;
}

void Touch_Spec_SetMoveKey(string key, bool down, bool was)
{
	if (down != was)
		localcmd(strcat((down ? "+" : "-"), key, " ", TOUCH_HOLD_KEY, "\n"));
}

void Touch_Spec_Release(void)
{
	Touch_Spec_SetMoveKey("moveup", false, touch_spec_key_up);
	Touch_Spec_SetMoveKey("movedown", false, touch_spec_key_down);
	touch_spec_key_up = false;
	touch_spec_key_down = false;
}

void Touch_Spec_Frame(void)
{
	int st = Touch_Spec_State();
	bool up = touch_spec_up_held && Touch_Spec_Flying(st);
	bool down = touch_spec_down_held && Touch_Spec_Flying(st);
	Touch_Spec_SetMoveKey("moveup", up, touch_spec_key_up);
	Touch_Spec_SetMoveKey("movedown", down, touch_spec_key_down);
	touch_spec_key_up = up;
	touch_spec_key_down = down;
	// Never +jump: that is the join, the engine's swipe handle alone.
	touch_spec_jump = false;
	touch_spec_up_held = false;
	touch_spec_down_held = false;
}

bool Touch_Spec_DrawOverlay(float alpha)
{
	int st = Touch_Spec_State();
	if (st == TOUCH_SPEC_NONE)
		return false;
	if (Touch_Spec_Flying(st))
	{
		if (autocvar_touch_move_visible)
			Touch_DrawMoveStick(alpha);
		float r = Touch_Spec_FlyRadius();
		Touch_Widget_Button(Touch_Spec_FlyCenter(true), r, "UP", touch_spec_key_up, alpha * 0.9, TOUCH_C_ACCENT);
		Touch_Widget_Button(Touch_Spec_FlyCenter(false), r, "DOWN", touch_spec_key_down, alpha * 0.9, TOUCH_C_ACCENT);
	}
	for (int part = 0; part < TOUCH_SPEC_BAR_PARTS; ++part)
	{
		string label = Touch_Spec_BarLabel(st, part);
		if (label == "")
			continue;
		int act = Touch_Spec_BarAction(st, part);
		bool held = Touch_RoleOwned(Touch_Spec_ActionRole(act)) && act != TOUCH_SPEC_ACT_NONE;
		Touch_Widget_Capsule(Touch_Spec_BarCenter(part), Touch_Spec_BarHalf(part), label, held, false,
			(act == TOUCH_SPEC_ACT_NONE ? alpha * 0.6 : alpha), TOUCH_C_ACCENT);
	}
	// who is being watched, over the bar
	string target = Touch_ModeTarget();
	if (target != "")
	{
		vector c = Touch_Spec_BarCenter(TOUCH_SPEC_BAR_MAIN);
		float hy = Touch_Spec_BarHalf(TOUCH_SPEC_BAR_MAIN).y;
		float fs = bound(9, hy * 0.62, 20);
		vector fsz = eX * fs + eY * fs;
		float w = stringwidth(target, false, fsz);
		vector at = c - eY * (hy * 1.75);
		drawfill(at - eX * (w * 0.5 + fs * 0.6) - eY * (fs * 0.75), eX * (w + fs * 1.2) + eY * (fs * 1.5), TOUCH_C_SURFACE, alpha * 0.6, DRAWFLAG_NORMAL);
		drawstring(at - eX * (w * 0.5) - eY * (fs * 0.5), target, fsz, '1 1 1', alpha, DRAWFLAG_NORMAL);
	}
	return true;
}

string Touch_Spec_CommandLabel(string command)
{
	int st = Touch_Spec_State();
	if (st == TOUCH_SPEC_NONE || !Touch_Active())
		return "";
	if (command == "+jump")
		return (st == TOUCH_SPEC_OBSERVING || st == TOUCH_SPEC_FOLLOWING) ? "SWIPE UP" : "";
	if (command == "+fire" || command == "+attack")
		return Touch_Spec_Flying(st) ? "FOLLOW" : ">";
	// flying, next / previous weapon change the speed: no button for that
	if (command == "weapnext")
		return Touch_Spec_Flying(st) ? "" : ">";
	if (command == "weapprev")
		return Touch_Spec_Flying(st) ? "" : "<";
	if (command == "+fire2")
		return st == TOUCH_SPEC_FOLLOWING ? "FREE" : "";
	return "";
}
