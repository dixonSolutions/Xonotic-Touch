#include "touch_widget.qh"
#include "touch_glass.qh"

// Three layers per control — drop shadow, translucent surface, rim stroke.
// The shadow is what separates a control from arbitrary game content: a flat
// translucent disc disappears over bright terrain, a shadowed one does not.

// Fit the label to the control's box: bounded by half-height so it never
// crowds the rim vertically, and by width so a long label in a small disc
// shrinks instead of spilling over the edge.
float Touch_Widget_LabelSize(vector half, string label)
{
	float by_height = half.y * TOUCH_FONT_FIT_H;
	// Text width scales linearly with font size, so one measurement at unit size
	// converts the available width straight into a font size. Measured rather than
	// counted, because "MENU" and "1111" have the same length and not the same
	// width, and it was the widest label in a set that used to overflow.
	float unit = max(0.01, Touch_Shape_LabelWidth(1, label));
	float by_width = (half.x * 2 * TOUCH_FONT_FIT_W) / unit;
	return bound(TOUCH_FONT_MIN, min(by_height, by_width), TOUCH_FONT_MAX);
}

float Touch_Widget_FontSize(float radius)
{
	return Touch_Widget_LabelSize('1 1 0' * radius, "AAAA");
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
	float fs = Touch_Widget_LabelSize('1 1 0' * radius, label);

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
	float a_surface = TOUCH_A_SURFACE;
	if (pressed)
		a_surface = TOUCH_A_SURFACE_ACTIVE;
	else if (latched)
		a_surface = TOUCH_A_SURFACE_LATCH;
	float a_rim = tinted ? TOUCH_A_RIM_ACTIVE : TOUCH_A_RIM;
	// Sized from the whole box, not the space left by the pip: a label that
	// changed size when the control latched would read as a glitch. The pip sits
	// a full cap-radius clear of a centred label at these proportions.
	float fs = Touch_Widget_LabelSize(half, label);

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
		// Toggle-style "on" pip, centred in the right cap (whose centre is
		// exactly half.y in from the end) so the thumb resting on the bar's
		// middle never hides it. Ink, not accent: on an accent-filled surface an
		// accent pip is invisible.
		vector pip = center + eX * (half.x - half.y);
		float pip_r = max(2, half.y * 0.26);
		Touch_Shape_Disc(pip, pip_r, TOUCH_C_INK, alpha * TOUCH_A_INK);
		Touch_Shape_Ring(pip, pip_r * 1.5, TOUCH_W_RING_THIN, TOUCH_C_INK,
			alpha * TOUCH_A_RIM);
	}
}

void Touch_Widget_Key(vector center, vector half, string label, bool pressed,
	bool on, bool emphasis, float alpha, vector accent)
{
	if (half.x < 2 || half.y < 2 || alpha <= 0)
		return;

	bool tinted = pressed || on || emphasis;
	vector surface = tinted ? accent : TOUCH_C_SURFACE;
	float a_surface = TOUCH_A_SURFACE;
	if (pressed)
		a_surface = TOUCH_A_SURFACE_ACTIVE;
	else if (on || emphasis)
		a_surface = TOUCH_A_SURFACE_LATCH;
	float fs = Touch_Widget_LabelSize(half, label);
	float r = TOUCH_R_SM * Touch_UIScale();

	if (!Touch_Shape_Ready())
	{
		drawfill_builtin(center - half, half * 2, surface, alpha * a_surface,
			DRAWFLAG_NORMAL);
		Touch_DebugAddFills(1);
		Touch_Shape_Label(center, fs, label, TOUCH_C_INK, alpha * TOUCH_A_INK);
		return;
	}

	Touch_Shape_RRect(center, half, r, surface, alpha * a_surface);
	// A pressed key is under a fingertip, so the only feedback that survives is
	// outside the contact patch: the rim, not the fill.
	Touch_Shape_RRectRing(center, half, r, TOUCH_W_STROKE * Touch_UIScale(),
		(tinted ? accent : TOUCH_C_RIM),
		alpha * (pressed ? TOUCH_A_RIM_ACTIVE : TOUCH_A_RIM * 0.5));
	Touch_Shape_Label(center, fs, label, TOUCH_C_INK, alpha * TOUCH_A_INK);
}

