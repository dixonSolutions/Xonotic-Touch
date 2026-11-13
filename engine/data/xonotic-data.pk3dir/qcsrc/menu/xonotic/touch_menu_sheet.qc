// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_sheet.qh"

#include "../menu.qh"
#include "../item/modalcontroller.qh"
#include "../item/inputbox.qh"
#include "touch_menu_scale.qh"
#include "dialog_touch_launcher.qh"
#include "../item/borderimage.qh"
#include "../item/checkbox.qh"
#include "../item/listbox.qh"

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
bool touch_sheet_swallow;    // the held press landed on a pinned band, off its buttons

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

// ---- Pinned chrome -------------------------------------------------------

.entity touch_pin_next;
.vector touch_pin_base;    // Container_origin before the shift
.float touch_pin_kind;     // 1 title (the close button), 2 action row
.float touch_pin_frame;    // the frame its draw state was recorded in
.vector touch_pin_shift, touch_pin_scale, touch_pin_font;
.float touch_pin_alpha;
entity touch_pin_head;
entity touch_pin_sheet;      // the sheet whose chrome is pinned; NULL none
float touch_pin_top;         // its title shift, in the sheet's own units
float touch_pin_bot;         // its action row shift (<= 0), same units
float touch_pin_title;       // title bar height, same units
float touch_pin_hdr_bottom;  // main-window y of the title bar's bottom
float touch_pin_ftr_top;     // main-window y of the action row band's top; 1 none
float touch_pin_framecount;
bool touch_pin_clipped;      // the content clip is on the draw stack
vector touch_pin_root_shift, touch_pin_root_scale, touch_pin_root_font;
float touch_pin_root_alpha;

void TouchSheet_unpin()
{
	for (entity e = touch_pin_head; e; )
	{
		entity n = e.touch_pin_next;
		e.Container_origin = e.touch_pin_base;
		e.touch_pin_kind = 0;
		e.touch_pin_next = NULL;
		e = n;
	}
	touch_pin_head = NULL;
	touch_pin_sheet = NULL;
	touch_pin_ftr_top = 1;
}

void TouchSheet_pin(entity e, float kind)
{
	if (e.touch_pin_kind)
		return;
	e.touch_pin_kind = kind;
	e.touch_pin_base = e.Container_origin;
	e.touch_pin_next = touch_pin_head;
	touch_pin_head = e;
}

// Items of c that count as its rows: not the frame, not the close button,
// not hidden.
bool TouchSheet_rowItem(entity c, entity e)
{
	return !e.instanceOfBorderImage && e != c.closeButton && e.Container_alpha >= 0.003;
}

entity TouchSheet_lowest(entity c)
{
	entity low = NULL;
	float b = -1;
	for (entity e = c.firstChild; e; e = e.nextSibling)
	{
		if (!TouchSheet_rowItem(c, e))
			continue;
		float eb = e.Container_origin.y + e.Container_size.y;
		if (eb > b + 0.0001)
		{
			b = eb;
			low = e;
		}
	}
	return low;
}

// The container whose last row is the sheet's action row: the dialog, or
// through each tab controller at its bottom, the tab it shows.
entity TouchSheet_footerHost(entity d)
{
	entity c = d;
	for (int i = 0; i < 8; ++i)
	{
		if (c.touch_sheet_nopin)
			return NULL;
		entity low = TouchSheet_lowest(c);
		if (!low)
			return NULL;
		if (!low.instanceOfModalController)
			return c;
		entity shown = NULL;
		for (entity x = low.firstChild; x; x = x.nextSibling)
			if (x.ModalController_state == 1)
				shown = x;
		if (!shown || !shown.instanceOfContainer)
			return NULL;
		c = shown;
	}
	return NULL;
}

// Which items of c lie in the band [y0, y1] (their centre does), and
// whether those make an action row: 1 every one a button or a label, with
// a button; 2 also text fields; 0 not a row to pin. A band with anything
// else (a checkbox, a slider, a list) is content.
float TouchSheet_rowKind(entity c, float y0, float y1, bool fields)
{
	bool button = false;
	for (entity e = c.firstChild; e; e = e.nextSibling)
	{
		if (!TouchSheet_rowItem(c, e))
			continue;
		float cy = e.Container_origin.y + 0.5 * e.Container_size.y;
		if (cy < y0 || cy > y1)
			continue;
		if (e.instanceOfCheckBox || e.instanceOfContainer || e.instanceOfListBox)
			return 0;
		if (e.instanceOfInputBox)
		{
			if (!fields)
				return 0;
			continue;
		}
		if (e.instanceOfButton)
			button = true;
		else if (!e.instanceOfLabel || e.instanceOfSlider)
			return 0;
	}
	return button ? 1 : 0;
}

