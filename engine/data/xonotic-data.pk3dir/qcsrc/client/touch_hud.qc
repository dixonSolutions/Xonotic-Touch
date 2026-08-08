#include "touch_hud.qh"
#include "touch_init.qh"
#include "touch_widget.qh"
#include "touch_layout.qh"
#include <client/main.qh>
#include <client/hud/hud.qh>
#include <client/view.qh>
#include <client/items/items.qh>
#include <common/items/item.qh>
#include <common/weapons/all.qh>
#include <common/resources/resources.qh>
#include <common/wepent.qh>

// Vitals readouts for touch play. These are readouts, never controls: they draw
// no surface and claim no input, because a panel large enough to read is also
// large enough to put a dead patch in the aim area.
//
// The stock hud_panel_healtharmor / hud_panel_ammo panels are sized for a
// mouse-and-monitor player sitting close to a large screen. Their type is around
// 2-3 mm on this device. These replace them rather than rescaling them, so the
// numbers share the controls' stroke and colour language.

bool Touch_Hud_ShouldDraw(void)
{
	return cvar("vid_touchscreen") && autocvar__menu_alpha < 1 && autocvar_touch_mobile_hud;
}

// A skin's pics are not square — weapon icons are roughly 2:1 — so drawing them
// into a square box stretches them. Everything here is therefore sized by height
// and given whatever width the image's own aspect asks for.
string Touch_Hud_SkinPicPath(string pic)
{
	string path = strcat(hud_skin_path, "/", pic);
	if (draw_getimagesize(path) != '0 0 0')
		return path;
	path = strcat("gfx/hud/default/", pic);
	if (draw_getimagesize(path) != '0 0 0')
		return path;
	return "";
}

bool Touch_Hud_SkinPicExists(string pic)
{
	return Touch_Hud_SkinPicPath(pic) != "";
}

// The size a pic takes when fitted inside `box` without distortion. Separate
// from the draw call because a row has to know how wide the icon will be before
// it can place the text after it.
vector Touch_Hud_SkinPicFit(string pic, vector box)
{
	string path = Touch_Hud_SkinPicPath(pic);
	if (path == "")
		return '0 0 0';
	vector sz = draw_getimagesize(path);
	float aspect = (sz.x > 0 && sz.y > 0) ? sz.x / sz.y : 1;
	float h = min(box.y, box.x / aspect);
	return eX * (h * aspect) + eY * h;
}