void Touch_Widget_Field(vector center, vector half, string text, string placeholder,
	float alpha, vector accent)
{
	if (half.x < 2 || half.y < 2)
		return;

	float r = TOUCH_R_SM * Touch_UIScale();
	bool empty = (text == "");
	float fs = min(TOUCH_FONT_READOUT * Touch_UIScale(), half.y * TOUCH_FONT_FIT_H);
	float pad = TOUCH_SPACE_2 * Touch_UIScale();

	if (Touch_Shape_Ready())
	{
		Touch_Shape_RRect(center, half, r, TOUCH_C_SURFACE, alpha * TOUCH_A_SURFACE_ACTIVE);
		// An accent *outline* marks the field as the focused element. Filling it
		// with the accent instead made the composer a solid blue slab — the one
		// element in the overlay that was neither glass nor text.
		Touch_Shape_RRectRing(center, half, r, TOUCH_W_STROKE * Touch_UIScale(),
			accent, alpha * TOUCH_A_RIM);
	}
	else
	{
		drawfill_builtin(center - half, half * 2, TOUCH_C_SURFACE,
			alpha * TOUCH_A_SURFACE_ACTIVE, DRAWFLAG_NORMAL);
		Touch_DebugAddFills(1);
	}

	string shown = empty ? placeholder : text;
	float left = center.x - half.x + pad;
	// Long messages scroll rather than wrap: the caret must stay visible, and a
	// field that grew taller would shove the keyboard off screen.
	float room = half.x * 2 - pad * 2;
	while (shown != "" && Touch_Shape_LabelWidth(fs, shown) > room)
		shown = substring(shown, 1, strlen(shown) - 1);

	Touch_Shape_LabelAligned(eX * left + eY * center.y, fs, shown, -1,
		(empty ? TOUCH_C_RIM : TOUCH_C_INK), alpha * (empty ? TOUCH_A_GHOST * 3 : TOUCH_A_INK));

	// Blinking caret at a 1 Hz duty cycle: present enough to mark the insertion
	// point, quiet enough not to pull the eye off the keyboard.
	if (Touch_Shape_Ready() && (time - floor(time)) < 0.6)
	{
		float caret_x = left + Touch_Shape_LabelWidth(fs, shown) + max(1, fs * 0.08);
		Touch_Shape_RRect(eX * caret_x + eY * center.y,
			eX * max(1, fs * 0.06) + eY * (fs * 0.55), 1, accent, alpha * TOUCH_A_INK);
	}
}

// Flatter than the gameplay controls — no elevation shadow — because chrome
// should recede. It recedes by being flat and small, not by being faint: these
// are the controls a player has to *find*, and they sit wherever the map happens
// to be bright, so the plate under the label stays solid.
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
	float fs = Touch_Widget_LabelSize(half, label);

	if (!Touch_Shape_Ready())
	{
		Touch_Glass_DrawBar(center, half.x * 2, half.y * 2, surface, a, pressed);
		Touch_Shape_Label(center, fs, label, TOUCH_C_INK, a * TOUCH_A_INK);
		return;
	}

	Touch_Shape_Capsule(center, half, surface, a * TOUCH_A_CHROME_FILL);
	// The dual-stroke rim, same as every other control: the single light stroke it
	// used to draw vanished against a bright background, taking the pill's outline
	// with it, so the label floated on nothing.
	Touch_Widget_CapsuleRim(center, half, rim, a * TOUCH_A_RIM);
	Touch_Shape_Label(center, fs, label, TOUCH_C_INK, a * TOUCH_A_INK);
}