void TouchSheet_pinBand(entity c, float y0, float y1)
{
	for (entity e = c.firstChild; e; e = e.nextSibling)
	{
		if (!TouchSheet_rowItem(c, e))
			continue;
		float cy = e.Container_origin.y + 0.5 * e.Container_size.y;
		if (cy >= y0 && cy <= y1)
			TouchSheet_pin(e, 2);
	}
}

float TouchSheet_view()
{
	return 1 - 2 * TouchSheet_margin();
}

// Every frame, before the draw: undo last frame's shifts, find the chrome
// of the front sheet again (the tab shown may have changed) and move it to
// the edges of the screen.
void TouchSheet_updatePins(entity f)
{
	TouchSheet_unpin();
	if (!f || !TouchSheet_scrolls(f) || f.ModalController_state != 1 || f.ModalController_factor < 1 || f.touch_sheet_nopin)
		return;
	float h = f.Container_size.y;
	if (h <= 0)
		return;
	touch_pin_sheet = f;
	touch_pin_top = (f.touch_sheet_max - f.Container_origin.y) / h;
	touch_pin_bot = (f.touch_sheet_min - f.Container_origin.y) / h;
	touch_pin_title = 0;
	if (f.border && f.titleFontSize)
		touch_pin_title = f.borderLines * f.titleHeight / (h * conheight);
	touch_pin_hdr_bottom = f.touch_sheet_max + touch_pin_title * h;
	if (f.closeButton)
		TouchSheet_pin(f.closeButton, 1);

	entity c = TouchSheet_footerHost(f);
	if (c)
	{
		entity low = TouchSheet_lowest(c);
		float top = low.Container_origin.y, bot = top + low.Container_size.y;
		float rh = bot - top;
		// Only a row at the bottom of the sheet: one in the middle of a
		// short tab is part of its page.
		vector lr = TouchSheet_rectInMain(low);
		float slack = 1.5 * TouchMenuScale_dpToUnits((TOUCH_MENU_ROW_DP + TOUCH_MENU_GAP_DP) * TouchMenuScale_size()) / conheight;
		bool atBottom = (f.Container_origin.y + h) - (lr.y + lr.z) < slack;
		if (atBottom && TouchSheet_rowKind(c, top, bot, false))
		{
			float y0 = top;
			// The row above, when it is an action row too.
			float up = top - 1.6 * rh;
			if (TouchSheet_rowKind(c, up, top - 0.0001, true))
			{
				float above = top;
				for (entity e = c.firstChild; e; e = e.nextSibling)
				{
					float cy = e.Container_origin.y + 0.5 * e.Container_size.y;
					if (TouchSheet_rowItem(c, e) && cy >= up && cy < top)
						above = min(above, e.Container_origin.y);
				}
				// Never more than about a third of the screen.
				if ((bot - above) * TouchSheet_rectInMain(c).z < 0.3 * TouchSheet_view())
					y0 = up;
			}
			if ((bot - top) * TouchSheet_rectInMain(c).z < 0.3 * TouchSheet_view())
				TouchSheet_pinBand(c, y0, bot);
		}
	}

	// The shifts, and where the action row's band starts on screen.
	touch_pin_ftr_top = 1;
	for (entity e = touch_pin_head; e; e = e.touch_pin_next)
	{
		entity p = e.parent;
		float ph = TouchSheet_rectInMain(p).z;
		if (ph <= 0)
			continue;
		if (e.touch_pin_kind == 1)
			e.Container_origin_y = e.touch_pin_base.y + touch_pin_top * h / ph;
		else
		{
			e.Container_origin_y = e.touch_pin_base.y + touch_pin_bot * h / ph;
			touch_pin_ftr_top = min(touch_pin_ftr_top, TouchSheet_rectInMain(e).y);
		}
	}
	if (touch_pin_ftr_top < 1)
		touch_pin_ftr_top -= 0.5 * TouchMenuScale_dpToUnits(TOUCH_MENU_GAP_DP * TouchMenuScale_size()) / conheight;
	++touch_pin_framecount;
}

