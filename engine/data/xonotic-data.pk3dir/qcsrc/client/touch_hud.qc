#include "touch_hud.qh"
#include "touch_init.qh"
#include "touch_widget.qh"
#include "touch_layout.qh"
#include "touch_weapons.qh"
#include "touch_input.qh"
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
	return autocvar__menu_alpha < 1 && autocvar_touch_mobile_hud;
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

// Three rows: health, armour, ammo. Ammo used to be its own free-floating anchor
// out by FIRE, which put it in the strip of screen the right thumb crosses on its
// way between FIRE and HOP — the number was both under the thumb and, having no
// backing, unreadable whenever a bright wall passed behind it. It is the same kind
// of number as the other two, so it reads as one group with them (Similarity) in
// one place the eye already visits.
vector Touch_Hud_StatusSize(void)
{
	float w = Touch_Hud_U(TOUCH_HUD_ICON + TOUCH_SPACE_1 + TOUCH_HUD_NUM_SLOT
		+ TOUCH_SPACE_2 + TOUCH_HUD_BAR_W);
	float h = Touch_Hud_U(TOUCH_HUD_ROW_H * 3 + TOUCH_HUD_ROW_GAP * 2);
	return w * eX + h * eY;
}

// Centre, so it positions like every other widget and mirrors with handedness.
vector Touch_Hud_StatusCenter(void)
{
	return Touch_WidgetCenter(autocvar_touch_hud_x, autocvar_touch_hud_y, 0);
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

	Touch_Shape_Capsule(center, half, TOUCH_C_SHADOW, alpha * TOUCH_A_SURFACE * Touch_GlassBody(false));
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

void Touch_Hud_DrawAmmoRow(vector origin, float alpha);

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

	// No plate behind the rows, unlike the weapons strip. One was tried at surface
	// alpha and measured: it darkened a dark background from 57 to 30 and a sunlit
	// wall from 255 to about 175, so it neither disappeared nor guaranteed
	// contrast — it only smudged a corner of the scene for nine draw calls. The
	// contrast is carried by the labels' own outline, which is why the numbers are
	// legible over bright geometry here. The strip keeps its plate because a plate
	// is what makes nine loose icons read as one inventory; three aligned rows with
	// bars already read as one readout.
	Touch_Hud_DrawStatRow(origin, "health", "HP", health,
		max(100, autocvar_touch_hud_maxhealth), alpha);
	// Armour is dimmed rather than hidden at zero: a row that disappears would
	// move the layout, and "no armour" is itself worth knowing. 0.7 rather than
	// something lower because at 0.45 the row was so faint that "no armour" and
	// "no armour readout" looked the same.
	Touch_Hud_DrawStatRow(origin + eY * row_pitch, "armor", "AR", armor,
		max(100, autocvar_touch_hud_maxarmor), armor > 0 ? alpha : alpha * 0.7);
	Touch_Hud_DrawAmmoRow(origin + eY * row_pitch * 2, alpha);
}

// ---------------------------------------------------------------------------
// Ammo
// ---------------------------------------------------------------------------

void Touch_Hud_DrawAmmoRow(vector origin, float alpha)
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
		text = ftos(clip);
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

	// Same three columns as a vitals row — icon slot, right-aligned number, then
	// the wide slot the bars occupy — so the three numbers form one column and
	// none of them moves as its value changes digits.
	float icon_sz = Touch_Hud_U(TOUCH_HUD_ICON);
	float num_slot = Touch_Hud_U(TOUCH_HUD_NUM_SLOT);
	float fs = Touch_Hud_U(TOUCH_FONT_READOUT);
	float mid_y = origin.y + Touch_Hud_U(TOUCH_HUD_ROW_H) * 0.5;

	// The ammo *type* icon, not the weapon icon: which gun is up is already told
	// by the weapons strip, whereas shells-vs-cells is what this number counts.
	// Same choice the stock panel makes.
	string icon = infinite ? "" : wep.ammo_type.m_icon;
	// Drawn 1.3x its slot: the ammo pics carry much more transparent padding than
	// the health cross, so at an equal box they render visibly smaller. The slot
	// itself keeps the vitals width, so the column holds either way.
	if (icon != "")
		Touch_Hud_DrawSkinPicFit(eX * (origin.x + icon_sz * 0.5) + eY * mid_y,
			'1 1 0' * icon_sz * 1.3, icon, alpha * TOUCH_A_INK);

	float num_right = origin.x + icon_sz + Touch_Hud_U(TOUCH_SPACE_1) + num_slot;
	Touch_Shape_LabelAligned(eX * num_right + eY * mid_y, fs, text, 1, col,
		alpha * TOUCH_A_INK);

	// Reserve rounds go where the bars go, at label weight, and only for weapons
	// that reload. What is in the gun is the number you act on; the reserve is
	// context, so it is not allowed to compete with it for the number column.
	if (clipped)
		Touch_Shape_LabelAligned(eX * (num_right + Touch_Hud_U(TOUCH_SPACE_2))
			+ eY * mid_y, Touch_Hud_U(TOUCH_FONT_READOUT_SM),
			strcat("/ ", ftos(reserve)), -1, TOUCH_C_INK, alpha * TOUCH_A_INK * 0.8);
}

