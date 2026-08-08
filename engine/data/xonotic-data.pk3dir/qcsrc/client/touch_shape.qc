#include "touch_shape.qh"
#include "touch_theme.qh"
#include "touch_input.qh"

// Every control used to be built by stretching gfx/colors/white, which is why
// circles rendered as squares. These helpers draw the pre-baked antialiased
// alpha masks from scripts/gen-touch-shapes.py instead: one drawpic per layer,
// tinted at draw time, so a single mask serves every colour.
//
// drawfill is avoided throughout — on the Flatpak build CSQC fills often fail to
// reach the framebuffer while drawpic and drawstring work.

const string TOUCH_PIC_DISC       = "gfx/touch/disc";
const string TOUCH_PIC_DISC_SOFT  = "gfx/touch/disc_soft";
const string TOUCH_PIC_RING_THIN  = "gfx/touch/ring_thin";
const string TOUCH_PIC_RING       = "gfx/touch/ring";
const string TOUCH_PIC_RING_THICK = "gfx/touch/ring_thick";
const string TOUCH_PIC_SHADOW     = "gfx/touch/shadow";
const string TOUCH_PIC_RRECT      = "gfx/touch/rrect";
const string TOUCH_PIC_WHITE      = "gfx/colors/white";

// The rounded-rect mask is authored with this corner radius as a fraction of the
// texture, which is also the 9-slice source inset.
const float TOUCH_RRECT_UV = 0.25;

bool touch_shape_ready;
bool touch_shape_probed;

void Touch_Shape_Probe(void)
{
	if (touch_shape_probed)
		return;
	touch_shape_probed = true;
	// A partial package or a stale overlay can ship the progs without the masks.
	// Probing one representative mask is enough: they are generated together.
	touch_shape_ready = (drawgetimagesize(TOUCH_PIC_DISC) != '0 0 0');
	if (!touch_shape_ready)
		print("Touch: gfx/touch shape masks missing — falling back to flat glass\n");
}

bool Touch_Shape_Ready(void)
{
	Touch_Shape_Probe();
	return touch_shape_ready;
}

// ---------------------------------------------------------------------------
// Round shapes
// ---------------------------------------------------------------------------

void Touch_Shape_Pic(string pic, vector pos, vector sz, vector rgb, float alpha)
{
	if (sz.x < 1 || sz.y < 1 || alpha <= 0)
		return;
	pos.z = 0;
	sz.z = 0;
	drawpic_builtin(pos, pic, sz, rgb, alpha, DRAWFLAG_NORMAL);
	Touch_DebugAddFills(1);
}

// drawsubpic is deliberately the raw builtin: client/draw.qh leaves it unwrapped,
// and the overlay draws in console space with HUD scaling already disabled.
void Touch_Shape_Pic_Sub(string pic, vector pos, vector sz, vector src, vector srcsz,
	vector rgb, float alpha)
{
	if (sz.x < 1 || sz.y < 1 || alpha <= 0)
		return;
	pos.z = 0;
	sz.z = 0;
	drawsubpic(pos, sz, pic, src, srcsz, rgb, alpha, DRAWFLAG_NORMAL);
	Touch_DebugAddFills(1);
}

void Touch_Shape_Centered(string pic, vector center, vector half, vector rgb, float alpha)
{
	Touch_Shape_Pic(pic, center - half, half * 2, rgb, alpha);
}

void Touch_Shape_Disc(vector center, float radius, vector rgb, float alpha)
{
	Touch_Shape_Centered(TOUCH_PIC_DISC, center, '1 1 0' * radius, rgb, alpha);
}

void Touch_Shape_Glow(vector center, float radius, vector rgb, float alpha)
{
	Touch_Shape_Centered(TOUCH_PIC_DISC_SOFT, center, '1 1 0' * radius, rgb, alpha);
}

void Touch_Shape_Shadow(vector center, float radius, float alpha)
{
	Touch_Shape_Centered(TOUCH_PIC_SHADOW, center, '1 1 0' * radius, TOUCH_C_SHADOW, alpha);
}