bool TouchSheet_drawLater(entity me, entity e)
{
	if (!touch_pin_sheet)
		return false;
	if (me == main)
	{
		if (touch_pin_clipped)
		{
			draw_ClearClip(); // something drawn after the sheet
			touch_pin_clipped = false;
		}
		if (e == touch_pin_sheet)
		{
			touch_pin_root_shift = draw_shift;
			touch_pin_root_scale = draw_scale;
			touch_pin_root_font = draw_fontscale;
			touch_pin_root_alpha = draw_alpha;
			// Its content shows between the title bar and the action row.
			float h = e.Container_size.y;
			float y0 = (touch_pin_hdr_bottom - e.Container_origin.y) / h;
			float y1 = (touch_pin_ftr_top - e.Container_origin.y) / h;
			draw_SetClipRect(eY * y0, eX + eY * max(0, y1 - y0));
			touch_pin_clipped = true;
		}
		return false;
	}
	if (!e.touch_pin_kind)
		return false;
	e.touch_pin_shift = draw_shift;
	e.touch_pin_scale = draw_scale;
	e.touch_pin_font = draw_fontscale;
	e.touch_pin_alpha = draw_alpha;
	e.touch_pin_frame = touch_pin_framecount;
	return true;
}

void TouchSheet_drawPinned(float kind)
{
	for (entity e = touch_pin_head; e; e = e.touch_pin_next)
	{
		if (e.touch_pin_kind != kind || e.touch_pin_frame != touch_pin_framecount)
			continue;
		draw_shift = e.touch_pin_shift;
		draw_scale = e.touch_pin_scale;
		draw_fontscale = e.touch_pin_font;
		draw_alpha = e.touch_pin_alpha;
		e.draw(e);
	}
}

// The sheet's frame again, moved by dy (its own units) and cut to [y0, y1].
void TouchSheet_drawFrameBand(entity f, float dy, float y0, float y1, float line_y)
{
	if (y1 <= y0)
		return;
	draw_shift = touch_pin_root_shift;
	draw_scale = touch_pin_root_scale;
	draw_fontscale = touch_pin_root_font;
	draw_alpha = touch_pin_root_alpha;
	// A backing, so the band reads as solid over what scrolled under it.
	draw_SetClipRect(eY * y0, eX + eY * (y1 - y0));
	draw_Fill(eY * y0, eX + eY * (y1 - y0), '0.02 0.05 0.09', 0.85);
	draw_shift_y += dy * draw_scale.y;
	if (f.border)
	{
		entity b = f.border;
		draw_alpha *= b.Container_alpha;
		b.draw(b);
	}
	draw_ClearClip();
	// A hairline where content goes under it.
	if (line_y < 0)
		return;
	draw_shift = touch_pin_root_shift;
	draw_alpha = touch_pin_root_alpha;
	float line = TouchMenuScale_dpToUnits(1) / (conheight * f.Container_size.y);
	draw_Fill(eY * min(line_y, 1 - line), eX + eY * line, '0.55 0.8 1', 0.35);
}

void TouchSheet_drawPins()
{
	if (touch_pin_clipped)
	{
		draw_ClearClip();
		touch_pin_clipped = false;
	}
	entity f = touch_pin_sheet;
	if (!f || touch_pin_root_scale.y <= 0)
		return;
	vector s0 = draw_shift, s1 = draw_scale, s2 = draw_fontscale;
	float a0 = draw_alpha;
	float h = f.Container_size.y;
	// The title bar, where the sheet's top is when it is scrolled to the top.
	if (touch_pin_title > 0)
	{
		float y1 = (touch_pin_hdr_bottom - f.Container_origin.y) / h;
		TouchSheet_drawFrameBand(f, touch_pin_top, (f.touch_sheet_max - f.Container_origin.y) / h, y1, (touch_pin_top > 0.0001) ? y1 : -1);
	}
	// The action row's band, where the sheet's bottom is when scrolled to the end.
	if (touch_pin_ftr_top < 1 && touch_pin_bot < -0.0001)
	{
		float y0 = (touch_pin_ftr_top - f.Container_origin.y) / h;
		TouchSheet_drawFrameBand(f, touch_pin_bot, y0, (1 - TouchSheet_margin() - f.Container_origin.y) / h, y0);
	}
	TouchSheet_drawPinned(2);
	TouchSheet_drawPinned(1);
	draw_shift = s0;
	draw_scale = s1;
	draw_fontscale = s2;
	draw_alpha = a0;
}

