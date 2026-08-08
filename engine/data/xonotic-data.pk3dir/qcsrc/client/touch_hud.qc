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
#include <common/wepent.qh>

string touch_hud_portrait_path;
string touch_hud_portrait_model;
string touch_hud_portrait_skin;

bool Touch_Hud_ShouldDraw(void)
{
	return cvar("vid_touchscreen") && autocvar__menu_alpha < 1 && autocvar_touch_mobile_hud;
}

void Touch_Hud_DrawSkinPic(vector pos, vector size, string pic, float alpha)
{
	string path = strcat(hud_skin_path, "/", pic);
	if (draw_getimagesize(path) == '0 0 0')
		path = strcat("gfx/hud/default/", pic);
	drawpic_builtin(pos, path, size, '1 1 1', alpha, DRAWFLAG_NORMAL);
}

string Touch_Hud_ResolvePortrait(void)
{
	string mdl = cvar_string("_cl_playermodel");
	string skin = cvar_string("_cl_playerskin");

	if (mdl == touch_hud_portrait_model && skin == touch_hud_portrait_skin && touch_hud_portrait_path != "")
		return touch_hud_portrait_path;

	touch_hud_portrait_model = mdl;
	touch_hud_portrait_skin = skin;
	touch_hud_portrait_path = "";

	if (mdl == "")
		mdl = "gak";

	float glob = search_begin(get_model_datafilename(string_null, -1, "txt"), true, true);
	if (glob < 0)
		return touch_hud_portrait_path;

	int i;
	for (i = 0; i < search_getsize(glob); ++i)
	{
		string fn = search_getfilename(glob, i);
		if (!get_model_parameters(fn, -1))
			continue;
		if (get_model_parameters_modelname == mdl && ftos(get_model_parameters_modelskin) == skin)
		{
			touch_hud_portrait_path = strcat("/", substring(get_model_datafilename(get_model_parameters_modelname, get_model_parameters_modelskin, "tga"), 0, -5));
			break;
		}
	}
	search_end(glob);
	return touch_hud_portrait_path;
}

vector Touch_Hud_StatusPanelSize(void)
{
	float u = Touch_ScreenMin() * 0.28 * Touch_WidgetScale();
	return u * eX + u * 0.38 * eY;
}

vector Touch_Hud_StatusPanelOrigin(void)
{
	float pad = Touch_ScreenMin() * autocvar_touch_hud_x;
	return pad * eX + pad * eY * autocvar_touch_hud_y / max(autocvar_touch_hud_x, 0.01);
}

vector Touch_Hud_AmmoPanelSize(void)
{
	float u = Touch_ScreenMin() * 0.22 * Touch_WidgetScale();
	return u * 1.35 * eX + u * eY;
}

vector Touch_Hud_AmmoPanelOrigin(void)
{
	vector vf = getpropertyvec(VF_SIZE);
	vector sz = Touch_Hud_AmmoPanelSize();
	float pad = Touch_ScreenMin() * autocvar_touch_ammo_x;
	return (vf.x - sz.x - pad) * eX + pad * eY * autocvar_touch_ammo_y / max(autocvar_touch_ammo_x, 0.01);
}

bool Touch_Hud_HitPanel(vector p)
{
	if (!Touch_Hud_ShouldDraw())
		return false;

	vector o = Touch_Hud_StatusPanelOrigin();
	vector sz = Touch_Hud_StatusPanelSize();
	if (p.x >= o.x && p.x <= o.x + sz.x && p.y >= o.y && p.y <= o.y + sz.y)
		return true;

	o = Touch_Hud_AmmoPanelOrigin();
	sz = Touch_Hud_AmmoPanelSize();
	if (p.x >= o.x && p.x <= o.x + sz.x && p.y >= o.y && p.y <= o.y + sz.y)
		return true;

	return false;
}

// Panels share the controls' surface/rim/shadow language so the HUD and the
// controls read as one overlay instead of two unrelated skins.
void Touch_Hud_DrawPanel(vector origin, vector size, float alpha)
{
	vector half = size * 0.5;
	vector center = origin + half;
	if (!Touch_Shape_Ready())
	{
		drawfill_builtin(origin, size, TOUCH_C_SHADOW, alpha * TOUCH_A_SHADOW, DRAWFLAG_NORMAL);
		drawfill_builtin(origin + '1 1 0', size - '2 2 0', TOUCH_C_SURFACE,
			alpha * TOUCH_A_SURFACE, DRAWFLAG_NORMAL);
		return;
	}
	float r = TOUCH_R_MD * Touch_UIScale();
	Touch_Shape_RRect(center + eY * Touch_Widget_Elevation(half.y), half, r,
		TOUCH_C_SHADOW, alpha * TOUCH_A_SHADOW * 0.7);
	Touch_Shape_RRect(center, half, r, TOUCH_C_SURFACE, alpha * TOUCH_A_SURFACE_ACTIVE);
}

// Capsule bars: a stat bar is read at a glance from its *length*, so the track
// stays visible at ghost alpha rather than vanishing when the value is low.
void Touch_Hud_DrawBar(vector pos, vector size, float frac, vector fill_color, float alpha)
{
	frac = bound(0, frac, 1);
	vector half = size * 0.5;
	vector center = pos + half;
	if (!Touch_Shape_Ready() || half.y < 2)
	{
		drawfill_builtin(pos, size, TOUCH_C_SHADOW, alpha * 0.4, DRAWFLAG_NORMAL);
		if (frac > 0.001)
			drawfill_builtin(pos, eX * (size.x * frac) + eY * size.y, fill_color, alpha * 0.9, DRAWFLAG_NORMAL);
		return;
	}
	Touch_Shape_Capsule(center, half, TOUCH_C_RIM, alpha * TOUCH_A_GHOST);
	float w = size.x * frac;
	if (w > size.y)
		Touch_Shape_Capsule(pos + eX * (w * 0.5) + eY * half.y, eX * (w * 0.5) + eY * half.y,
			fill_color, alpha * 0.92);
}