// Returns the size drawn; '0 0 0' means the skin has no such pic.
vector Touch_Hud_DrawSkinPicFit(vector center, vector box, string pic, float alpha)
{
	vector fit = Touch_Hud_SkinPicFit(pic, box);
	if (fit == '0 0 0')
		return fit;
	drawpic_builtin(center - fit * 0.5, Touch_Hud_SkinPicPath(pic), fit, '1 1 1',
		alpha, DRAWFLAG_NORMAL);
	Touch_DebugAddFills(1);
	return fit;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
// Authored against the 540-unit short axis like every other token, then scaled
// by Touch_UIScale() and touch_hud_scale.

const float TOUCH_HUD_ROW_H    = 30; // one stat row, icon height included
const float TOUCH_HUD_ROW_GAP  = 6;
const float TOUCH_HUD_ICON     = 22;
const float TOUCH_HUD_NUM_SLOT = 54; // fits four digits at readout size
const float TOUCH_HUD_BAR_W    = 122;
const float TOUCH_HUD_BAR_H    = 9;

float Touch_Hud_Scale(void)
{
	return Touch_UIScale() * max(0.4, autocvar_touch_hud_scale);
}

float Touch_Hud_U(float authored)
{
	return authored * Touch_Hud_Scale();
}

vector Touch_Hud_StatusSize(void)
{
	float w = Touch_Hud_U(TOUCH_HUD_ICON + TOUCH_SPACE_1 + TOUCH_HUD_NUM_SLOT
		+ TOUCH_SPACE_2 + TOUCH_HUD_BAR_W);
	float h = Touch_Hud_U(TOUCH_HUD_ROW_H * 2 + TOUCH_HUD_ROW_GAP);
	return w * eX + h * eY;
}

// Centre, so it positions like every other widget and mirrors with handedness.
vector Touch_Hud_StatusCenter(void)
{
	return Touch_WidgetCenter(autocvar_touch_hud_x, autocvar_touch_hud_y, 0);
}

vector Touch_Hud_AmmoCenter(void)
{
	return Touch_WidgetCenter(autocvar_touch_ammo_x, autocvar_touch_ammo_y, 0);
}

// ---------------------------------------------------------------------------
// Stat rows
// ---------------------------------------------------------------------------

// A bar is read from its length, so the track stays visible at ghost alpha
// instead of vanishing when the value is low — an empty track and a missing bar
// must not look the same.
void Touch_Hud_DrawBar(vector left_mid, float width, float height, float frac,
	vector fill_color, float alpha)
{
	frac = bound(0, frac, 1);
	vector half = eX * (width * 0.5) + eY * (height * 0.5);
	vector center = left_mid + eX * (width * 0.5);

	if (!Touch_Shape_Ready() || half.y < 2)
	{
		drawfill_builtin(left_mid - eY * half.y, eX * width + eY * height,
			TOUCH_C_SHADOW, alpha * 0.45, DRAWFLAG_NORMAL);
		if (frac > 0.001)
			drawfill_builtin(left_mid - eY * half.y, eX * (width * frac) + eY * height,
				fill_color, alpha * 0.9, DRAWFLAG_NORMAL);
		Touch_DebugAddFills(2);
		return;
	}

	Touch_Shape_Capsule(center, half, TOUCH_C_SHADOW, alpha * TOUCH_A_SURFACE);
	Touch_Shape_CapsuleRing(center, half, TOUCH_W_RING_THIN, TOUCH_C_RIM,
		alpha * TOUCH_A_GHOST);

	// Below one cap-diameter a capsule degenerates into two overlapping caps and
	// reads as wider than its value, so the last sliver draws as a dot instead.
	float w = width * frac;
	if (w > height)
		Touch_Shape_Capsule(left_mid + eX * (w * 0.5), eX * (w * 0.5) + eY * half.y,
			fill_color, alpha * 0.92);
	else if (frac > 0.001)
		Touch_Shape_Disc(left_mid + eX * half.y, half.y, fill_color, alpha * 0.92);
}

// icon + right-aligned value + bar, on one baseline. Right-aligning the value
// means the bar never shifts as the number goes from 100 to 99.
void Touch_Hud_DrawStatRow(vector origin, string icon, string fallback, int value,
	int maxvalue, float alpha)
{
	float icon_sz = Touch_Hud_U(TOUCH_HUD_ICON);
	float num_slot = Touch_Hud_U(TOUCH_HUD_NUM_SLOT);
	float bar_w = Touch_Hud_U(TOUCH_HUD_BAR_W);
	float bar_h = Touch_Hud_U(TOUCH_HUD_BAR_H);
	float row_h = Touch_Hud_U(TOUCH_HUD_ROW_H);
	float mid_y = origin.y + row_h * 0.5;

	// blink=false. The stock helper's blink modulates the colour with
	// sin(2*pi*time) at exactly 100% and below 25%, so a full-health readout
	// cycles green-white-green every second — two captures of the same untouched
	// HUD came back different colours. The green-to-red ramp already carries the
	// value, the bar length carries it a second time, and a readout that is
	// glanced at from peripheral vision should not be the thing that moves.
	vector col = HUD_Get_Num_Color(value, maxvalue, false);

	// The icon sits centred in a fixed slot rather than flush left, so health and
	// armour keep one column even though their pics are not the same width.
	vector icon_at = eX * (origin.x + icon_sz * 0.5) + eY * mid_y;
	if (Touch_Hud_DrawSkinPicFit(icon_at, '1 1 0' * icon_sz, icon,
		alpha * TOUCH_A_INK) == '0 0 0')
		Touch_Shape_Label(icon_at, Touch_Hud_U(TOUCH_FONT_READOUT_SM), fallback, col,
			alpha * TOUCH_A_INK);

	float num_right = origin.x + icon_sz + Touch_Hud_U(TOUCH_SPACE_1) + num_slot;
	Touch_Shape_LabelAligned(eX * num_right + eY * mid_y,
		Touch_Hud_U(TOUCH_FONT_READOUT), ftos(value), 1, col, alpha * TOUCH_A_INK);

	float bar_x = num_right + Touch_Hud_U(TOUCH_SPACE_2);
	Touch_Hud_DrawBar(eX * bar_x + eY * mid_y, bar_w, bar_h,
		value / max(1, maxvalue), col, alpha);
}

void Touch_Hud_DrawStatus(float alpha)
{
	vector size = Touch_Hud_StatusSize();
	vector origin = Touch_Hud_StatusCenter() - size * 0.5;
	float row_pitch = Touch_Hud_U(TOUCH_HUD_ROW_H + TOUCH_HUD_ROW_GAP);

	int health = max(0, STAT(HEALTH));
	int armor = max(0, STAT(ARMOR));

	Touch_Hud_DrawStatRow(origin, "health", "HP", health,
		max(100, autocvar_touch_hud_maxhealth), alpha);
	// Armour is dimmed rather than hidden at zero: a row that disappears would
	// move the layout, and "no armour" is itself worth knowing. 0.7 rather than
	// something lower because at 0.45 the row was so faint that "no armour" and
	// "no armour readout" looked the same.
	Touch_Hud_DrawStatRow(origin + eY * row_pitch, "armor", "AR", armor,
		max(100, autocvar_touch_hud_maxarmor), armor > 0 ? alpha : alpha * 0.7);
}

// ---------------------------------------------------------------------------
// Ammo
// ---------------------------------------------------------------------------

void Touch_Hud_DrawAmmo(float alpha)
{
	entity wepent = viewmodels[0];
	Weapon wep = wepent.switchweapon;
	if (!wep)
		return;

	bool infinite = boolean(STAT(ITEMS) & IT_UNLIMITED_AMMO)
		|| wep.ammo_type == RES_NONE;
	int reserve = infinite ? 0 : getstati(GetAmmoStat(wep.ammo_type));
	int clip = wepent.clip_load;
	int clipmax = wepent.clip_size;
	bool clipped = (!infinite && clipmax > 0 && clip >= 0);

	string text;
	if (infinite)
		text = "\xE2\x88\x9E"; // U+221E
	else if (clipped)
		text = strcat(ftos(clip), " / ", ftos(reserve));
	else
		text = ftos(reserve);

	// Colour is the peripheral signal — the exact count only matters once it has
	// caught the eye. It tracks whichever number is on screen: a clipped weapon
	// is out when the clip is out, an unclipped one when the reserve is. The
	// unclipped threshold is the stock panel's own `ammo < 10` so that "low"
	// means the same thing here as everywhere else in the game.
	vector col = TOUCH_C_INK;
	if (!infinite)
	{
		int shown = clipped ? clip : reserve;
		bool low = clipped ? (shown <= clipmax * 0.25) : (shown < 10);
		if (shown <= 0)
			col = TOUCH_C_DANGER;
		else if (low)
			col = TOUCH_C_WARNING;
	}

	// Laid out exactly like a vitals row — icon in a fixed slot, then the value
	// right-aligned in a fixed slot — for two reasons. It is the same kind of
	// readout, so it should have the same shape; and the number is anchored
	// instead of centred, so going 100 → 99 → 9 does not walk the whole group
	// sideways. Centring it is what made a glanceable readout move under the eye.
	float fs = Touch_Hud_U(TOUCH_FONT_READOUT);
	float gap = Touch_Hud_U(TOUCH_SPACE_1);
	// 1.3x the vitals icon box: the ammo pics carry more internal padding, so at
	// the same box size they render visibly smaller than the health cross.
	float icon_h = Touch_Hud_U(TOUCH_HUD_ICON) * 1.3;
	// The ammo *type* icon, not the weapon icon: which gun is up is already told
	// by the weapons strip, whereas shells-vs-cells is what this number counts.
	// Same choice the stock panel makes.
	string icon = infinite ? "" : wep.ammo_type.m_icon;
	// A fixed square slot, centred, rather than one sized to the fitted pic. The
	// ammo pics carry different amounts of transparent padding — the shells pic is
	// mostly padding — so a slot measured from the fit put a 4-unit glyph 24 units
	// away from its number, and the gap changed with the weapon.
	float icon_slot = (icon != "") ? icon_h : 0;
	vector icon_box = '1 1 0' * icon_h;
	// A clipped weapon shows "12 / 140", so its slot has to hold both numbers.
	float num_slot = Touch_Shape_LabelWidth(fs, clipped ? "000 / 000" : "000");
	float total = num_slot + (icon_slot > 0 ? icon_slot + gap : 0);

	vector center = Touch_Hud_AmmoCenter();
	float left = center.x - total * 0.5;

	if (icon_slot > 0)
	{
		Touch_Hud_DrawSkinPicFit(eX * (left + icon_slot * 0.5) + eY * center.y,
			icon_box, icon, alpha * TOUCH_A_INK);
		left += icon_slot + gap;
	}
	Touch_Shape_LabelAligned(eX * (left + num_slot) + eY * center.y, fs, text, 1,
		col, alpha * TOUCH_A_INK);
}

void Touch_Hud_Draw(float alpha)
{
	if (!Touch_Hud_ShouldDraw())
		return;
	// Spectators have no vitals of their own, and a corpse's readout is noise on
	// top of the death overlay.
	if (spectatee_status == -1 || STAT(HEALTH) <= 0)
		return;

	Touch_Hud_DrawStatus(alpha);
	Touch_Hud_DrawAmmo(alpha);
}
