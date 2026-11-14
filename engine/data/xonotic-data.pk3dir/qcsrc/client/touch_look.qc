#include "touch_look.qh"
#include "touch_init.qh"
#include "touch_defs.qh"
#include "touch_feel.qh"
#include "touch_mode.qh"

vector touch_look_owed;    // degrees turned by the finger, not yet applied (x yaw, y pitch)
float touch_look_speed;    // smoothed finger speed, dp/s
bool touch_look_primed;
vector touch_look_down_pos;
bool touch_look_escaped;
bool touch_look_active;
float touch_look_prev_time;
bool touch_look_owns_sensitivity;

void Touch_Look_Reset(void)
{
	touch_look_owed = '0 0 0';
	touch_look_speed = 0;
	touch_look_primed = false;
	touch_look_escaped = false;
	touch_look_active = false;
	touch_look_down_pos = '0 0 0';
	touch_look_prev_time = time;
}

void Touch_Look_BeginGesture(vector down_pos)
{
	touch_look_down_pos = down_pos;
	touch_look_escaped = false;
	// a second finger joining a look in progress adds to it: nothing owed is dropped
	if (!touch_look_active)
	{
		touch_look_owed = '0 0 0';
		touch_look_speed = 0;
		touch_look_primed = false;
	}
	touch_look_active = true;
}

void Touch_Look_BeginGestureEscaped(vector pos)
{
	// For a finger that has already proven it is dragging (a FIRE button slide),
	// re-running the deadzone would throw away a frame of aim.
	Touch_Look_BeginGesture(pos);
	touch_look_escaped = true;
}

bool Touch_Look_Escaped(void)
{
	return touch_look_escaped;
}

void Touch_Look_EndGesture(void)
{
	// the finger lifted: the turn still owed is paid at once
	if (touch_look_active && touch_look_owed != '0 0 0')
		Touch_Look_Turn(touch_look_owed.x, touch_look_owed.y);
	touch_look_owed = '0 0 0';
	touch_look_primed = false;
	touch_look_active = false;
	touch_look_escaped = false;
}

void Touch_Look_SetOwnership(bool own)
{
	// Prefer setsensitivityscale over cvar_set("sensitivity") so config.cfg never
	// archives sensitivity 0 (B3). GetCurrentFov must honor Touch_Look_BlocksMouse().
	// Not while the engine holds a mouse to aim with (vid_touchscreen_mouselook):
	// then the mouse turns the view as it does without touch, and fingers
	// alongside it. Zeroing it here left a keyboard-and-mouse player unable to
	// look around after a single tap on the screen.
	if (own && autocvar_touch_look_owns_view && !cvar("vid_touchscreen_mouselook"))
		touch_look_owns_sensitivity = true;
	else
		touch_look_owns_sensitivity = false;
}

bool Touch_Look_BlocksMouse(void)
{
	return touch_look_owns_sensitivity;
}

void Touch_Look_ApplyDelta(float dx, float dy)
{
	// Legacy entry point: treat as one raw pixel sample at ~30 Hz.
	Touch_Look_Update(dx * eX + dy * eY, 1.0 / 30.0);
}

// The look, as engine/darkplaces/touch_feel.h (TouchLook_Step): degrees per dp
// of finger travel, so the same swipe in centimetres turns the same angle on
// every screen; a gain that rises with finger speed (touch_look_accel); and a
// smoothing whose cutoff rises with speed (the 1 euro filter's idea, Casiez et
// al., CHI 2012) applied to the turn still owed, so nothing a finger moved is
// lost when it lifts.
const float TOUCH_TWO_PI = 6.28318530717958647693;

float Touch_Look_Alpha(float cutoff, float dt)
{
	float tau = 1.0 / (TOUCH_TWO_PI * max(cutoff, 0.0001));
	return 1.0 / (1.0 + tau / dt);
}

