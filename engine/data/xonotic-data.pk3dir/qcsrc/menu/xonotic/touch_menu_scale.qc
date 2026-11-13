// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_scale.qh"

#include "../menu.qh"

AUTOCVAR_SAVE(menu_touch_size, float, 1, "Godnotic: menu size on a touch screen (0.6 to 1.6, 1 = sized from the screen's density; -1 = the stock desktop layout; anything but 1 on a desktop forces the touch layout). Applies when the menu restarts");

bool TouchMenuScale_active()
{
	if (autocvar_menu_touch_size < 0)
		return false; // -1: stock layout everywhere
	if (cvar("vid_touchscreen") || cvar("vid_touchscreen_detected"))
		return true;
	// A desktop asked for the touch layout explicitly (anything but the default 1).
	return autocvar_menu_touch_size != 1 && autocvar_menu_touch_size > 0;
}

// Real pixels per console unit (scr_sbarscale is vid.mode.height / vid_conheight).
float TouchMenuScale_pxPerCon()
{
	float f = cvar("scr_sbarscale");
	if (f <= 0 && cvar("vid_conheight") > 0)
		f = cvar("vid_height") / cvar("vid_conheight");
	return (f > 0) ? f : 1;
}

float TouchMenuScale_dpi()
{
	// vid_touch_dpi: the engine's reading of the display (touch_display.c,
	// SDL_GetDisplayDPI: Android DisplayMetrics, the Wayland/X11 output size).
	float dpi = cvar("vid_touch_dpi");
	if (dpi >= 72)
		return dpi;
	// Android's own density, as the activity reports it with the insets.
	if (tokenize_console(cvar_string("_touch_safe_insets")) >= 5 && stof(argv(4)) > 0.4)
		return stof(argv(4)) * 160;
	// What the Linux launcher measured (screen-calc.sh); 160 is its "unknown"
	// placeholder and 300 the engine default nobody set.
	dpi = cvar("vid_touchscreen_ydpi");
	if (dpi >= 72 && dpi != 160 && dpi != 300)
		return dpi;
	// Nothing known on a desktop that asked for the touch layout: one dp
	// per pixel, as a desktop's 100% scale.
	if (!cvar("vid_touchscreen") && !cvar("vid_touchscreen_detected"))
		return 160;
	// Nothing known on a touch screen: guess a phone, whose short side is
	// about 2.7 inches.
	float px_short = min(cvar("vid_conwidth"), cvar("vid_conheight")) * TouchMenuScale_pxPerCon();
	return max(96, px_short / 2.7);
}

float TouchMenuScale_conToDp(float conunits)
{
	return conunits * TouchMenuScale_pxPerCon() * 160 / TouchMenuScale_dpi();
}

float TouchMenuScale_dpToUnits(float dp)
{
	return (touch_menu_unit_dp > 0) ? dp / touch_menu_unit_dp : dp;
}

float TouchMenuScale_size()
{
	return (autocvar_menu_touch_size > 0) ? bound(0.6, autocvar_menu_touch_size, 1.6) : 1;
}

float TouchMenuScale_compute()
{
	if (!TouchMenuScale_active())
		return 1;
	float con_short = min(realconwidth, realconheight);
	if (con_short <= 0)
		return 1;
	// Console units per layout unit so one unit is DP_PER_UNIT dp.
	float s = TOUCH_MENU_DP_PER_UNIT * TouchMenuScale_size() / TouchMenuScale_conToDp(1);
	// Never so big that a layout is shorter than about seven rows.
	s = min(s, con_short / 200);
	// Only ever larger than stock: a touch monitor already showing the stock
	// layout big keeps it.
	return max(1, s);
}

// Left and right console units kept clear: display cutouts, and on the right
// the engine's console button (touch_shellbtn.c), which sits in the top right
// corner over whatever a full-width dialog puts there (its close button).
void TouchMenuScale_reserve()
{
	float l = 0, r = 0, t = 0, b = 0, density = 0;
	float px2con = 1 / TouchMenuScale_pxPerCon();
	int n = tokenize_console(cvar_string("_touch_safe_insets"));
	if (n >= 4)
	{
		l = stof(argv(0)) * px2con;
		t = stof(argv(1)) * px2con;
		r = stof(argv(2)) * px2con;
		b = stof(argv(3)) * px2con;
	}
	// Never more than a fifth of the height between them.
	touch_menu_reserve_t = min(t, realconheight * 0.1);
	touch_menu_reserve_b = min(b, realconheight * 0.1);
	if (n >= 5)
		density = stof(argv(4));
	if (cvar("touch_shell_button") >= 1)
	{
		// The density touch_shellbtn.c sizes the button with: the platform's,
		// else its guess from the short side.
		if (density <= 0)
		{
			float shortpx = min(cvar("vid_width"), cvar("vid_height"));
			density = (shortpx >= 900) ? bound(1, shortpx / 420, 4) : 1;
		}
		// Its size and 8 dp margin, plus 8 dp between it and the dialog.
		float bs = bound(32, cvar("touch_shell_button_size"), 96);
		r += (bs + 16) * density * px2con;
	}
	touch_menu_reserve_l = l;
	touch_menu_reserve_r = r;
}

