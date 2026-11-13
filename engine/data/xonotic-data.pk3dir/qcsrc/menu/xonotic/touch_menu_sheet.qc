// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_sheet.qh"

#include "../menu.qh"
#include "../item/modalcontroller.qh"
#include "touch_menu_scale.qh"
#include "dialog_touch_launcher.qh"

bool touch_sheet_pending;    // a press held back until it is a tap or a drag
entity touch_sheet_target;   // the sheet the press landed on
entity touch_sheet_scrolling;
entity touch_sheet_fling;
vector touch_sheet_p0;       // where the press landed
float touch_sheet_o0;        // the sheet's origin.y then
vector touch_sheet_last;
float touch_sheet_lasttime;
float touch_sheet_vel;       // layout heights per second
float touch_sheet_bar;       // scroll bar emphasis, 0..1
bool touch_sheet_resize;     // children need their absolute positions again
entity touch_sheet_focus;    // the focused control last frame

entity TouchSheet_front()
{
	entity f = NULL;
	for (entity e = main.firstChild; e; e = e.nextSibling)
		if (e.ModalController_state)
			f = e;
	return f;
}

bool TouchSheet_scrolls(entity e)
{
	return e && e.touch_sheet_h > 0 && e.touch_sheet_max - e.touch_sheet_min > 0.001;
}

float TouchSheet_margin()
{
	return TouchMenuScale_dpToUnits(6) / conheight;
}

// Give every root dialog of the main window its real height back, scrolled
// to the top, where it is taller than the screen.
void TouchSheet_setup()
{
	float m = TouchSheet_margin();
	for (entity e = main.firstChild; e; e = e.nextSibling)
	{
		if (!e.instanceOfDialog || e.touch_sheet_h > 0)
			continue;
		float h = e.intendedHeight;
		e.touch_sheet_h = max(0.001, h);
		if (h <= 1 - 2 * m)
		{
			e.touch_sheet_min = e.touch_sheet_max = e.ModalController_initialOrigin.y;
			continue;
		}
		e.touch_sheet_max = m;
		e.touch_sheet_min = 1 - m - h;
		e.ModalController_initialSize_y = h;
		e.ModalController_initialOrigin_y = m;
		e.Container_size_y = h;
		e.Container_origin_y = m;
		touch_sheet_resize = true;
	}
}

void TouchSheet_setScroll(entity e, float y)
{
	y = bound(e.touch_sheet_min, y, e.touch_sheet_max);
	e.ModalController_initialOrigin_y = y;
	if (e.ModalController_state != 1 || e.ModalController_factor >= 1)
		e.Container_origin_y = y;
}

void TouchSheet_doResize()
{
	touch_sheet_resize = false;
	vector sz = eX * conwidth + eY * conheight;
	main.resizeNotify(main, '0 0 0', sz, '0 0 0', sz);
}

// The innermost item under pos, through the shown tab of each tab controller.
entity TouchSheet_leafAt(vector pos)
{
	entity e = TouchSheet_front();
	if (!e)
		return NULL;
	vector p = globalToBox(pos, e.Container_origin, e.Container_size);
	while (e.instanceOfContainer)
	{
		entity c = NULL;
		if (e.instanceOfModalController)
		{
			for (entity x = e.firstChild; x; x = x.nextSibling)
				if (x.ModalController_state == 1)
					c = x;
		}
		else
			c = e.itemFromPoint(e, p);
		if (!c)
			break;
		p = globalToBox(p, c.Container_origin, c.Container_size);
		e = c;
	}
	return e;
}

// A dialog opened by a button saves the focus of everything under it
// (DialogOpenButton_Click, saveFocus), and Container_setFocus hands the
// next focus to that saved item, whatever was asked for. With a mouse the
// hover after the dialog closes takes it once and the next hover puts it
// right. A finger's press is the first event at the new spot: it went to
// the saved button (the tile that opened the last sheet), outside it, and
// the tap was lost -- every other tap on the launcher. A press means the
// item under it, so the containers it goes through forget what they saved.
void TouchSheet_dropSavedFocus(vector pos)
{
	entity e = main;
	vector p = pos;
	for (int i = 0; e && e.instanceOfContainer && i < 32; ++i)
	{
		e.savedFocus = NULL;
		entity c = NULL;
		if (e.instanceOfModalController)
		{
			for (entity x = e.firstChild; x; x = x.nextSibling)
				if (x.ModalController_state)
					c = x; // the front one, as ModalController_draw picks it
		}
		else
			c = e.itemFromPoint(e, p);
		if (!c)
			break;
		p = globalToBox(p, c.Container_origin, c.Container_size);
		e = c;
	}
}