// `weight` is a stroke fraction of the radius; pick the mask whose baked stroke
// is closest so the ring keeps a constant visual weight at any size.
float Touch_Shape_RingWeight(float weight)
{
	if (weight <= (TOUCH_W_RING_THIN + TOUCH_W_RING) * 0.5)
		return TOUCH_W_RING_THIN;
	if (weight <= (TOUCH_W_RING + TOUCH_W_RING_THICK) * 0.5)
		return TOUCH_W_RING;
	return TOUCH_W_RING_THICK;
}

string Touch_Shape_RingPic(float weight)
{
	float w = Touch_Shape_RingWeight(weight);
	if (w == TOUCH_W_RING_THIN)
		return TOUCH_PIC_RING_THIN;
	if (w == TOUCH_W_RING)
		return TOUCH_PIC_RING;
	return TOUCH_PIC_RING_THICK;
}

void Touch_Shape_Ring(vector center, float radius, float weight, vector rgb, float alpha)
{
	Touch_Shape_Centered(Touch_Shape_RingPic(weight), center, '1 1 0' * radius, rgb, alpha);
}

// ---------------------------------------------------------------------------
// Capsules (pills, the jump bar)
// ---------------------------------------------------------------------------
// Built from the left and right halves of a round mask plus a straight middle
// section, so the end caps stay circular however wide the capsule gets. A
// stretched rounded-rect would flatten them.

// `stroke` is the baked stroke fraction of the cap mask, and is only read when
// `hollow` is set: the straight edges have to match the caps' thickness exactly
// or the joint shows a step.
void Touch_Shape_CapsuleFrom(string pic, vector center, vector half, vector rgb, float alpha,
	bool hollow, float stroke)
{
	if (half.x < 1 || half.y < 1 || alpha <= 0)
		return;
	float cap = half.y;
	if (half.x <= cap)
	{
		// Degenerate: no straight section left, so it is just a circle.
		Touch_Shape_Centered(pic, center, '1 1 0' * half.y, rgb, alpha);
		return;
	}

	vector capsz = eX * cap + eY * (cap * 2);
	vector top = center - eY * cap;

	Touch_Shape_Pic_Sub(pic, center - half, capsz, '0 0 0', '0.5 1 0', rgb, alpha);
	Touch_Shape_Pic_Sub(pic, top + eX * (half.x - cap), capsz, '0.5 0 0', '0.5 1 0', rgb, alpha);

	float midw = (half.x - cap) * 2;
	if (hollow)
	{
		// Only the two long edges connect the caps.
		float t = max(1, stroke * cap);
		Touch_Shape_Pic(TOUCH_PIC_WHITE, top + eX * (cap - half.x), eX * midw + eY * t, rgb, alpha);
		Touch_Shape_Pic(TOUCH_PIC_WHITE, center + eY * (cap - t) - eX * (half.x - cap),
			eX * midw + eY * t, rgb, alpha);
	}
	else
	{
		Touch_Shape_Pic(TOUCH_PIC_WHITE, top + eX * (cap - half.x), eX * midw + eY * (cap * 2), rgb, alpha);
	}
}

void Touch_Shape_Capsule(vector center, vector half, vector rgb, float alpha)
{
	Touch_Shape_CapsuleFrom(TOUCH_PIC_DISC, center, half, rgb, alpha, false, 0);
}

void Touch_Shape_CapsuleRing(vector center, vector half, float weight, vector rgb, float alpha)
{
	Touch_Shape_CapsuleFrom(Touch_Shape_RingPic(weight), center, half, rgb, alpha, true,
		Touch_Shape_RingWeight(weight));
}

void Touch_Shape_CapsuleShadow(vector center, vector half, float alpha)
{
	Touch_Shape_CapsuleFrom(TOUCH_PIC_SHADOW, center, half, TOUCH_C_SHADOW, alpha, false, 0);
}

