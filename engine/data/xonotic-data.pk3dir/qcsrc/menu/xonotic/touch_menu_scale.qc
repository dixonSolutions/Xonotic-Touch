// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_scale.qh"

#include "../menu.qh"

AUTOCVAR_SAVE(menu_touch_size, float, 1, "Godnotic: menu size on a touch screen (0.6 to 1.6, 1 = sized from the screen's density; -1 = the stock desktop layout). Applies when the menu restarts");

bool TouchMenuScale_active()
{
	if (autocvar_menu_touch_size < 0)
		return false; // -1: stock layout everywhere
	return cvar("vid_touchscreen") || cvar("vid_touchscreen_detected");
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
	// What the Linux launcher measured (screen-calc.sh); 160 is its "unknown"
	// placeholder and 300 the engine default nobody set.
	dpi = cvar("vid_touchscreen_ydpi");
	if (dpi >= 72 && dpi != 160 && dpi != 300)
		return dpi;
	// Nothing known: guess a phone, whose short side is about 2.7 inches.
	float px_short = min(cvar("vid_conwidth"), cvar("vid_conheight")) * TouchMenuScale_pxPerCon();
	return max(96, px_short / 2.7);
}

float TouchMenuScale_conToDp(float conunits)
{
	return conunits * TouchMenuScale_pxPerCon() * 160 / TouchMenuScale_dpi();
}

float TouchMenuScale_compute()
{
	if (!TouchMenuScale_active())
		return 1;
	float con_short = min(realconwidth, realconheight);
	if (con_short <= 0)
		return 1;
	float short_dp = TouchMenuScale_conToDp(con_short);
	float size = autocvar_menu_touch_size ? bound(0.6, autocvar_menu_touch_size, 1.6) : 1;
	float layout = max(TOUCH_MENU_MIN_LAYOUT, short_dp / (TOUCH_MENU_DP_PER_UNIT * size));
	// Only ever larger than stock: a touch monitor already showing the stock
	// layout big keeps it.
	return max(1, con_short / layout);
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
	if (realconheight > realconwidth)
	{
		conwidth = realconwidth / s;
		conheight = realconheight / s;
	}
	else
	{
		conheight = realconheight / s;
		conwidth = min(realconwidth / s, conheight * TOUCH_MENU_MAX_ASPECT);
	}
}

bool TouchMenuScale_drawReset()
{
	if (touch_menu_scale <= 1)
		return false;
	float s = touch_menu_scale;
	draw_reset(conwidth * s, conheight * s, 0.5 * (realconwidth - conwidth * s), 0.5 * (realconheight - conheight * s));
	return true;
}

void TouchMenuScale_applySkin()
{
	if (!TouchMenuScale_active())
		return;
	// Compact dialog margins, so the 24-row dialogs fit a phone's short side
	// at a larger size. Rows keep their full height; the spacing between them
	// shrinks from 4 to 2 units.
	SKINMARGIN_TOP = min(SKINMARGIN_TOP, 4);
	SKINMARGIN_BOTTOM = min(SKINMARGIN_BOTTOM, 6);
	SKINMARGIN_ROWS = min(SKINMARGIN_ROWS, 2);
}