// ---------------------------------------------------------------------------
// Weapons strip
// ---------------------------------------------------------------------------
// Which guns you hold and which one is up, on the right edge. The
// stock hud_panel_weapons was doing this job, but its selected slot is the skin's
// `weapon_current_bg` — an opaque chamfered plate — which made the brightest,
// hardest-edged thing on screen the one element nobody needs to look at twice.
// Its brightness lives in the asset, so no cvar could tone it down, and dimming
// the panel would have dimmed the icons with it.
//
// Every row is also a tap target (Touch_Weapons_AddSlot): the strip is the
// weapon switcher, here and -- through the engine's copy of this HUD -- on
// servers that run their own CSQC, so the two never diverge.

const float TOUCH_HUD_WEP_H    = 26;
const float TOUCH_HUD_WEP_W    = 46;
const float TOUCH_HUD_WEP_PAD  = 4;
// The resize grip under the last row: its own band inside the plate, so it is
// never mistaken for (or taken by) a row.
const float TOUCH_HUD_WEP_GRIP = 12;

// cl_touch_weplist_scale is an engine cvar (touch_hud.c draws the same strip on
// servers), archived in config.cfg. It is deliberately not a touch_* cvar: those
// are pinned by touch.layout.cfg at launch, so a size dragged on a server could
// never be kept. 0 means an engine that does not have it yet.
const float TOUCH_WEPLIST_SCALE_DEF = 1.3;
const float TOUCH_WEPLIST_SCALE_MIN = 0.7;
const float TOUCH_WEPLIST_SCALE_MAX = 2.4;
// Of the screen height: the most a strip may take, and the band it stays in,
// from below the top row of pills (MENU, CHAT and the engine's vote chip at
// the end of it) to near the bottom edge.
const float TOUCH_WEPLIST_FIT       = 0.83;
const float TOUCH_WEPLIST_MARGIN_TOP = 0.13;
const float TOUCH_WEPLIST_MARGIN    = 0.04;
// The grip's touch area, as a share of the short axis: a 12-unit grip is a
// sliver, so it reaches below the plate to a finger's width.
const float TOUCH_WEPLIST_GRIP_REACH = 0.07;

float Touch_Hud_WeplistScale(void)
{
	float s = cvar("cl_touch_weplist_scale");
	if (s == 0)
		s = TOUCH_WEPLIST_SCALE_DEF;
	return bound(TOUCH_WEPLIST_SCALE_MIN, s, TOUCH_WEPLIST_SCALE_MAX);
}

// The strip's own unit, sized by cl_touch_weplist_scale rather than
// touch_hud_scale: the rows are tap targets, not a readout, and at the readout
// size they were 4 mm tall on a tablet. Shrunk for a long inventory so the whole
// strip, grip included, fits the screen height.
float Touch_Hud_WeaponsU(int rows)
{
	float u = Touch_UIScale() * Touch_Hud_WeplistScale();
	float units = rows * TOUCH_HUD_WEP_H + TOUCH_HUD_WEP_GRIP + 2 * TOUCH_HUD_WEP_PAD;
	float cap = TOUCH_WEPLIST_FIT * Touch_UISize().y;
	if (units * u > cap)
		u = cap / units;
	return u;
}