float Touch_Look_Gain(float speed)
{
	float accel = cvar("touch_look_accel");
	float lo = cvar("touch_look_accel_lo"), hi = cvar("touch_look_accel_hi");
	if (accel <= 0 || hi <= lo)
		return 1;
	float t = bound(0, (speed - lo) / (hi - lo), 1);
	return 1 + accel * t * t * (3 - 2 * t);
}

void Touch_Look_Turn(float yaw, float pitch)
{
	if (autocvar_touch_invert_y)
		pitch = -pitch;
	// Aim friction: the engine's touch aim assist (touch_aim.c) slows the drag
	// while the crosshair is on or near an enemy. It publishes the multiplier
	// because it also runs on servers whose CSQC is not this one.
	float assist = cvar("_cl_touch_aim_lookscale");
	if (assist > 0 && assist < 1)
	{
		yaw *= assist;
		pitch *= assist;
	}
	vector ang = getpropertyvec(VF_CL_VIEWANGLES);
	ang.y -= yaw;
	ang.x += pitch;
	ang.x = bound(-89, ang.x, 89);
	ang.z = 0;
	setproperty(VF_CL_VIEWANGLES, ang);
}

void Touch_Look_Update(vector raw_px, float dt)
{
	if (!touch_look_active)
		return;

	dt = bound(1.0 / 240.0, dt, 0.1);
	raw_px.z = 0;
	// console units to dp
	vector d = raw_px * (1 / max(0.001, Touch_DpUnits()));
	float v = vlen(d) / dt;
	if (!touch_look_primed)
	{
		touch_look_speed = v;
		touch_look_primed = true;
	}
	else
		touch_look_speed += (v - touch_look_speed) * Touch_Look_Alpha(TOUCHFEEL_SPEED_HZ, dt);
	float sens = autocvar_touch_sens_base;
	float accel = max(0, cvar("touch_look_accel"));
	float g = Touch_Look_Gain(touch_look_speed);
	touch_look_owed.x += d.x * TOUCHFEEL_DEG_PER_DP * sens * g;
	touch_look_owed.y += d.y * TOUCHFEEL_DEG_PER_DP * sens * autocvar_touch_sens_y_mult * g;
	float a = 1;
	if (autocvar_touch_look_filter == 1)
		a = Touch_Look_Alpha(max(autocvar_touch_look_fcmin, 0.01) + max(autocvar_touch_look_beta, 0) * touch_look_speed, dt);
	else if (autocvar_touch_look_filter >= 2)
		a = Touch_Look_Alpha(max(autocvar_touch_look_fcmin, 0.01), dt);
	vector out = touch_look_owed * a;
	// a clamp for contacts that jump, not a limit on turning
	float cap = autocvar_touch_look_max_deg_per_s * max(1, sens / 2.8) * (1 + accel) * dt;
	if (cap > 0)
	{
		out.x = bound(-cap, out.x, cap);
		out.y = bound(-cap, out.y, cap);
	}
	touch_look_owed -= out;
	Touch_Look_Turn(out.x, out.y);
}

void Touch_Look_Frame(vector accum_px, vector last_pos, bool first_frame, bool active)
{
	float dt = bound(1.0 / 240.0, time - touch_look_prev_time, 0.1);
	touch_look_prev_time = time;

	if (!active)
	{
		Touch_Look_EndGesture();
		return;
	}

	if (!touch_look_active)
		Touch_Look_BeginGesture(last_pos - accum_px);

	if (first_frame)
		return;

	if (!touch_look_escaped)
	{
		vector from_down = last_pos - touch_look_down_pos;
		from_down.z = 0;
		if (vlen(from_down) < autocvar_touch_look_deadzone_px)
			return;
		touch_look_escaped = true;
		// all of the travel that proved it a drag: half of it used to be
		// dropped here, which a flick (most of it in one frame) felt
		accum_px = from_down;
	}

	Touch_Look_Update(accum_px, dt);
}
