/*
Godnotic: how a finger turns the view and how the move stick reads a thumb,
as pure functions both HUD copies follow (the engine's touch_hud.c on
servers; the port's CSQC mirrors them in qcsrc/client/touch_feel.qh, and
scripts/test-touch-controls.sh checks the constants agree and runs the
maths). docs/TOUCH_CONTROLS_REVIEW.md has the reasons.

LOOK
  Density-independent: the turn is degrees per dp of finger travel, so the
  same swipe in centimetres turns the same angle on a phone and a tablet
  (it used to be a fraction of the screen width: a 6.5" phone turned about
  1.7 times as far per centimetre as a 10" tablet at the same setting).
      degrees = dp * TOUCHFEEL_DEG_PER_DP * touch_sens_base * gain(speed)
  touch_sens_base keeps its scale: 2.8 on a 283 dpi 1920 px wide tablet is
  what 2.8 was before.
  Acceleration: gain rises smoothly from 1 below touch_look_accel_lo dp/s
  (aiming: 1:1, predictable) to 1 + touch_look_accel at touch_look_accel_hi
  dp/s and above (a flick: a turn-around in one short swipe). 0 turns it off.
  Smoothing: the turn owed is paid out through a low-pass whose cutoff rises
  with finger speed (the idea of the 1 euro filter, Casiez et al. 2012,
  applied to the displacement still owed). What is still owed when the
  finger lifts is paid at once: nothing a finger moved is ever lost, which
  the old velocity filter did on every fast, short flick.

STICK
  Floating: the stick's centre is where the thumb lands in the move zone,
  and it follows a thumb that drags past the rim, so there is no reaching
  for a fixed spot and no "stuck at the edge". Inside the dead zone nothing;
  from there the speed rises to full at TOUCHFEEL "run" deflection and the
  direction is the thumb's angle, not one of eight.
*/
#ifndef TOUCH_FEEL_H
#define TOUCH_FEEL_H

#include <math.h>

#define TOUCHFEEL_DEG_PER_DP    0.16f   // at touch_sens_base 1, gain 1
#define TOUCHFEEL_SPEED_HZ      8.0f    // low-pass on the finger speed estimate
#define TOUCHFEEL_PI            3.14159265358979f

typedef struct touchlook_s
{
	float owed_yaw, owed_pitch;   // degrees turned by the finger, not yet applied
	float speed;                  // smoothed finger speed, dp/s
	int primed;
} touchlook_t;

typedef struct touchlook_cfg_s
{
	float sens;        // touch_sens_base
	float y_mult;      // touch_sens_y_mult
	float accel;       // touch_look_accel: extra gain at speed, 0 = none
	float accel_lo;    // touch_look_accel_lo, dp/s
	float accel_hi;    // touch_look_accel_hi, dp/s
	int filter;        // touch_look_filter: 0 none, 1 adaptive, 2 fixed
	float fcmin;       // touch_look_fcmin, Hz
	float beta;        // touch_look_beta, Hz per dp/s
	float max_rate;    // degrees per second at most (a contact that jumped)
} touchlook_cfg_t;

static inline float TouchFeel_Alpha(float cutoff, float dt)
{
	float tau = 1.0f / (2.0f * TOUCHFEEL_PI * (cutoff > 0.0001f ? cutoff : 0.0001f));
	return 1.0f / (1.0f + tau / dt);
}

static inline float TouchFeel_Gain(const touchlook_cfg_t *c, float speed)
{
	float t;
	if (c->accel <= 0 || c->accel_hi <= c->accel_lo)
		return 1.0f;
	t = (speed - c->accel_lo) / (c->accel_hi - c->accel_lo);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return 1.0f + c->accel * t * t * (3.0f - 2.0f * t);
}

static inline void TouchLook_Reset(touchlook_t *s)
{
	s->owed_yaw = s->owed_pitch = s->speed = 0;
	s->primed = 0;
}