float TouchMenuScale_availWidth()
{
	return max(realconwidth * 0.5, realconwidth - touch_menu_reserve_l - touch_menu_reserve_r);
}

void TouchMenuScale_layout()
{
	if (!touch_menu_scale_fixed)
	{
		touch_menu_scale = TouchMenuScale_compute();
		// The dialogs take their proportions from the first layout; keep it.
		if (main)
			touch_menu_scale_fixed = true;
	}
	if (touch_menu_scale <= 1)
		return;
	float s = touch_menu_scale;
	TouchMenuScale_reserve();
	float avail_w = TouchMenuScale_availWidth();
	float avail_h = realconheight - touch_menu_reserve_t - touch_menu_reserve_b;
	if (realconheight > realconwidth)
	{
		conwidth = avail_w / s;
		conheight = avail_h / s;
	}
	else
	{
		conheight = avail_h / s;
		conwidth = min(avail_w / s, conheight * TOUCH_MENU_MAX_ASPECT);
	}
}

bool TouchMenuScale_drawReset()
{
	if (touch_menu_scale <= 1)
		return false;
	float s = touch_menu_scale;
	float avail_w = TouchMenuScale_availWidth();
	float avail_h = realconheight - touch_menu_reserve_t - touch_menu_reserve_b;
	float left = min(touch_menu_reserve_l, realconwidth - avail_w);
	draw_reset(conwidth * s, conheight * s, left + 0.5 * (avail_w - conwidth * s), touch_menu_reserve_t + 0.5 * (avail_h - conheight * s));
	return true;
}

void TouchMenuScale_applySkin()
{
	touch_menu_on = false;
	if (!TouchMenuScale_active())
		return;
	if (!touch_menu_scale_fixed)
		touch_menu_scale = TouchMenuScale_compute();
	touch_menu_on = true;
	touch_menu_unit_dp = TouchMenuScale_conToDp(max(1, touch_menu_scale));
	float sz = TouchMenuScale_size();
	// Rows: a finger target each, with a finger's gap between them. The
	// fonts stay as the skin has them; the rows grow around them.
	float row = TouchMenuScale_dpToUnits(TOUCH_MENU_ROW_DP * sz);
	float gap = TouchMenuScale_dpToUnits(TOUCH_MENU_GAP_DP * sz);
	SKINHEIGHT_NORMAL = max(SKINHEIGHT_NORMAL, row / SKINFONTSIZE_NORMAL);
	SKINHEIGHT_TITLE = max(SKINHEIGHT_TITLE, row / SKINFONTSIZE_TITLE);
	SKINMARGIN_ROWS = max(SKINMARGIN_ROWS, gap);
	SKINMARGIN_COLUMNS = max(SKINMARGIN_COLUMNS, gap);
	SKINMARGIN_TOP = max(SKINMARGIN_TOP, gap);
	SKINMARGIN_BOTTOM = max(SKINMARGIN_BOTTOM, gap);
	// No scroll bar column: a finger drags a list from anywhere on it, and
	// a thin indicator inside the list shows where it is (touch_menu_sheet.qc).
	SKINWIDTH_SCROLLBAR = 0;
}

// Data lists: compact rows, many in view; a list can set its own.
bool TouchMenuScale_dataList(entity me)
{
	switch (me.classname)
	{
		case "XonoticServerList":
		case "XonoticMapList":
		case "XonoticDemoList":
		case "XonoticScreenshotList":
		case "XonoticCvarList":
		case "XonoticKeyBinder":
		case "XonoticPlayList":
		case "XonoticSoundList":
		case "XonoticStatsList":
			return true;
	}
	return false;
}

