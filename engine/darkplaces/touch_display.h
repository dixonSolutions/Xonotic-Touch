// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef TOUCH_DISPLAY_H
#define TOUCH_DISPLAY_H

// Godnotic: the display's density for the menu's touch layout
// (qcsrc/menu/xonotic/touch_menu_scale.qc). See touch_display.c.
void TouchDisplay_Init(void);              // vid_sdl.c, VID_Init
void TouchDisplay_Update(int displayindex); // vid_sdl.c, after the window is created

#endif
