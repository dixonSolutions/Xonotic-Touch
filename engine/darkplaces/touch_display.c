// SPDX-License-Identifier: GPL-3.0-or-later
/*
Godnotic: the display's density, read once the window exists.

vid_touch_dpi is what SDL reports for the display the window is on: on Android
the DisplayMetrics density (densityDpi, the figure Android's own dp are based
on), on Wayland and X11 the output's physical size. The menu derives its size
on a touch device from it (touch_menu_scale.qc); 0 means SDL did not know, and
the menu falls back to the launcher's measurement or a phone-sized guess.
vid_touchscreen_xdpi/ydpi are left alone: the Linux launcher sets them and
the touch HUD reads them.
*/

#include "quakedef.h"
#include "touch_display.h"
// OBJ_COMMON also goes into the dedicated server, which is built without SDL.
#if defined(__has_include)
# if __has_include(<SDL.h>)
#  include <SDL.h>
#  define TOUCH_DISPLAY_SDL 1
# endif
#endif

cvar_t vid_touch_dpi = {CF_CLIENT | CF_READONLY, "vid_touch_dpi", "0",
	"density of the display the game runs on, in dots per inch, as the system reports it (Android: DisplayMetrics.densityDpi); 0 = unknown. Read by the menu to size itself on a touch screen"};

void TouchDisplay_Init(void)
{
	Cvar_RegisterVariable(&vid_touch_dpi);
}

void TouchDisplay_Update(int displayindex)
{
	float ddpi = 0, hdpi = 0, vdpi = 0, dpi = 0;
	if (displayindex < 0)
		displayindex = 0;
#ifdef TOUCH_DISPLAY_SDL
	if (SDL_GetDisplayDPI(displayindex, &ddpi, &hdpi, &vdpi) == 0)
	{
		// The diagonal figure is densityDpi on Android and the physical
		// density elsewhere; the vertical one where only that is known.
		dpi = ddpi > 0 ? ddpi : vdpi;
		if (dpi < 50 || dpi > 1200)
			dpi = 0;
	}
#endif
	Cvar_SetValueQuick(&vid_touch_dpi, dpi);
	Con_DPrintf("touch display: %g dpi (SDL: diagonal %g, horizontal %g, vertical %g)\n", dpi, ddpi, hdpi, vdpi);
}