// Centre of the plate, kept on screen: within the screen across, and within
// 13%..96% of its height down.
vector Touch_Hud_WeaponsCenter(int rows, float u)
{
	vector ui = Touch_UISize();
	vector c = Touch_WidgetCenter(autocvar_touch_weplist_x, autocvar_touch_weplist_y, 0);
	float half_w = (TOUCH_HUD_WEP_W * 0.5 + TOUCH_HUD_WEP_PAD) * u;
	float half_h = (rows * TOUCH_HUD_WEP_H + TOUCH_HUD_WEP_GRIP + 2 * TOUCH_HUD_WEP_PAD) * u * 0.5;
	c.x = bound(half_w, c.x, ui.x - half_w);
	c.y = bound(TOUCH_WEPLIST_MARGIN_TOP * ui.y + half_h, c.y, (1 - TOUCH_WEPLIST_MARGIN) * ui.y - half_h);
	c.z = 0;
	return c;
}

int Touch_Hud_OwnedWeaponCount(void)
{
	WepSet owned = STAT(WEAPONS);
	int n = 0;
	FOREACH(Weapons, it != WEP_Null && it.impulse >= 0 && (owned & it.m_wepset), n += 1);
	return n;
}

// Two columns: the impulse key, then the icon. The impulse is what a player with
// a keyboard attached presses and is how the stock panel labels slots, but it is
// reference rather than the point of the row, so it stays small and dim.
//
// It was drawn as a corner badge first. A row is 26 units and the digit is 10, so
// there is no corner to sit in — it landed just above centre, overlapping the
// icon's box and looking misplaced rather than deliberate. As a column it aligns
// down the whole strip and the icon gets the space that is left.
void Touch_Hud_DrawWeaponRow(vector center, vector half, float u, Weapon wep, bool current,
	float alpha)
{
	float pad = TOUCH_HUD_WEP_PAD * u;
	float num = TOUCH_FONT_READOUT_SM * 0.8 * u;
	float num_slot = Touch_Shape_LabelWidth(num, "9");
	float left = center.x - half.x + pad;

	Touch_Shape_LabelAligned(eX * (left + num_slot) + eY * center.y, num,
		ftos(wep.impulse), 1, TOUCH_C_INK, alpha * TOUCH_A_INK * 0.7);

	float icon_left = left + num_slot + pad;
	float icon_right = center.x + half.x - pad;
	Touch_Hud_DrawSkinPicFit(eX * ((icon_left + icon_right) * 0.5) + eY * center.y,
		eX * (icon_right - icon_left) + eY * (half.y * 2 - pad * 2), wep.model2,
		alpha * (current ? TOUCH_A_INK : TOUCH_A_INK * 0.55));
}

// Two short lines, the drag-handle idiom of a bottom sheet: quiet enough to read
// as part of the plate, there to say the strip can be pulled.
void Touch_Hud_DrawWeaponGrip(vector center, float u, bool held, float alpha)
{
	vector half = eX * (TOUCH_HUD_WEP_W * 0.2 * u) + eY * max(0.6, 0.7 * u);
	vector rgb = held ? TOUCH_C_ACCENT_LIGHT : TOUCH_C_INK;
	float a = alpha * (held ? TOUCH_A_RIM_ACTIVE : TOUCH_A_INK * 0.35);
	vector gap = eY * (2.2 * u);
	if (!Touch_Shape_Ready())
	{
		drawfill_builtin(center - gap - half, half * 2, rgb, a, DRAWFLAG_NORMAL);
		drawfill_builtin(center + gap - half, half * 2, rgb, a, DRAWFLAG_NORMAL);
		Touch_DebugAddFills(2);
		return;
	}
	Touch_Shape_Capsule(center - gap, half, rgb, a);
	Touch_Shape_Capsule(center + gap, half, rgb, a);
}

// The grip is being dragged by a finger (Touch_Hud_GripBegin .. _GripEnd).
bool touch_wep_grip_held;
float touch_wep_grip_u0;
float touch_wep_grip_y0;
int touch_wep_grip_rows;

