/*
Xonotic Touch: the quick-turn button (see touch_turn.h).

Always the same way round, to the right (clockwise seen from above): the turn
is for "what is behind me", which a half turn reaches either way, and a turn
that always goes the same way is one the thumb learns. The yaw is changed by
the eased share of the turn due this frame, so a look drag in the same frames
adds to it rather than fighting it, and a server's fixangle (a teleport, a
respawn) ends it.
*/

#include "quakedef.h"
#include "touch_aim.h"
#include "touch_turn.h"

extern cvar_t vid_touchscreen;
extern cvar_t cl_touch_aim_debug;

#define TURN_ANGLE_MIN 90.0f
#define TURN_ANGLE_MAX 360.0f
#define TURN_TIME_MAX  0.5f

static struct
{
	qbool active;
	double start;
	float total;    // degrees of this turn (more if tapped again)
	float done;     // degrees turned so far
	float duration;
} tt;

static float cvf(const char *name, float def)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	return var ? var->value : def;
}

static float ease(float u)
{
	u = bound(0.0f, u, 1.0f);
	return u * u * (3 - 2 * u);
}

qbool TouchTurn_ButtonShown(void)
{
	return cvf("touch_turn_visible", 1) != 0 && vid_touchscreen.integer && !vid_touchscreen_mouselook.integer;
}

qbool TouchTurn_Spinning(void)
{
	return tt.active;
}

void TouchTurn_Start(void)
{
	float angle = bound(TURN_ANGLE_MIN, cvf("touch_turn_angle", 180), TURN_ANGLE_MAX);
	const char *blocked = TouchAim_BlockReason();
	if (blocked)
	{
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch turn: not now: %s\n", blocked);
		return;
	}
	if (tt.active)
	{
		// tapped again mid-turn: the rest of this one plus a whole new one,
		// from here
		tt.total = tt.total - tt.done + angle;
		tt.done = 0;
	}
	else
	{
		tt.total = angle;
		tt.done = 0;
	}
	tt.active = true;
	tt.start = host.realtime;
	tt.duration = bound(0.0f, cvf("touch_turn_time", 0.12f), TURN_TIME_MAX);
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch turn: %.0f degrees in %.2f s from yaw %.1f\n", tt.total, tt.duration, cl.viewangles[YAW]);
}

static void TouchTurn_f(cmd_state_t *cmd)
{
	TouchTurn_Start();
}

void TouchTurn_Frame(void)
{
	float want, step;
	if (!tt.active)
		return;
	if (cl.fixangle[0] || TouchAim_BlockReason())
	{
		tt.active = false; // a teleport, a respawn, a menu: the turn is off
		return;
	}
	want = tt.duration > 0 ? tt.total * ease((float)((host.realtime - tt.start) / tt.duration)) : tt.total;
	step = want - tt.done;
	cl.viewangles[YAW] -= step;
	tt.done = want;
	if (tt.done >= tt.total - 0.001f)
	{
		tt.active = false;
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch turn: done, yaw %.1f\n", cl.viewangles[YAW]);
	}
}

void TouchTurn_Init(void)
{
	Cmd_AddCommand(CF_CLIENT, "touch_turn", TouchTurn_f, "turn the view round by touch_turn_angle degrees (the touch controls' TURN button); touch mode only");
}
