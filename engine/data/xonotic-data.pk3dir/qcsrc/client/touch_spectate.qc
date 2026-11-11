#include "touch_spectate.qh"

#include <client/touch_defs.qh>
#include <client/main.qh>
#include <common/ent_cs.qh>
#include <client/touch_draw.qh>
#include <client/touch_input.qh>
#include <client/touch_layout.qh>
#include <client/touch_theme.qh>
#include <client/touch_widget.qh>

// ---------------------------------------------------------------------------
// The server's spectator code (ObserverOrSpectatorThink) reads +attack as
// "follow" / "next player", weapprev as "previous player", +attack2 as "back to
// free-fly", +moveup / +movedown as flying up / down, and +jump as "join" in
// every state. So only JOIN sends +jump, and it has to be held.
// ---------------------------------------------------------------------------

bool touch_spec_up_held, touch_spec_down_held, touch_spec_join_held;
bool touch_spec_key_up, touch_spec_key_down;
float touch_spec_join_start;   // finger on JOIN since, 0 none
float touch_spec_join_sent;    // join sent at, 0 not
float touch_spec_join_locked;  // shown as locked until

int Touch_Spec_State(void)
{
	if (!spectatee_status || intermission)
		return TOUCH_SPEC_NONE;
	// in the game's scoreboard but out of the round (Clan Arena and the like)
	if (entcs_GetSpecState(player_localnum) == ENTCS_SPEC_IN_SCOREBOARD)
		return spectatee_status > 0 ? TOUCH_SPEC_OUT : TOUCH_SPEC_OUT_FREE;
	return spectatee_status == -1 ? TOUCH_SPEC_OBSERVING : TOUCH_SPEC_FOLLOWING;
}

// Free-flying: the stick, UP and DOWN fly the camera.
bool Touch_Spec_Flying(int st)
{
	return st == TOUCH_SPEC_OBSERVING || st == TOUCH_SPEC_OUT_FREE;
}

bool Touch_Spec_JoinShown(int st)
{
	return st == TOUCH_SPEC_OBSERVING || st == TOUCH_SPEC_FOLLOWING;
}

vector Touch_Spec_JoinCenter(void)
{
	return Touch_WidgetCenter(cvar_or("touch_spec_join_x", 0.5), cvar_or("touch_spec_join_y", 0.885), autocvar_touch_jump_size);
}

vector Touch_Spec_JoinHalf(void)
{
	return Touch_WidgetHalfExtents(autocvar_touch_jump_size, 2.8);
}

// Slot meanings, as TouchSpec_SlotAction.
string Touch_Spec_FireLabel(int st) { return Touch_Spec_Flying(st) ? "FOLLOW" : "NEXT"; }
string Touch_Spec_HopLabel(int st)
{
	if (Touch_Spec_Flying(st)) return "UP";
	if (st == TOUCH_SPEC_FOLLOWING) return "FREE";
	return "";
}

int Touch_Spec_AssignRole(vector p)
{
	int st = Touch_Spec_State();
	if (st == TOUCH_SPEC_NONE || touch_desk)
		return TOUCH_ROLE_NONE;
	if (Touch_InEdgeDeadzone(p))
		return TOUCH_ROLE_IGNORED;
	if (Touch_Spec_JoinShown(st) && Touch_HitRectPx(p, Touch_Spec_JoinCenter(), Touch_Spec_JoinHalf(), 1.1))
		return Touch_RoleOwned(TOUCH_ROLE_SPEC_JOIN) ? TOUCH_ROLE_IGNORED : TOUCH_ROLE_SPEC_JOIN;
	if (Touch_HitCircle(p, autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size, 1.15))
		return TOUCH_ROLE_SPEC_ATTACK;
	if (!Touch_Spec_Flying(st) && Touch_HitCircle(p, autocvar_touch_fire2_x, autocvar_touch_fire2_y, autocvar_touch_fire2_size, 1.15))
		return TOUCH_ROLE_SPEC_PREV;
	if (st != TOUCH_SPEC_OUT && Touch_HitRect(p, autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size, autocvar_touch_jump_aspect, 1.10))
		return Touch_Spec_Flying(st) ? TOUCH_ROLE_SPEC_UP : TOUCH_ROLE_SPEC_FREE;
	if (Touch_Spec_Flying(st) && Touch_HitCircle(p, autocvar_touch_crouch_x, autocvar_touch_crouch_y, autocvar_touch_crouch_size, 1.10))
		return TOUCH_ROLE_SPEC_DOWN;
	if (Touch_TurnAvailable() && Touch_HitCircle(p, autocvar_touch_turn_x, autocvar_touch_turn_y, autocvar_touch_turn_size, 1.10))
		return TOUCH_ROLE_TURN;
	// The stick flies the camera while observing; following, the view is theirs.
	if (Touch_Spec_Flying(st) && (Touch_InMoveZone(p)
		|| (autocvar_touch_move_visible && Touch_HitCircle(p, autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size, 1.0))))
		return Touch_RoleOwned(TOUCH_ROLE_MOVE) ? TOUCH_ROLE_IGNORED : TOUCH_ROLE_MOVE;
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
	else if (role == TOUCH_ROLE_SPEC_JOIN)
		touch_spec_join_held = true;
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
	touch_spec_join_start = 0;
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

	// JOIN, as TouchSpec_JoinFrame: held, then +jump for a moment; still a
	// spectator well after that, the server did not take it.
	touch_spec_jump = false;
	if (touch_spec_join_sent)
	{
		if (st == TOUCH_SPEC_NONE)
			touch_spec_join_sent = 0;
		else if (time - touch_spec_join_sent >= TOUCH_SPEC_JOIN_WAIT)
		{
			touch_spec_join_sent = 0;
			touch_spec_join_locked = time + TOUCH_SPEC_JOIN_LOCKED;
		}
		else
			touch_spec_jump = time - touch_spec_join_sent < TOUCH_SPEC_JOIN_PRESS;
	}
	else if (!touch_spec_join_held || !Touch_Spec_JoinShown(st))
		touch_spec_join_start = 0;
	else
	{
		if (!touch_spec_join_start)
			touch_spec_join_start = time;
		if (time - touch_spec_join_start >= TOUCH_SPEC_JOIN_HOLD)
		{
			touch_spec_join_start = 0;
			touch_spec_join_sent = time;
			touch_spec_join_locked = 0;
			touch_spec_jump = true;
		}
	}
	touch_spec_up_held = false;
	touch_spec_down_held = false;
	touch_spec_join_held = false;
}