void TouchSheet_mousePress(vector pos)
{
	TouchSheet_dropSavedFocus(pos);
	touch_sheet_pending = false;
	touch_sheet_scrolling = NULL;
	if (touch_sheet_fling)
	{
		touch_sheet_fling = NULL; // a finger stops a fling, and that is all it does
		touch_sheet_resize = true;
	}
	entity f = TouchSheet_front();
	if (!touch_menu_on || !TouchSheet_scrolls(f))
	{
		main.mousePress(main, pos);
		return;
	}
	entity leaf = TouchSheet_leafAt(pos);
	if ((leaf && leaf.instanceOfListBox)
		|| pos.y < f.Container_origin.y || pos.y > f.Container_origin.y + f.Container_size.y
		|| pos.x < f.Container_origin.x || pos.x > f.Container_origin.x + f.Container_size.x)
	{
		main.mousePress(main, pos);
		return;
	}
	touch_sheet_pending = true;
	touch_sheet_target = f;
	touch_sheet_p0 = pos;
}

void TouchSheet_mouseDrag(vector pos)
{
	if (touch_sheet_pending)
	{
		vector d = pos - touch_sheet_p0;
		float dx = fabs(d.x) * conwidth * touch_menu_unit_dp;
		float dy = fabs(d.y) * conheight * touch_menu_unit_dp;
		if (max(dx, dy) < TOUCH_SHEET_SLOP_DP)
			return;
		touch_sheet_pending = false;
		if (dy >= dx)
		{
			touch_sheet_scrolling = touch_sheet_target;
			touch_sheet_o0 = touch_sheet_target.ModalController_initialOrigin.y;
			touch_sheet_last = pos;
			touch_sheet_lasttime = time;
			touch_sheet_vel = 0;
		}
		else
		{
			// Sideways: the control under the finger gets the press after all.
			main.mousePress(main, touch_sheet_p0);
			main.mouseDrag(main, pos);
			return;
		}
	}
	if (touch_sheet_scrolling)
	{
		entity e = touch_sheet_scrolling;
		TouchSheet_setScroll(e, touch_sheet_o0 + pos.y - touch_sheet_p0.y);
		float dt = time - touch_sheet_lasttime;
		if (dt > 0.001)
		{
			float v = (pos.y - touch_sheet_last.y) / dt;
			touch_sheet_vel = (dt > 0.1) ? v : 0.6 * v + 0.4 * touch_sheet_vel;
			touch_sheet_last = pos;
			touch_sheet_lasttime = time;
		}
		touch_sheet_bar = 1;
		return;
	}
	main.mouseDrag(main, pos);
}

void TouchSheet_mouseRelease(vector pos)
{
	if (touch_sheet_pending)
	{
		// A tap: the control gets its press and release together.
		touch_sheet_pending = false;
		main.mousePress(main, touch_sheet_p0);
		main.mouseRelease(main, touch_sheet_p0);
		return;
	}
	if (touch_sheet_scrolling)
	{
		entity e = touch_sheet_scrolling;
		touch_sheet_scrolling = NULL;
		if (time - touch_sheet_lasttime > 0.12)
			touch_sheet_vel = 0; // the finger stopped before it lifted
		if (fabs(touch_sheet_vel) > 0.2)
			touch_sheet_fling = e;
		else
			touch_sheet_resize = true;
		return;
	}
	main.mouseRelease(main, pos);
}

// Where an item is, in the main window's units.
vector TouchSheet_rectInMain(entity it)
{
	vector o = '0 0 0', s = '1 1 0';
	for (entity e = it; e && e != main; e = e.parent)
	{
		o = boxToGlobal(o, e.Container_origin, e.Container_size);
		s = boxToGlobalSize(s, e.Container_size);
	}
	return o + eZ * s.y; // .z carries the height
}