void Touch_Hud_DrawStatusPanel(float alpha)
{
	vector origin = Touch_Hud_StatusPanelOrigin();
	vector panel = Touch_Hud_StatusPanelSize();
	float portrait = panel.y * 0.82;
	vector portrait_pos = origin + eX * (panel.y * 0.10) + eY * (panel.y - portrait) * 0.5;

	Touch_Hud_DrawPanel(origin, panel, alpha);

	string pic = Touch_Hud_ResolvePortrait();
	if (pic != "" && draw_getimagesize(pic) != '0 0 0')
		drawpic_builtin(portrait_pos, pic, portrait * eX + portrait * eY, '1 1 1', alpha, DRAWFLAG_NORMAL);
	else
		Touch_Hud_DrawSkinPic(portrait_pos, portrait * eX + portrait * eY, "health", alpha * 0.85);

	vector portrait_center = portrait_pos + '0.5 0.5 0' * portrait;
	if (Touch_Shape_Ready())
		Touch_Shape_Ring(portrait_center, portrait * 0.52, TOUCH_W_RING_THIN, TOUCH_C_RIM,
			alpha * TOUCH_A_RIM);

	int health = max(0, STAT(HEALTH));
	int armor = max(0, STAT(ARMOR));
	int maxhealth = max(100, autocvar_touch_hud_maxhealth);
	int maxarmor = max(100, autocvar_touch_hud_maxarmor);

	vector text_origin = origin + eX * (panel.y * 0.10 + portrait * 1.10) + eY * (panel.y * 0.16);
	vector bar_size = eX * (panel.x - (panel.y * 0.10 + portrait * 1.10) - panel.y * 0.14)
		+ eY * (panel.y * 0.12);
	vector label_sz = '1 1 0' * panel.y * 0.16;

	vector hp_color = HUD_Get_Num_Color(health, maxhealth, true);
	vector ar_color = HUD_Get_Num_Color(armor, maxarmor, true);

	// Number first, bar under it: the exact value matters in a duel, the bar is
	// the peripheral-vision cue.
	Touch_Shape_Label(text_origin + eX * (bar_size.x * 0.5) + eY * (label_sz.y * 0.5),
		label_sz.y, strcat(ftos(health), " HP"), hp_color, alpha * TOUCH_A_INK);
	Touch_Hud_DrawBar(text_origin + eY * (panel.y * 0.22), bar_size, health / maxhealth, hp_color, alpha);

	Touch_Shape_Label(text_origin + eX * (bar_size.x * 0.5) + eY * (panel.y * 0.44 + label_sz.y * 0.5),
		label_sz.y, strcat(ftos(armor), " AR"), ar_color, alpha * TOUCH_A_INK);
	Touch_Hud_DrawBar(text_origin + eY * (panel.y * 0.66), bar_size, armor / maxarmor, ar_color, alpha);
}

void Touch_Hud_DrawAmmoPanel(float alpha)
{
	entity wepent = viewmodels[0];
	Weapon wep = wepent.switchweapon;
	if (!wep)
		return;

	vector origin = Touch_Hud_AmmoPanelOrigin();
	vector panel = Touch_Hud_AmmoPanelSize();
	Touch_Hud_DrawPanel(origin, panel, alpha);

	float icon = panel.y * 0.72;
	vector icon_pos = origin + eX * (panel.y * 0.14) + eY * (panel.y - icon) * 0.5;
	if (wep.model2 != "")
		Touch_Hud_DrawSkinPic(icon_pos, icon * eX + icon * eY, wep.model2, alpha);

	float text_sz = panel.y * 0.30;
	string ammo_text;

	bool infinite = (STAT(ITEMS) & IT_UNLIMITED_AMMO);
	int clip = wepent.clip_load;
	int clipmax = wepent.clip_size;

	if (infinite)
		ammo_text = "\xE2\x88\x9E";
	else if (clipmax > 0 && clip >= 0)
	{
		int reserve = getstati(GetAmmoStat(wep.ammo_type));
		ammo_text = strcat(ftos(clip), " / ", ftos(reserve));
	}
	else if (wep.ammo_type != RES_NONE)
		ammo_text = ftos(getstati(GetAmmoStat(wep.ammo_type)));
	else
		ammo_text = "--";

	vector col = TOUCH_C_INK;
	if (!infinite && clipmax > 0 && clip <= 0)
		col = TOUCH_C_DANGER;
	else if (!infinite && clipmax > 0 && clip <= clipmax * 0.25)
		col = TOUCH_C_WARNING;

	// Centred in the space left of the icon so the count does not drift as the
	// digit count changes.
	float text_left = origin.x + panel.y * 0.14 + icon * 1.05;
	Touch_Shape_Label(eX * ((text_left + origin.x + panel.x - panel.y * 0.14) * 0.5)
		+ eY * (origin.y + panel.y * 0.5), text_sz, ammo_text, col, alpha * TOUCH_A_INK);
}

void Touch_Hud_Draw(float alpha)
{
	if (!Touch_Hud_ShouldDraw())
		return;
	if (spectatee_status == -1)
		return;
	if (STAT(HEALTH) <= 0)
		return;

	Touch_Hud_DrawStatusPanel(alpha);
	Touch_Hud_DrawAmmoPanel(alpha);
}