void Touch_Hud_GripBegin(vector p)
{
	touch_wep_grip_held = true;
	touch_wep_grip_u0 = Touch_Weapons_GripU();
	touch_wep_grip_y0 = p.y;
	touch_wep_grip_rows = max(1, Touch_Weapons_GripRows());
}

// The grip follows the finger: the strip is centred on its anchor, so its
// bottom edge moves half as far as its height grows, hence the 2.
void Touch_Hud_GripMove(vector p)
{
	if (!touch_wep_grip_held || p.y == touch_wep_grip_y0)
		return;
	float u = touch_wep_grip_u0 + 2 * (p.y - touch_wep_grip_y0)
		/ (touch_wep_grip_rows * TOUCH_HUD_WEP_H + TOUCH_HUD_WEP_GRIP);
	float s = bound(TOUCH_WEPLIST_SCALE_MIN, u / max(0.001, Touch_UIScale()),
		TOUCH_WEPLIST_SCALE_MAX);
	s = rint(s * 100) / 100;
	if (s == cvar("cl_touch_weplist_scale"))
		return;
	if (cvar_type("cl_touch_weplist_scale") & CVAR_TYPEFLAG_EXISTS)
		cvar_set("cl_touch_weplist_scale", ftos(s));
	else
		localcmd("seta cl_touch_weplist_scale ", ftos(s), "\n");
}

void Touch_Hud_GripEnd(void)
{
	if (!touch_wep_grip_held)
		return;
	touch_wep_grip_held = false;
	// Android never quits cleanly, so a size that only reached config.cfg at
	// exit could be lost; one write per drag is cheap.
	localcmd("saveconfig\n");
}

// The NADE row: the grenade icon and the bonus grenades in stock; while one
// cooks, the fuse spent fills the row from the left, red near the end; while
// the next is on its way, a faint icon that takes no touch.
void Touch_Hud_DrawNadeRow(vector at, vector row_half, float u, float r, int state, float alpha)
{
	float pad = TOUCH_HUD_WEP_PAD * u;
	float num = TOUCH_FONT_READOUT_SM * 0.8 * u;
	float num_slot = Touch_Shape_LabelWidth(num, "9");
	float left = at.x - row_half.x + pad;
	float ink = (state == 3) ? TOUCH_A_INK * 0.25 : TOUCH_A_INK;
	if (state == 2)
	{
		float t = bound(0, cvar("_cl_touch_nade_timer"), 1);
		vector c = (t > 0.6) ? TOUCH_C_DANGER : TOUCH_C_ACCENT;
		drawfill_builtin(at - row_half, eX * (row_half.x * 2 * t) + eY * (row_half.y * 2), c,
			alpha * TOUCH_A_SURFACE_ACTIVE, DRAWFLAG_NORMAL);
		Touch_DebugAddFills(1);
	}
	else if (Touch_NadeHeld())
		Touch_Shape_RRect(at, row_half, r, TOUCH_C_ACCENT, alpha * TOUCH_A_SURFACE_LATCH);
	float bonus = STAT(NADE_BONUS);
	if (bonus >= 1)
		Touch_Shape_LabelAligned(eX * (left + num_slot) + eY * at.y, num, ftos(floor(bonus)), 1, TOUCH_C_INK, alpha * ink);
	float icon_left = left + num_slot + pad;
	float icon_right = at.x + row_half.x - pad;
	Touch_Hud_DrawSkinPicFit(eX * ((icon_left + icon_right) * 0.5) + eY * at.y,
		eX * (icon_right - icon_left) + eY * (row_half.y * 2 - pad * 2), "nade_normal", alpha * ink);
	Touch_Weapons_SetNade(at - row_half, row_half * 2, state == 1 || state == 2);
}