bool Touch_Spec_DrawOverlay(float alpha)
{
	int st = Touch_Spec_State();
	if (st == TOUCH_SPEC_NONE)
		return false;
	if (Touch_Spec_Flying(st) && autocvar_touch_move_visible)
		Touch_DrawMoveStick(alpha);
	Touch_DrawCircleWidget(autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size,
		Touch_Spec_FireLabel(st), Touch_RoleOwned(TOUCH_ROLE_SPEC_ATTACK), alpha, TOUCH_C_ACCENT);
	if (!Touch_Spec_Flying(st))
		Touch_DrawCircleWidget(autocvar_touch_fire2_x, autocvar_touch_fire2_y, autocvar_touch_fire2_size,
			"PREV", Touch_RoleOwned(TOUCH_ROLE_SPEC_PREV), alpha, TOUCH_C_ACCENT);
	string hop = Touch_Spec_HopLabel(st);
	if (hop != "")
		Touch_Widget_Capsule(Touch_WidgetCenter(autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size),
			Touch_WidgetHalfExtents(autocvar_touch_jump_size, autocvar_touch_jump_aspect), hop,
			Touch_RoleOwned(TOUCH_ROLE_SPEC_UP) || Touch_RoleOwned(TOUCH_ROLE_SPEC_FREE), false, alpha, TOUCH_C_ACCENT);
	if (Touch_Spec_Flying(st))
		Touch_DrawCircleWidget(autocvar_touch_crouch_x, autocvar_touch_crouch_y, autocvar_touch_crouch_size,
			"DOWN", Touch_RoleOwned(TOUCH_ROLE_SPEC_DOWN), alpha, TOUCH_C_ACCENT);
	if (Touch_TurnAvailable())
		Touch_DrawCircleWidget(autocvar_touch_turn_x, autocvar_touch_turn_y, autocvar_touch_turn_size,
			TOUCH_LABEL_TURN, Touch_TurnHeld(), alpha, TOUCH_C_ACCENT);
	if (Touch_Spec_JoinShown(st))
	{
		bool locked = time < touch_spec_join_locked;
		vector c = Touch_Spec_JoinCenter(), half = Touch_Spec_JoinHalf();
		Touch_Widget_Capsule(c, half, (locked ? "LOCKED" : "HOLD TO JOIN"), false, false,
			(locked ? alpha * 0.45 : alpha), TOUCH_C_SUCCESS);
		if (touch_spec_join_start)
		{
			float p = bound(0, (time - touch_spec_join_start) / TOUCH_SPEC_JOIN_HOLD, 1);
			drawroundedbox(c - eX * (half.x - half.x * p), eX * (half.x * p) + eY * half.y,
				min(half.y, half.x * p), 0, TOUCH_C_SUCCESS, alpha * 0.55);
		}
	}
	return true;
}

string Touch_Spec_CommandLabel(string command)
{
	int st = Touch_Spec_State();
	if (st == TOUCH_SPEC_NONE || !Touch_Active())
		return "";
	if (command == "+jump")
		return Touch_Spec_JoinShown(st) ? "JOIN" : "";
	if (command == "+fire" || command == "+attack")
		return Touch_Spec_FireLabel(st);
	// flying, next / previous weapon change the speed: no button for that
	if (command == "weapnext")
		return Touch_Spec_Flying(st) ? "" : "NEXT";
	if (command == "weapprev")
		return Touch_Spec_Flying(st) ? "" : "PREV";
	if (command == "+fire2")
		return st == TOUCH_SPEC_FOLLOWING ? "FREE" : "";
	return "";
}