// One frame of finger travel, in dp (x right, y down). Writes the degrees to
// turn now: yaw to the right, pitch down.
static inline void TouchLook_Step(touchlook_t *s, const touchlook_cfg_t *c, float dx_dp, float dy_dp, float dt, float *yaw, float *pitch)
{
	float v, g, a = 1.0f, oy, op, cap;
	dt = dt < 1.0f / 240.0f ? 1.0f / 240.0f : dt > 0.1f ? 0.1f : dt;
	v = (float)sqrt(dx_dp * dx_dp + dy_dp * dy_dp) / dt;
	if (!s->primed)
	{
		s->speed = v;
		s->primed = 1;
	}
	else
		s->speed += (v - s->speed) * TouchFeel_Alpha(TOUCHFEEL_SPEED_HZ, dt);
	g = TouchFeel_Gain(c, s->speed);
	s->owed_yaw += dx_dp * TOUCHFEEL_DEG_PER_DP * c->sens * g;
	s->owed_pitch += dy_dp * TOUCHFEEL_DEG_PER_DP * c->sens * c->y_mult * g;
	if (c->filter == 1)
		a = TouchFeel_Alpha((c->fcmin > 0.01f ? c->fcmin : 0.01f) + (c->beta > 0 ? c->beta : 0) * s->speed, dt);
	else if (c->filter >= 2)
		a = TouchFeel_Alpha(c->fcmin > 0.01f ? c->fcmin : 0.01f, dt);
	oy = s->owed_yaw * a;
	op = s->owed_pitch * a;
	cap = c->max_rate * dt;
	if (cap > 0)
	{
		oy = oy > cap ? cap : oy < -cap ? -cap : oy;
		op = op > cap ? cap : op < -cap ? -cap : op;
	}
	s->owed_yaw -= oy;
	s->owed_pitch -= op;
	*yaw = oy;
	*pitch = op;
}

// The finger lifted: what is still owed, to be applied at once.
static inline void TouchLook_Flush(touchlook_t *s, float *yaw, float *pitch)
{
	*yaw = s->owed_yaw;
	*pitch = s->owed_pitch;
	TouchLook_Reset(s);
}

// ---------------------------------------------------------------------------
// The move stick. Positions in any one unit (console units); r is the stick's
// radius. The centre floats: TouchStick_Land puts it under the thumb (kept
// far enough from the screen edges to push every way), TouchStick_Read drags
// it along when the thumb passes the rim, and gives the move as a vector of
// length 0..1 (x right, y down): nothing inside the dead zone, full speed
// from the "run" deflection outwards.
// ---------------------------------------------------------------------------
typedef struct touchstick_s { float cx, cy; } touchstick_t;

static inline void TouchStick_Land(touchstick_t *s, float x, float y, float r, float w, float h)
{
	float pad = r * 0.55f;
	s->cx = x < pad ? pad : x > w - pad ? w - pad : x;
	s->cy = y < pad ? pad : y > h - pad ? h - pad : y;
}

static inline void TouchStick_Read(touchstick_t *s, float x, float y, float r, int follow, float deadzone, float run, float *mx, float *my, float *knob_x, float *knob_y)
{
	float ox = x - s->cx, oy = y - s->cy, mag = (float)sqrt(ox * ox + oy * oy), d, m;
	if (r < 1)
		r = 1;
	if (mag > r)
	{
		if (follow)
		{
			// the rim stays under the thumb
			s->cx += ox * (1 - r / mag);
			s->cy += oy * (1 - r / mag);
		}
		ox *= r / mag;
		oy *= r / mag;
		mag = r;
	}
	*knob_x = ox / r;
	*knob_y = oy / r;
	d = mag / r;
	if (run <= deadzone)
		run = deadzone + 0.01f;
	if (d <= deadzone || mag <= 0)
	{
		*mx = *my = 0;
		return;
	}
	m = (d - deadzone) / (run - deadzone);
	if (m > 1)
		m = 1;
	*mx = ox / mag * m;
	*my = oy / mag * m;
}

#endif