void Touch_Hud_DrawWeapons(float alpha)
{
	if (!autocvar_touch_weplist_visible)
		return;
	int n = Touch_Hud_OwnedWeaponCount();
	if (n <= 0)
		return;

	// The engine's nade state (touch_nade.c): 0 none, 1 ready, 2 cooking, 3
	// the next on its way. Its row's place is kept while the server has nades,
	// so the weapon rows never move as a grenade comes and goes.
	int nade_state = cvar("_cl_touch_nade");
	int rows = n + (nade_state != 0);
	float u = Touch_Hud_WeaponsU(rows);
	float h = TOUCH_HUD_WEP_H * u;
	float pad = TOUCH_HUD_WEP_PAD * u;
	float w = TOUCH_HUD_WEP_W * u;
	float grip = TOUCH_HUD_WEP_GRIP * u;
	float r = TOUCH_R_SM * u;
	vector row_half = eX * (w * 0.5) + eY * (h * 0.5);
	vector center = Touch_Hud_WeaponsCenter(rows, u);
	float plate_h = rows * h + grip + pad * 2;
	// Centred on its anchor, so the strip grows in both directions as weapons are
	// picked up instead of walking down the screen.
	float top = center.y - plate_h * 0.5 + pad;

	// One region behind the whole list rather than a plate per weapon (Law of
	// Common Region): this is a single inventory, not nine loose badges, and it is
	// also the difference between two rounded rects and eleven. Each rounded rect
	// is nine draws, so per-slot plates put the overlay at 175 draw calls.
	Touch_Shape_RRect(center, eX * (w * 0.5 + pad) + eY * (plate_h * 0.5), r + pad,
		TOUCH_C_SURFACE, alpha * TOUCH_A_SURFACE * Touch_GlassBody(false));

	entity wepent = viewmodels[0];
	WepSet owned = STAT(WEAPONS);
	// Not `i`: FOREACH declares its own, and shadowing it makes the counter const.
	int slot = 0;
	// Rebuilt every frame, like the stock panel does when it is the one drawing.
	Touch_Weapons_BeginFrame();
	// The held weapon is a selected row, the way a list view marks one: an accent
	// wash and nothing else. Marking it by *weight* — a bright opaque plate — is
	// what made the stock panel the loudest object on screen. One washed row
	// inside a quiet region is not the same thing as a plate behind every slot,
	// which is what the first attempt drew.
	FOREACH(Weapons, it != WEP_Null && it.impulse >= 0 && (owned & it.m_wepset),
	{
		vector at = eX * center.x + eY * (top + slot * h + h * 0.5);
		bool current = (it == wepent.switchweapon);
		if (current)
			Touch_Shape_RRect(at, row_half, r, TOUCH_C_ACCENT,
				alpha * TOUCH_A_SURFACE_LATCH);
		Touch_Hud_DrawWeaponRow(at, row_half, u, it, current, alpha);
		Touch_Weapons_AddSlot(at - row_half, row_half * 2, it.impulse, true);
		slot += 1;
	});

	if (nade_state != 0)
		Touch_Hud_DrawNadeRow(eX * center.x + eY * (top + n * h + h * 0.5), row_half, u, r, nade_state, alpha);
	float grip_top = top + rows * h;
	Touch_Hud_DrawWeaponGrip(eX * center.x + eY * (grip_top + grip * 0.5), u,
		touch_wep_grip_held, alpha);
	// The grip's touch area starts at its top edge, never above it over the last
	// row, and reaches down to a finger's width.
	float reach = max(grip + pad, TOUCH_WEPLIST_GRIP_REACH * Touch_ScreenMin());
	Touch_Weapons_SetGrip(eX * (center.x - w * 0.5 - pad) + eY * grip_top,
		eX * (w + pad * 2) + eY * reach, u, rows);
}

void Touch_Hud_Draw(float alpha)
{
	if (!Touch_Hud_ShouldDraw())
		return;
	// Spectators have no vitals of their own, and a corpse's readout is noise on
	// top of the death overlay.
	if (spectatee_status == -1 || STAT(HEALTH) <= 0)
		return;

	// The vitals are a touch-control readout, like the stock panels they replace; the
	// weapon strip stays up at the desk too (see TouchHUD_SyncStockPanels).
	if (cvar("vid_touchscreen"))
		Touch_Hud_DrawStatus(alpha);
	// Following someone, the weapons are theirs: the stock panel shows them, and
	// a row tap would send an impulse to the spectator code.
	if (!spectatee_status)
		Touch_Hud_DrawWeapons(alpha);
}
