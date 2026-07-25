#include "touch_glass.qh"

// Solid panels via drawpic(white). On this Flatpak build, CSQC drawfill often
// fails to land in the framebuffer/screenshot while drawpic + drawstring work.

void Touch_Glass_Fill(vector pos, vector sz, vector rgb, float alpha)
{
	if (sz.x < 1 || sz.y < 1 || alpha <= 0)
		return;
	pos.z = 0;
	sz.z = 0;
	drawpic_builtin(pos, "gfx/colors/white", sz, rgb, alpha, DRAWFLAG_NORMAL);
}

void Touch_Glass_Frame(vector pos, vector sz, vector rgb, float alpha, float t)
{
	// 4-edge rim (readable when fills are subtle)
	if (t < 1)
		t = 1;
	Touch_Glass_Fill(pos, eX * sz.x + eY * t, rgb, alpha);
	Touch_Glass_Fill(pos + eY * (sz.y - t), eX * sz.x + eY * t, rgb, alpha);
	Touch_Glass_Fill(pos, eX * t + eY * sz.y, rgb, alpha);
	Touch_Glass_Fill(pos + eX * (sz.x - t), eX * t + eY * sz.y, rgb, alpha);
}

void Touch_Glass_DrawWidget(vector center, float w, float h, vector tint, float alpha, bool pressed)
{
	if (w < 2 || h < 2)
		return;
	vector pos = center - eX * (w * 0.5) - eY * (h * 0.5);
	vector sz = eX * w + eY * h;
	if (pressed)
		pos = pos + '0 2 0';

	Touch_Glass_Fill(pos + '2 3 0', sz, TOUCH_GLASS_SHADOW, alpha * 0.55);
	Touch_Glass_Fill(pos, sz, tint, alpha * 0.72);
	Touch_Glass_Fill(pos + eX * (w * 0.08) + eY * (h * 0.08),
		eX * (w * 0.84) + eY * (h * 0.28), TOUCH_GLASS_HIGHLIGHT, alpha * 0.28);
	Touch_Glass_Frame(pos, sz, TOUCH_GLASS_HIGHLIGHT, alpha * 0.65, 2);
	if (pressed)
		Touch_Glass_Fill(center - eX * (w * 0.22) - eY * (h * 0.22),
			eX * (w * 0.44) + eY * (h * 0.44), TOUCH_GLASS_ACCENT, alpha * 0.35);
}

void Touch_Glass_DrawBar(vector center, float w, float h, vector tint, float alpha, bool pressed)
{
	if (w < 2 || h < 2)
		return;
	vector pos = center - eX * (w * 0.5) - eY * (h * 0.5);
	vector sz = eX * w + eY * h;
	if (pressed)
		pos = pos + '0 2 0';
	Touch_Glass_Fill(pos + '2 3 0', sz, TOUCH_GLASS_SHADOW, alpha * 0.55);
	Touch_Glass_Fill(pos, sz, tint, alpha * 0.78);
	Touch_Glass_Fill(pos + eX * (w * 0.06) + eY * (h * 0.12),
		eX * (w * 0.88) + eY * (h * 0.36), TOUCH_GLASS_HIGHLIGHT, alpha * 0.30);
	Touch_Glass_Frame(pos, sz, TOUCH_GLASS_HIGHLIGHT, alpha * 0.75, 2);
	if (pressed)
		Touch_Glass_Fill(pos + '3 3 0', sz - '6 6 0', TOUCH_GLASS_ACCENT, alpha * 0.30);
}

void Touch_Glass_DrawCircle(vector center, float radius, vector tint, float alpha)
{
	float d = radius * 2;
	Touch_Glass_DrawWidget(center, d, d, tint, alpha, false);
}

void Touch_Glass_DrawCircleOutline(vector center, float radius, vector tint, float alpha, float thickness)
{
	float d = radius * 2;
	vector pos = center - eX * radius - eY * radius;
	Touch_Glass_Frame(pos, eX * d + eY * d, tint, alpha, max(2, thickness));
}

void Touch_Glass_DrawDisc(vector center, float radius, vector tint, float alpha)
{
	Touch_Glass_DrawCircle(center, radius, tint, alpha);
}

void Touch_Glass_DrawRoundedButton(vector center, float size, vector tint, float alpha, bool pressed)
{
	Touch_Glass_DrawWidget(center, size, size, tint, alpha, pressed);
}

void Touch_Glass_DrawJoystickBase(vector center, float radius, float alpha)
{
	// One plate + rim only (avoid nested widgets that read as stacked boxes).
	float d = radius * 2;
	vector pos = center - eX * radius - eY * radius;
	Touch_Glass_Fill(pos + '2 3 0', eX * d + eY * d, TOUCH_GLASS_SHADOW, alpha * 0.45);
	Touch_Glass_Fill(pos, eX * d + eY * d, TOUCH_GLASS_TINT, alpha * 0.55);
	Touch_Glass_Frame(pos, eX * d + eY * d, TOUCH_GLASS_HIGHLIGHT, alpha * 0.80, 3);
	float pad = radius * 0.22;
	Touch_Glass_Frame(pos + eX * pad + eY * pad, eX * (d - pad * 2) + eY * (d - pad * 2), TOUCH_GLASS_HIGHLIGHT, alpha * 0.35, 2);
}

void Touch_Glass_DrawJoystickKnob(vector center, float radius, vector offset, float alpha, bool active)
{
	vector knob = center + offset;
	float kr = radius * 0.36;
	vector tint = TOUCH_GLASS_HIGHLIGHT;
	if (active)
		tint = TOUCH_GLASS_ACCENT;
	vector pos = knob - eX * kr - eY * kr;
	float d = kr * 2;
	Touch_Glass_Fill(pos + '2 2 0', eX * d + eY * d, TOUCH_GLASS_SHADOW, alpha * 0.50);
	Touch_Glass_Fill(pos, eX * d + eY * d, tint, alpha * 0.90);
	Touch_Glass_Frame(pos, eX * d + eY * d, TOUCH_GLASS_HIGHLIGHT, alpha * 0.95, 2);
}
