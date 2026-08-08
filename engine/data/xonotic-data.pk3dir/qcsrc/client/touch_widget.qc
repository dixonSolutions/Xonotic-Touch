#include "touch_widget.qh"
#include "touch_glass.qh"

// Three layers per control — drop shadow, translucent surface, rim stroke.
// The shadow is what separates a control from arbitrary game content: a flat
// translucent disc disappears over bright terrain, a shadowed one does not.

float Touch_Widget_FontSize(float radius)
{
	return bound(TOUCH_FONT_MIN, radius * TOUCH_FONT_RATIO, TOUCH_FONT_MAX);
}

float Touch_Widget_Elevation(float radius)
{
	return max(1, radius * TOUCH_ELEVATION_Y);
}

// A dark stroke outside a bright one. Either alone fails on half the maps this
// game ships — a white rim disappears against a white skybox and a dark rim
// disappears in a black corridor — so every edge draws both.
void Touch_Widget_Rim(vector center, float radius, vector rgb, float alpha)
{
	Touch_Shape_Ring(center, radius, TOUCH_W_RING_THIN, TOUCH_C_SHADOW, alpha * TOUCH_A_CONTOUR);
	Touch_Shape_Ring(center, radius * (1 - TOUCH_W_CONTOUR_INSET), TOUCH_W_RING_THIN, rgb, alpha);
}

void Touch_Widget_CapsuleRim(vector center, vector half, vector rgb, float alpha)
{
	Touch_Shape_CapsuleRing(center, half, TOUCH_W_RING_THIN, TOUCH_C_SHADOW,
		alpha * TOUCH_A_CONTOUR);
	Touch_Shape_CapsuleRing(center, half * (1 - TOUCH_W_CONTOUR_INSET), TOUCH_W_RING_THIN,
		rgb, alpha);
}

// Pressed is the only state a thumb can actually see, since the thumb covers
// the control: brighten the rim and push a glow *past* the edge so feedback
// lands outside the contact patch.
void Touch_Widget_Button(vector center, float radius, string label, bool pressed,
	float alpha, vector accent)
{
	if (radius < 2 || alpha <= 0)
		return;

	vector surface = pressed ? accent : TOUCH_C_SURFACE;
	vector rim = pressed ? accent : TOUCH_C_RIM;
	float a_surface = pressed ? TOUCH_A_SURFACE_ACTIVE : TOUCH_A_SURFACE;
	float a_rim = pressed ? TOUCH_A_RIM_ACTIVE : TOUCH_A_RIM;
	float fs = Touch_Widget_FontSize(radius);

	if (!Touch_Shape_Ready())
	{
		Touch_Glass_DrawRoundedButton(center, radius * 2, surface, alpha, pressed);
		Touch_Shape_Label(center, fs, label, TOUCH_C_INK, alpha * TOUCH_A_INK);
		return;
	}

	Touch_Shape_Shadow(center + eY * Touch_Widget_Elevation(radius), radius * 1.12,
		alpha * TOUCH_A_SHADOW);
	Touch_Shape_Disc(center, radius, surface, alpha * a_surface);
	if (pressed)
		Touch_Shape_Glow(center, radius * 1.35, accent, alpha * TOUCH_A_GLOW);
	Touch_Widget_Rim(center, radius, rim, alpha * a_rim);
	Touch_Shape_Label(center, fs, label, TOUCH_C_INK, alpha * TOUCH_A_INK);
}

// `latched` is a sticky "still on after release" state (bunny-hop). It reads as
// tinted-but-not-pressed plus a pip, so it is distinguishable from both rest
// and an active press.
void Touch_Widget_Capsule(vector center, vector half, string label, bool pressed,
	bool latched, float alpha, vector accent)
{
	if (half.x < 2 || half.y < 2 || alpha <= 0)
		return;

	bool tinted = pressed || latched;
	vector surface = tinted ? accent : TOUCH_C_SURFACE;
	vector rim = tinted ? accent : TOUCH_C_RIM;
	float a_surface = pressed ? TOUCH_A_SURFACE_ACTIVE : TOUCH_A_SURFACE;
	float a_rim = tinted ? TOUCH_A_RIM_ACTIVE : TOUCH_A_RIM;
	float fs = Touch_Widget_FontSize(half.y);

	if (!Touch_Shape_Ready())
	{
		Touch_Glass_DrawBar(center, half.x * 2, half.y * 2, surface, alpha, pressed);
		Touch_Shape_Label(center, fs, label, TOUCH_C_INK, alpha * TOUCH_A_INK);
		return;
	}

	Touch_Shape_CapsuleShadow(center + eY * Touch_Widget_Elevation(half.y),
		half + '1 1 0' * (half.y * 0.10), alpha * TOUCH_A_SHADOW);
	Touch_Shape_Capsule(center, half, surface, alpha * a_surface);
	if (pressed)
		Touch_Shape_Capsule(center, half + '1 1 0' * (half.y * 0.18), accent, alpha * TOUCH_A_GLOW);
	Touch_Widget_CapsuleRim(center, half, rim, alpha * a_rim);
	Touch_Shape_Label(center, fs, label, TOUCH_C_INK, alpha * TOUCH_A_INK);

	if (latched)
	{
		// Toggle-style "on" pip, parked in the cap so the thumb never hides it.
		Touch_Shape_Disc(center + eX * (half.x - half.y * 0.55), max(2, half.y * 0.16),
			accent, alpha * TOUCH_A_RIM_ACTIVE);
	}
}

// Deliberately flatter than the gameplay controls: no elevation shadow and a
// single rim rather than the paired stroke. That is both the cheaper path (a
// capsule costs 3-4 draws per layer, and these are the least important controls
// on screen) and the correct one — chrome should recede.
void Touch_Widget_ChromePill(vector center, vector half, string label, bool pressed,
	float alpha, vector accent)
{
	if (half.x < 2 || half.y < 2)
		return;

	// Chrome keeps a floor on opacity: at touch_opacity 0.3 the action controls
	// may fade into the scene, but a control you have to *find* must stay legible.
	float a = pressed ? max(0.85, alpha) : max(TOUCH_A_CHROME_REST, alpha * 0.8);
	vector surface = pressed ? accent : TOUCH_C_SURFACE;
	vector rim = pressed ? accent : TOUCH_C_RIM;
	float fs = Touch_Widget_FontSize(half.y);

	if (!Touch_Shape_Ready())
	{
		Touch_Glass_DrawBar(center, half.x * 2, half.y * 2, surface, a, pressed);
		Touch_Shape_Label(center, fs, label, TOUCH_C_INK, a * TOUCH_A_INK);
		return;
	}

	Touch_Shape_Capsule(center, half, surface, a * TOUCH_A_SURFACE_ACTIVE);
	Touch_Shape_CapsuleRing(center, half, TOUCH_W_RING_THIN, rim, a * TOUCH_A_RIM);
	Touch_Shape_Label(center, fs, label, TOUCH_C_INK, a * TOUCH_A_INK);
}