// ---------------------------------------------------------------------------
// Rounded rectangles
// ---------------------------------------------------------------------------

void Touch_Shape_RRect(vector center, vector half, float radius, vector rgb, float alpha)
{
	if (half.x < 1 || half.y < 1 || alpha <= 0)
		return;
	// A corner cannot exceed half the box, or opposite corners would overlap.
	float r = bound(1, radius, min(half.x, half.y));
	vector pos = center - half;
	vector sz = half * 2;
	float midw = sz.x - r * 2;
	float midh = sz.y - r * 2;
	vector cr = eX * r + eY * r;
	float uv = TOUCH_RRECT_UV;
	float uvmid = 1 - uv * 2;

	// Corners
	Touch_Shape_Pic_Sub(TOUCH_PIC_RRECT, pos, cr, '0 0 0', eX * uv + eY * uv, rgb, alpha);
	Touch_Shape_Pic_Sub(TOUCH_PIC_RRECT, pos + eX * (sz.x - r), cr,
		eX * (1 - uv), eX * uv + eY * uv, rgb, alpha);
	Touch_Shape_Pic_Sub(TOUCH_PIC_RRECT, pos + eY * (sz.y - r), cr,
		eY * (1 - uv), eX * uv + eY * uv, rgb, alpha);
	Touch_Shape_Pic_Sub(TOUCH_PIC_RRECT, pos + eX * (sz.x - r) + eY * (sz.y - r), cr,
		eX * (1 - uv) + eY * (1 - uv), eX * uv + eY * uv, rgb, alpha);

	// Edges and centre, stretched from the mask's straight sections.
	if (midw > 0)
	{
		Touch_Shape_Pic_Sub(TOUCH_PIC_RRECT, pos + eX * r, eX * midw + eY * r,
			eX * uv, eX * uvmid + eY * uv, rgb, alpha);
		Touch_Shape_Pic_Sub(TOUCH_PIC_RRECT, pos + eX * r + eY * (sz.y - r), eX * midw + eY * r,
			eX * uv + eY * (1 - uv), eX * uvmid + eY * uv, rgb, alpha);
	}
	if (midh > 0)
	{
		Touch_Shape_Pic(TOUCH_PIC_WHITE, pos + eY * r, eX * r + eY * midh, rgb, alpha);
		Touch_Shape_Pic(TOUCH_PIC_WHITE, pos + eX * (sz.x - r) + eY * r, eX * r + eY * midh, rgb, alpha);
	}
	if (midw > 0 && midh > 0)
		Touch_Shape_Pic(TOUCH_PIC_WHITE, pos + eX * r + eY * r, eX * midw + eY * midh, rgb, alpha);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

float Touch_Shape_LabelWidth(float fontsize, string label)
{
	return strlen(label) * fontsize * TOUCH_FONT_ADVANCE;
}

void Touch_Shape_Label(vector center, float fontsize, string label, vector rgb, float alpha)
{
	if (label == "" || alpha <= 0)
		return;
	vector sz = '1 1 0' * fontsize;
	vector pos = center - eX * (Touch_Shape_LabelWidth(fontsize, label) * 0.5) - eY * (fontsize * 0.5);
	pos.z = 0;
	// A dark 1 px stroke is the single largest legibility win over arbitrary game
	// content: without it white text vanishes into bright terrain.
	float s = max(1, floor(fontsize * 0.09));
	float sa = alpha * TOUCH_A_INK_SHADOW;
	drawstring_builtin(pos - eX * s, label, sz, TOUCH_C_INK_SHADOW, sa, 0);
	drawstring_builtin(pos + eX * s, label, sz, TOUCH_C_INK_SHADOW, sa, 0);
	drawstring_builtin(pos - eY * s, label, sz, TOUCH_C_INK_SHADOW, sa, 0);
	drawstring_builtin(pos + eY * s, label, sz, TOUCH_C_INK_SHADOW, sa, 0);
	drawstring_builtin(pos, label, sz, rgb, alpha, 0);
	Touch_DebugAddFills(5);
}
