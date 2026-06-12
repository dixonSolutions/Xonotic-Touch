#include "touch_hud.qh"
#include "touch_init.qh"
#include "touch_glass.qh"
#include "touch_layout.qh"
#include <client/main.qh>
#include <client/hud/hud.qh>
#include <client/view.qh>
#include <client/items/items.qh>
#include <common/items/item.qh>
#include <common/weapons/all.qh>

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
	float u = Touch_ScreenMin() * 0.42 * Touch_WidgetScale();
	return u * eX + u * 0.38 * eY;
}

vector Touch_Hud_StatusPanelOrigin(void)
{
	vector vf = getpropertyvec(VF_SIZE);
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

void Touch_Hud_DrawBar(vector pos, vector size, float frac, vector fill_color, float alpha)
{
	frac = bound(0, frac, 1);
	drawfill_builtin(pos, size, TOUCH_GLASS_SHADOW, alpha * 0.4, DRAWFLAG_NORMAL);
	if (frac > 0.001)
		drawfill_builtin(pos, eX * (size.x * frac) + eY * size.y, fill_color, alpha * 0.9, DRAWFLAG_NORMAL);
	drawfill_builtin(pos + eY * (size.y - 1), eX * size.x + eY, TOUCH_GLASS_HIGHLIGHT, alpha * 0.2, DRAWFLAG_NORMAL);
}

void Touch_Hud_DrawStatusPanel(float alpha)
{
	vector origin = Touch_Hud_StatusPanelOrigin();
	vector panel = Touch_Hud_StatusPanelSize();
	float portrait = panel.y * 0.92;
	vector portrait_pos = origin + eX * (portrait * 0.04) + eY * (panel.y - portrait) * 0.5;

	drawfill_builtin(origin, panel, TOUCH_GLASS_SHADOW, alpha * 0.35, DRAWFLAG_NORMAL);
	drawfill_builtin(origin + eX * 1 + eY * 1, panel - '2 2 0', TOUCH_GLASS_TINT, alpha * 0.28, DRAWFLAG_NORMAL);

	string pic = Touch_Hud_ResolvePortrait();
	if (pic != "" && draw_getimagesize(pic) != '0 0 0')
		drawpic_builtin(portrait_pos, pic, portrait * eX + portrait * eY, '1 1 1', alpha, DRAWFLAG_NORMAL);
	else
		Touch_Hud_DrawSkinPic(portrait_pos, portrait * eX + portrait * eY, "health", alpha * 0.85);

	Touch_Glass_DrawCircleOutline(portrait_pos + '0.5 0.5 0' * portrait, portrait * 0.52, TOUCH_GLASS_HIGHLIGHT, alpha * 0.45, 2);

	int health = max(0, STAT(HEALTH));
	int armor = max(0, STAT(ARMOR));
	int maxhealth = max(100, autocvar_touch_hud_maxhealth);
	int maxarmor = max(100, autocvar_touch_hud_maxarmor);

	vector text_origin = origin + eX * (portrait * 1.08) + eY * (panel.y * 0.18);
	vector bar_size = eX * (panel.x - portrait * 1.15) + eY * (panel.y * 0.14);
	vector label_sz = '1 1 0' * panel.y * 0.11;

	vector hp_color = HUD_Get_Num_Color(health, maxhealth, true);
	vector ar_color = HUD_Get_Num_Color(armor, maxarmor, true);

	drawstring_builtin(text_origin, strcat("HP ", ftos(health)), label_sz, hp_color, alpha, 0);
	Touch_Hud_DrawBar(text_origin + eY * (panel.y * 0.16), bar_size, health / maxhealth, hp_color, alpha);

	drawstring_builtin(text_origin + eY * (panel.y * 0.36), strcat("AR ", ftos(armor)), label_sz, ar_color, alpha, 0);
	Touch_Hud_DrawBar(text_origin + eY * (panel.y * 0.52), bar_size, armor / maxarmor, ar_color, alpha);
}

void Touch_Hud_DrawAmmoPanel(float alpha)
{
	entity wepent = viewmodels[0];
	Weapon wep = wepent.switchweapon;
	if (!wep)
		return;

	vector origin = Touch_Hud_AmmoPanelOrigin();
	vector panel = Touch_Hud_AmmoPanelSize();
	drawfill_builtin(origin, panel, TOUCH_GLASS_SHADOW, alpha * 0.35, DRAWFLAG_NORMAL);
	drawfill_builtin(origin + '1 1 0', panel - '2 2 0', TOUCH_GLASS_TINT, alpha * 0.28, DRAWFLAG_NORMAL);

	float icon = panel.y * 0.82;
	vector icon_pos = origin + eX * (panel.x * 0.04) + eY * (panel.y - icon) * 0.5;
	if (wep.model2 != "")
		Touch_Hud_DrawSkinPic(icon_pos, icon * eX + icon * eY, wep.model2, alpha);

	vector text_pos = origin + eX * (icon * 1.05) + eY * (panel.y * 0.22);
	vector text_sz = '1 1 0' * panel.y * 0.22;
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

	vector col = '1 1 1';
	if (!infinite && clipmax > 0 && clip <= 0)
		col = '0.95 0.2 0.2';
	else if (!infinite && clipmax > 0 && clip <= clipmax * 0.25)
		col = '1 0.85 0.2';

	drawstring_builtin(text_pos, ammo_text, text_sz, col, alpha, 0);
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