void TouchSheet_followFocus(entity f)
{
	entity leaf = f;
	while (leaf.focusedChild)
		leaf = leaf.focusedChild;
	if (leaf == touch_sheet_focus)
		return;
	touch_sheet_focus = leaf;
	if (leaf == f || mouseButtonsPressed || !TouchSheet_scrolls(f))
		return;
	// Only a control: a container focused with nothing in it (the pointer
	// over a disabled button or an empty part of a tab) is the whole page.
	if (leaf.instanceOfContainer)
		return;
	vector r = TouchSheet_rectInMain(leaf);
	float m = TouchSheet_margin() * 4;
	if (r.z > 1 - 2 * m)
		return;
	float y = f.ModalController_initialOrigin.y;
	if (r.y < m)
		y += m - r.y;
	else if (r.y + r.z > 1 - m)
		y -= r.y + r.z - (1 - m);
	else
		return;
	TouchSheet_setScroll(f, y);
	touch_sheet_resize = true;
}

void TouchSheet_preDraw()
{
	if (!touch_menu_on)
		return;
	TouchSheet_setup();
	TouchLauncher_heal();

	entity f = TouchSheet_front();
	for (entity e = main.firstChild; e; e = e.nextSibling)
	{
		// Each time a sheet opens it starts at the top.
		if (e.ModalController_state == 1 && !e.touch_sheet_was_shown && TouchSheet_scrolls(e))
			TouchSheet_setScroll(e, e.touch_sheet_max);
		e.touch_sheet_was_shown = (e.ModalController_state != 0);
	}

	if (touch_sheet_fling)
	{
		entity e = touch_sheet_fling;
		float y = e.ModalController_initialOrigin.y + touch_sheet_vel * frametime;
		touch_sheet_vel *= pow(0.03, frametime);
		TouchSheet_setScroll(e, y);
		if (fabs(touch_sheet_vel) < 0.05 || y <= e.touch_sheet_min || y >= e.touch_sheet_max || e.ModalController_state != 1)
		{
			touch_sheet_fling = NULL;
			touch_sheet_resize = true;
		}
		touch_sheet_bar = 1;
	}
	if (touch_sheet_resize && !touch_sheet_scrolling && !touch_sheet_fling)
		TouchSheet_doResize();

	if (f)
		TouchSheet_followFocus(f);

	// Behind a sheet the main screen (the banner, the launcher) steps back,
	// so a dialog over the banner reads as a sheet on top of it, not a
	// window cut into it.
	if (f && f != touch_launcher && !(gamestatus & (GAME_CONNECTED | GAME_ISSERVER)))
	{
		float a = draw_alpha;
		draw_reset_full();
		draw_Fill('0 0 0', '1 1 0', '0 0 0', 0.6 * a);
		draw_reset_cropped();
		draw_alpha = a;
	}
}

void TouchSheet_postDraw()
{
	if (!touch_menu_on)
		return;
	entity f = TouchSheet_front();
	if (!TouchSheet_scrolls(f) || f.ModalController_factor < 1)
		return;
	if (!touch_sheet_scrolling && !touch_sheet_fling)
		touch_sheet_bar = max(0, touch_sheet_bar - frametime * 2);
	float range = f.touch_sheet_max - f.touch_sheet_min;
	float view = 1 - 2 * TouchSheet_margin();
	float thumb = max(0.08, view * view / f.touch_sheet_h);
	float t = (f.touch_sheet_max - f.Container_origin.y) / range;
	float w = TouchMenuScale_dpToUnits(4) / conwidth;
	float x = f.Container_origin.x + f.Container_size.x + w;
	if (x + w > 1)
		x = f.Container_origin.x + f.Container_size.x - 2 * w;
	vector top = eX * x + eY * TouchSheet_margin();
	draw_Fill(top, eX * w + eY * view, '1 1 1', 0.12 * draw_alpha);
	draw_Fill(top + eY * (t * (view - thumb)), eX * w + eY * thumb, '0.55 0.8 1', (0.45 + 0.5 * touch_sheet_bar) * draw_alpha);
}

bool TouchSheet_wheel(float key)
{
	if (!touch_menu_on || (key != K_MWHEELUP && key != K_MWHEELDOWN))
		return false;
	entity f = TouchSheet_front();
	if (!TouchSheet_scrolls(f))
		return false;
	entity leaf = TouchSheet_leafAt(menuMousePos);
	if (leaf && leaf.instanceOfListBox)
		return false;
	// Three rows a notch.
	float d = TouchMenuScale_dpToUnits(3 * (TOUCH_MENU_ROW_DP + TOUCH_MENU_GAP_DP) * TouchMenuScale_size()) / conheight;
	TouchSheet_setScroll(f, f.ModalController_initialOrigin.y + ((key == K_MWHEELUP) ? d : -d));
	touch_sheet_bar = 1;
	touch_sheet_resize = true;
	return true;
}
