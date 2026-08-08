#include "touch_look.qh"
#include "touch_init.qh"
#include "touch_defs.qh"

vector touch_look_vel;
vector touch_look_carry;
vector touch_look_down_pos;
vector touch_look_prev_raw;
vector touch_look_dvel;
bool touch_look_escaped;
bool touch_look_active;
float touch_look_prev_time;
bool touch_look_owns_sensitivity;

void Touch_Look_Reset(void)
{
	touch_look_vel = '0 0 0';
	touch_look_carry = '0 0 0';
	touch_look_prev_raw = '0 0 0';
	touch_look_dvel = '0 0 0';
	touch_look_escaped = false;
	touch_look_active = false;
	touch_look_down_pos = '0 0 0';
	touch_look_prev_time = time;
}

void Touch_Look_BeginGesture(vector down_pos)
{
	touch_look_down_pos = down_pos;
	touch_look_escaped = false;
	touch_look_active = true;
	touch_look_vel = '0 0 0';
	touch_look_carry = '0 0 0';
	touch_look_prev_raw = '0 0 0';
	touch_look_dvel = '0 0 0';
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
	touch_look_active = false;
	touch_look_escaped = false;
	touch_look_vel = '0 0 0';
}

void Touch_Look_SetOwnership(bool own)
{
	// Prefer setsensitivityscale over cvar_set("sensitivity") so config.cfg never
	// archives sensitivity 0 (B3). GetCurrentFov must honor Touch_Look_BlocksMouse().
	if (own && autocvar_touch_look_owns_view)
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

// One-euro filter (Casiez et al., CHI 2012), the same approach Valve shipped for
// Steam Deck gyro. A fixed low-pass trades jitter for latency at *every* speed;
// this raises its cutoff with speed instead, because people notice jitter when
// moving slowly and latency when moving fast. Touch-look is a drag, the most
// latency-sensitive input class there is, so constant lag is not affordable.
const float TOUCH_TWO_PI = 6.28318530717958647693;

float Touch_Look_Alpha(float cutoff, float dt)
{
	// Standard exponential-smoothing coefficient for a given cutoff frequency.
	float tau = 1.0 / (TOUCH_TWO_PI * max(cutoff, 0.0001));
	return 1.0 / (1.0 + tau / dt);
}

vector Touch_Look_Filter(vector v_raw, float dt)
{
	int mode = autocvar_touch_look_filter;

	if (mode == TOUCH_LOOK_FILTER_OFF)
		return v_raw;

	if (mode == TOUCH_LOOK_FILTER_EMA)
	{
		// Legacy fixed exponential smoother, kept so a player who preferred the
		// old feel can get it back.
		float tau = 0;
		if (autocvar_touch_look_smoothing == 1)
			tau = 0.035;
		else if (autocvar_touch_look_smoothing >= 2)
			tau = 0.070;
		float a = 1.0;
		if (tau > 0)
			a = 1.0 - exp(-dt / tau);
		return touch_look_vel + (v_raw - touch_look_vel) * a;
	}

	// Derivative of the signal, itself low-passed at a fixed cutoff, gives the
	// speed estimate that drives the adaptive cutoff.
	vector dv = (v_raw - touch_look_prev_raw) * (1.0 / dt);
	float da = Touch_Look_Alpha(1.0, dt);
	touch_look_dvel = touch_look_dvel + (dv - touch_look_dvel) * da;
	touch_look_prev_raw = v_raw;

	// fcmin governs slow-speed jitter, beta governs high-speed lag.
	float cutoff = max(autocvar_touch_look_fcmin, 0.01)
		+ max(autocvar_touch_look_beta, 0) * vlen(touch_look_dvel);
	float a = Touch_Look_Alpha(cutoff, dt);
	return touch_look_vel + (v_raw - touch_look_vel) * a;
}

void Touch_Look_Update(vector raw_px, float dt)
{
	if (!touch_look_active)
	{
		touch_look_vel = '0 0 0';
		return;
	}

	dt = bound(1.0 / 240.0, dt, 0.1);
	raw_px.z = 0;

	if (!touch_look_escaped)
	{
		// Escape is handled in Touch_Look_Frame with down_pos; raw path skips if tiny.
		if (vlen(raw_px) < autocvar_touch_look_deadzone_px * 0.25)
			return;
	}

	vector v_raw = raw_px * (1.0 / dt);
	touch_look_vel = Touch_Look_Filter(v_raw, dt);
	vector applied = touch_look_vel * dt;

	vector vf = getpropertyvec(VF_SIZE);
	float k = TOUCH_LOOK_DEG_PER_PX * autocvar_touch_sens_base * (TOUCH_LOOK_REF_WIDTH / max(vf.x, 1));
	float yaw = applied.x * k;
	float pitch = applied.y * k * autocvar_touch_sens_y_mult;
	if (autocvar_touch_invert_y)
		pitch = -pitch;

	float cap = autocvar_touch_look_max_deg_per_s * dt;
	yaw = bound(-cap, yaw, cap);
	pitch = bound(-cap, pitch, cap);

	touch_look_carry.x += yaw;
	touch_look_carry.y += pitch;

	vector ang = getpropertyvec(VF_CL_VIEWANGLES);
	ang.y -= touch_look_carry.x;
	ang.x += touch_look_carry.y;
	ang.x = bound(-89, ang.x, 89);
	ang.z = 0;
	setproperty(VF_CL_VIEWANGLES, ang);
	touch_look_carry = '0 0 0';
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
		accum_px = from_down * autocvar_touch_look_escape_carry;
	}

	Touch_Look_Update(accum_px, dt);
}