// A pinned item, or something inside one.
bool TouchSheet_isPinned(entity leaf)
{
	for (entity e = leaf; e && e != main; e = e.parent)
		if (e.touch_pin_kind)
			return true;
	return false;
}

// A press on the title bar or the action row's band that is not on one of
// its buttons would reach the content scrolled under it.
bool TouchSheet_inPinBand(vector pos)
{
	if (!touch_pin_sheet)
		return false;
	entity f = touch_pin_sheet;
	if (pos.x < f.Container_origin.x || pos.x > f.Container_origin.x + f.Container_size.x)
		return false;
	return pos.y < touch_pin_hdr_bottom || pos.y > touch_pin_ftr_top;
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

// The press goes to the main window. A press that lands on a text field
// asks for the screen keyboard again (vid_touchscreen_showkeyboard 2, which
// the engine turns back into 1): Android's Back puts the keyboard away
// without the field losing focus, and tapping the field must bring it back.
void TouchSheet_press(vector pos)
{
	main.mousePress(main, pos);
	entity leaf = main;
	while (leaf.focusedChild)
		leaf = leaf.focusedChild;
	if (leaf.instanceOfInputBox && cvar("vid_touchscreen") && cvar("vid_touchscreen_showkeyboard"))
		cvar_set("vid_touchscreen_showkeyboard", "2");
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

// A list that has more rows than it shows scrolls itself under the finger.
// One whose rows all fit (the LAN list, a short map list) is part of the
// sheet: a drag on it scrolls the sheet, or a sheet whose list is taller
// than the screen (Multiplayer's) could not be moved to Join from the list.
bool TouchSheet_listScrolls(entity leaf)
{
	return leaf && leaf.instanceOfListBox && leaf.getTotalHeight(leaf) > 1.001;
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
		TouchSheet_press(pos);
		return;
	}
	entity leaf = TouchSheet_leafAt(pos);
	// On the pinned bands only their own buttons take a tap; the rest of
	// the band covers content scrolled under it. A drag scrolls either way.
	touch_sheet_swallow = (TouchSheet_inPinBand(pos) && !TouchSheet_isPinned(leaf));
	if (!touch_sheet_swallow && (TouchSheet_listScrolls(leaf)
		|| pos.y < f.Container_origin.y || pos.y > f.Container_origin.y + f.Container_size.y
		|| pos.x < f.Container_origin.x || pos.x > f.Container_origin.x + f.Container_size.x))
	{
		TouchSheet_press(pos);
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
		else if (touch_sheet_swallow)
		{
			touch_sheet_swallow = false;
			return;
		}
		else
		{
			// Sideways: the control under the finger gets the press after all.
			TouchSheet_press(touch_sheet_p0);
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
		if (touch_sheet_swallow)
		{
			touch_sheet_swallow = false;
			return; // on a pinned band, off its buttons: nothing under it
		}
		TouchSheet_press(touch_sheet_p0);
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
	if (TouchSheet_isPinned(leaf))
		return;
	vector r = TouchSheet_rectInMain(leaf);
	float m = TouchSheet_margin() * 4;
	// Between the pinned title bar and action row, when there are those.
	float lo = m, hi = 1 - m;
	if (touch_pin_sheet == f)
	{
		lo = max(lo, f.touch_sheet_max + touch_pin_title * f.Container_size.y + m);
		hi = min(hi, touch_pin_ftr_top - m);
	}
	if (r.z > hi - lo)
		return;
	float y = f.ModalController_initialOrigin.y;
	if (r.y < lo)
		y += lo - r.y;
	else if (r.y + r.z > hi)
		y -= r.y + r.z - hi;
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

	TouchSheet_updatePins(f);
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
	TouchSheet_drawPins();
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
	if (TouchSheet_listScrolls(leaf))
		return false;
	// Three rows a notch.
	float d = TouchMenuScale_dpToUnits(3 * (TOUCH_MENU_ROW_DP + TOUCH_MENU_GAP_DP) * TouchMenuScale_size()) / conheight;
	TouchSheet_setScroll(f, f.ModalController_initialOrigin.y + ((key == K_MWHEELUP) ? d : -d));
	touch_sheet_bar = 1;
	touch_sheet_resize = true;
	return true;
}