float TouchMenuScale_listItemHeight(entity me, float itemHeight, vector absSize)
{
	if (!touch_menu_on || absSize.y <= 0)
		return itemHeight;
	float dp = me.touch_list_row_dp;
	if (dp <= 0)
		dp = TouchMenuScale_dataList(me) ? TOUCH_MENU_DATAROW_DP : TOUCH_MENU_LISTROW_DP;
	float h = TouchMenuScale_dpToUnits(dp * TouchMenuScale_size()) / absSize.y;
	// A data list keeps its own row when that is taller (two-line rows).
	return TouchMenuScale_dataList(me) ? max(min(itemHeight, h * 1.6), h) : max(itemHeight, h);
}

// The item's height in layout units, from its font size: Label_resizeNotify
// keeps realFontSize = fontSize / absSize.
vector TouchMenuSkin_absSize(entity me)
{
	if (me.realFontSize.x <= 0 || me.realFontSize.y <= 0)
		return '0 0 0';
	return eX * (me.fontSize / me.realFontSize.x) + eY * (me.fontSize / me.realFontSize.y);
}

vector TouchMenuSkin_box(entity me, vector stockSize)
{
	touch_menu_box_origin = eY * (0.5 * (1 - stockSize.y)) + eX * (0.5 * (me.keepspaceLeft - stockSize.x));
	if (!touch_menu_on)
		return stockSize;
	vector abs = TouchMenuSkin_absSize(me);
	if (abs.x <= 0 || abs.y <= 0)
		return stockSize;
	float u = TouchMenuScale_dpToUnits(TOUCH_MENU_BOX_DP * TouchMenuScale_size());
	vector sz = eX * min(u / abs.x, max(stockSize.x, me.keepspaceLeft)) + eY * min(u / abs.y, 1);
	if (sz.x < stockSize.x || sz.y < stockSize.y)
		return stockSize;
	touch_menu_box_origin = eY * (0.5 * (1 - sz.y)) + eX * (0.5 * (me.keepspaceLeft - sz.x));
	// Checked reads at a glance: the box filled behind its tick or dot.
	if (me.checked && !me.disabled)
	{
		vector inset = eX * (0.14 * sz.x) + eY * (0.14 * sz.y);
		draw_Fill(touch_menu_box_origin + inset, sz - 2 * inset, '0.25 0.6 1', 0.6);
	}
	return sz;
}

bool TouchMenuSkin_slider(entity me)
{
	if (!touch_menu_on || me.valueMax <= me.valueMin)
		return false;
	vector abs = TouchMenuSkin_absSize(me);
	if (abs.x <= 0 || abs.y <= 0)
		return false;
	float sz = TouchMenuScale_size();
	float th = min(1, TouchMenuScale_dpToUnits(TOUCH_MENU_TRACK_DP * sz) / abs.y);
	float dh = min(1, TouchMenuScale_dpToUnits(TOUCH_MENU_THUMB_DP * sz) / abs.y);
	float dw = dh * abs.y / abs.x;
	// The thumb's centre runs where the stock thumb's did, so a press lands
	// on the value it shows.
	float x0 = 0.5 * me.controlWidth;
	float x1 = 1 - me.valueSpace - 0.5 * me.controlWidth;
	if (x1 <= x0)
		return false;
	float f = bound(0, (me.sliderValue - me.valueMin) / (me.valueMax - me.valueMin), 1);
	float cx = x0 + f * (x1 - x0);
	vector ty = eY * (0.5 * (1 - th));
	draw_Fill(ty + eX * x0, eX * (x1 - x0) + eY * th, '1 1 1', 0.22);
	if (!me.disabled)
		draw_Fill(ty + eX * x0, eX * (cx - x0) + eY * th, '0.3 0.68 1', 0.9);
	vector c = '0.82 0.9 1';
	if (me.disabled)
		c = '0.5 0.5 0.5';
	else if (me.pressed)
		c = '0.45 0.8 1';
	else if (me.focused)
		c = '1 1 1';
	vector o = eX * (cx - 0.5 * dw) + eY * (0.5 * (1 - dh));
	// A soft halo while it is held, as the finger covers the thumb.
	if (me.pressed)
		draw_Picture(o - eX * (0.4 * dw) - eY * (0.4 * dh), "/gfx/touch/disc", eX * (1.8 * dw) + eY * (1.8 * dh), '0.3 0.68 1', 0.25);
	draw_Picture(o, "/gfx/touch/disc", eX * dw + eY * dh, c, 1);
	return true;
}
