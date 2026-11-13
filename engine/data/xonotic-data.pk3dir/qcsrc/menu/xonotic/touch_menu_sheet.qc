// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_sheet.qh"

#include "../menu.qh"
#include "../item/modalcontroller.qh"
#include "../item/inputbox.qh"
#include "touch_menu_scale.qh"
#include "dialog_touch_launcher.qh"
#include "touch_menu_reflow.qh"
#include "../item/borderimage.qh"
#include "../item/checkbox.qh"
#include "../item/listbox.qh"

bool touch_sheet_resize;     // children need their absolute positions again
entity touch_sheet_focus;    // the focused control last frame
bool touch_sheet_swallow;    // the held press landed on a pinned band, off its buttons
float touch_keys_frac;       // top of the engine's keys, fraction of the screen; 0 none
float touch_keys_top = 1;    // the same in the main window's units; 1 none
bool touch_sheet_keyspress;  // the press is the engine's keys', not the menu's
entity touch_kb_field;       // the field last moved above the keys
float touch_kb_fieldtop;
.float touch_sheet_kbshift;  // the sheet left its range to show a field above the keys

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

void TouchScroll_drawBars();
bool TouchScroll_is(entity e);

// List boxes drawn this frame, for their scroll indicators.
const int TOUCH_BAR_LISTS = 16;
entity touch_bar_list[TOUCH_BAR_LISTS];
int touch_bar_n;
void TouchScroll_noteList(entity e)
{
	if (touch_menu_on && touch_bar_n < TOUCH_BAR_LISTS)
	{
		touch_bar_list[touch_bar_n] = e;
		++touch_bar_n;
	}
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
	for (entity t = f.firstChild; t; t = t.nextSibling)
		if (t.touch_sheet_titlebar && t.Container_alpha >= 0.003)
			TouchSheet_pin(t, 1);

	// With the engine's keys up the action row would only be under them.
	entity c = (touch_keys_top < 1) ? NULL : TouchSheet_footerHost(f);
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
	// List boxes, and any item that scrolls itself (touch_scroll_get set),
	// get an indicator; sheets have their own.
	if (e.instanceOfListBox || (TouchScroll_is(e) && !e.instanceOfDialog))
		TouchScroll_noteList(e);
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
	if (touch_pin_ftr_top < 1)
	{
		float y0 = (touch_pin_ftr_top - f.Container_origin.y) / h;
		TouchSheet_drawFrameBand(f, touch_pin_bot, y0, (1 - TouchSheet_margin() - f.Container_origin.y) / h, (touch_pin_bot < -0.0001) ? y0 : -1);
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

// With the engine's keys up, a sheet scrolls on until its end is above them.
float TouchSheet_keysExtra()
{
	return (touch_keys_top < 1) ? max(0, 1 - TouchSheet_margin() - touch_keys_top) : 0;
}

void TouchSheet_resized(entity e)
{
	float h = e.intendedHeight;
	e.ModalController_initialSize_y = h;
	e.ModalController_initialOrigin_y = 0.5 * (1 - h);
	if (e.ModalController_factor >= 1)
	{
		e.Container_size_y = h;
		e.Container_origin_y = 0.5 * (1 - h);
	}
	e.touch_sheet_h = 0;
	e.touch_sheet_was_shown = false; // starts at the top
	touch_sheet_resize = true;
}

void TouchSheet_setScroll(entity e, float y)
{
	y = bound(e.touch_sheet_min - TouchSheet_keysExtra(), y, e.touch_sheet_max);
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
	{
		cvar_set("vid_touchscreen_showkeyboard", "2");
		cvar_set("_touch_menu_keys_ask", "1"); // the engine's own keys too, after DONE (touch_ui.c)
	}
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

// ---- Scrollers: what a drag moves --------------------------------------
//
// The reusable part (touch_menu_sheet.qh): any entity in the menu tree is a
// scroller when it sets touch_scroll_get/_max/_set/_unit. Sheets (root
// dialogs) and list boxes get theirs here; anything else can set its own.

bool TouchScroll_is(entity e)
{
	return e && e.touch_scroll_get != func_null;
}

float TouchScroll_room(entity e)
{
	return TouchScroll_is(e) ? e.touch_scroll_max(e) : 0;
}

// Can e move further in dir (+1: towards its end, as a finger moving up)?
bool TouchScroll_canMove(entity e, float dir)
{
	if (!TouchScroll_is(e))
		return false;
	float o = e.touch_scroll_get(e), m = e.touch_scroll_max(e);
	if (m <= 0.0001)
		return false;
	return (dir > 0) ? (o < m - 0.0001) : (o > 0.0001);
}

// List boxes: scrollPos in list heights.
float TouchScroll_listGet(entity me) { return me.scrollPos; }
float TouchScroll_listMax(entity me) { return max(0, me.getTotalHeight(me) - 1); }
void TouchScroll_listSet(entity me, float o)
{
	o = bound(0, o, max(0, me.getTotalHeight(me) - 1));
	me.scrollPos = me.scrollPosTarget = o;
	me.focusedItem = -1; // no row stays lit under a finger that scrolled
}
float TouchScroll_listUnit(entity me) { return TouchSheet_rectInMain(me).z; }

// Sheets: how far the dialog is moved up from its top position.
float TouchScroll_sheetGet(entity me) { return me.touch_sheet_max - me.ModalController_initialOrigin.y; }
float TouchScroll_sheetMax(entity me)
{
	if (!TouchSheet_scrolls(me) && !(touch_keys_top < 1))
		return 0;
	return me.touch_sheet_max - (me.touch_sheet_min - TouchSheet_keysExtra());
}
void TouchScroll_sheetSet(entity me, float o) { TouchSheet_setScroll(me, me.touch_sheet_max - o); }
float TouchScroll_sheetUnit(entity me) { return 1; }
void TouchScroll_sheetEnd(entity me) { touch_sheet_resize = true; }

void TouchScroll_adopt(entity e)
{
	if (!e || TouchScroll_is(e))
		return;
	if (e.instanceOfListBox)
	{
		e.touch_scroll_get = TouchScroll_listGet;
		e.touch_scroll_max = TouchScroll_listMax;
		e.touch_scroll_set = TouchScroll_listSet;
		e.touch_scroll_unit = TouchScroll_listUnit;
	}
	else if (e.parent == main && e.instanceOfDialog && e.touch_sheet_h > 0)
	{
		e.touch_scroll_get = TouchScroll_sheetGet;
		e.touch_scroll_max = TouchScroll_sheetMax;
		e.touch_scroll_set = TouchScroll_sheetSet;
		e.touch_scroll_unit = TouchScroll_sheetUnit;
		e.touch_scroll_end = TouchScroll_sheetEnd;
	}
}

// The innermost scroller at or above leaf that can still move in dir; with
// dir 0, the innermost one with any room at all.
entity TouchScroll_find(entity leaf, float dir)
{
	for (entity e = leaf; e && e != main; e = e.parent)
	{
		TouchScroll_adopt(e);
		if (dir ? TouchScroll_canMove(e, dir) : (TouchScroll_room(e) > 0.0001))
			return e;
	}
	return NULL;
}

void TouchScroll_ended(entity e)
{
	if (e && e.touch_scroll_end)
		e.touch_scroll_end(e);
}

// The scroll indicator: a thin rounded thumb inside the box o..o+s (main
// window units), at its right edge, for content `total` long of which
// `view` shows, scrolled by `pos`; alpha fades it.
void TouchScroll_drawBar(vector o, vector s, float view, float total, float pos, float alpha)
{
	if (alpha < 0.02 || total <= view + 0.0001 || s.y <= 0)
		return;
	float w = TouchMenuScale_dpToUnits(TOUCH_SCROLL_BAR_DP) / conwidth;
	float inset = TouchMenuScale_dpToUnits(TOUCH_SCROLL_INSET_DP);
	float ix = inset / conwidth, iy = inset / conheight;
	float trackh = s.y - 2 * iy;
	if (trackh <= 0)
		return;
	float th = max(TouchMenuScale_dpToUnits(24) / conheight, trackh * view / total);
	th = min(th, trackh);
	float t = bound(0, pos / (total - view), 1);
	vector to = eX * (o.x + s.x - ix - w) + eY * (o.y + iy + t * (trackh - th));
	float cap = w * conwidth / conheight; // a circle's height for its width
	vector c = '0.85 0.92 1';
	float a = 0.55 * alpha;
	if (th > 2 * cap)
		draw_Fill(to + eY * (0.5 * cap), eX * w + eY * (th - cap), c, a);
	draw_Picture(to, "/gfx/touch/disc", eX * w + eY * cap, c, a);
	draw_Picture(to + eY * (th - cap), "/gfx/touch/disc", eX * w + eY * cap, c, a);
}

// ---- The gesture: tap, drag, fling -------------------------------------

bool touch_g_pending;        // a press held back until it is a tap or a drag
bool touch_g_stopper;        // the press stopped a fling: a tap then does nothing
entity touch_g_leaf;         // what the press landed on
entity touch_g_scroller;     // what the drag moves
entity touch_g_fling;
vector touch_g_p0;           // where the press landed (main units)
float touch_g_o0;            // the scroller's offset then
vector touch_g_last;
float touch_g_lasttime;
float touch_g_vel;           // main-window heights per second, + = content moves up

bool TouchSheet_overKeys(vector pos)
{
	return touch_menu_on && touch_keys_top < 1 && pos.y >= touch_keys_top;
}

void TouchSheet_mousePress(vector pos)
{
	// The engine's keys are over the menu there: the press is theirs.
	touch_sheet_keyspress = TouchSheet_overKeys(pos);
	if (touch_sheet_keyspress)
		return;
	TouchSheet_dropSavedFocus(pos);
	touch_g_pending = false;
	touch_g_scroller = NULL;
	touch_g_stopper = false;
	if (touch_g_fling)
	{
		// A finger stops a fling, and that is all it does.
		entity fl = touch_g_fling;
		touch_g_fling = NULL;
		TouchScroll_ended(fl);
		touch_g_stopper = true;
	}
	if (!touch_menu_on)
	{
		TouchSheet_press(pos);
		return;
	}
	entity leaf = TouchSheet_leafAt(pos);
	// On the pinned bands only their own buttons take a tap; the rest of
	// the band covers content scrolled under it. A drag scrolls either way.
	touch_sheet_swallow = (TouchSheet_inPinBand(pos) && !TouchSheet_isPinned(leaf));
	// Nothing under the finger can scroll: the press goes straight on.
	if (!touch_sheet_swallow && !touch_g_stopper && !TouchScroll_find(leaf, 0))
	{
		TouchSheet_press(pos);
		return;
	}
	touch_g_pending = true;
	// On a pinned band the content under it is not what the finger holds:
	// the sheet is.
	touch_g_leaf = touch_sheet_swallow ? TouchSheet_front() : leaf;
	touch_g_p0 = pos;
}

void TouchSheet_mouseDrag(vector pos)
{
	if (touch_sheet_keyspress)
		return;
	if (touch_g_pending)
	{
		vector d = pos - touch_g_p0;
		float dx = fabs(d.x) * conwidth * touch_menu_unit_dp;
		float dy = fabs(d.y) * conheight * touch_menu_unit_dp;
		if (max(dx, dy) < TOUCH_SHEET_SLOP_DP)
			return;
		touch_g_pending = false;
		if (dy >= dx)
		{
			// Up or down: the innermost thing that can still go that way.
			float dir = (d.y < 0) ? 1 : -1;
			entity s = TouchScroll_find(touch_g_leaf, dir);
			touch_g_scroller = s;
			if (s)
				touch_g_o0 = s.touch_scroll_get(s);
			touch_g_p0 = pos; // no jump by the slop
			touch_g_last = pos;
			touch_g_lasttime = time;
			touch_g_vel = 0;
			return;
		}
		if (touch_sheet_swallow || touch_g_stopper)
			return;
		// Sideways: the control under the finger (a slider) gets the press.
		TouchSheet_press(touch_g_p0);
		main.mouseDrag(main, pos);
		return;
	}
	if (touch_g_scroller || touch_sheet_swallow || touch_g_stopper)
	{
		entity e = touch_g_scroller;
		float dt = time - touch_g_lasttime;
		if (dt > 0.001)
		{
			float v = (touch_g_last.y - pos.y) / dt;
			touch_g_vel = (dt > 0.1) ? v : 0.6 * v + 0.4 * touch_g_vel;
			touch_g_last = pos;
			touch_g_lasttime = time;
		}
		if (!e)
			return;
		float want = touch_g_o0 + (touch_g_p0.y - pos.y) / max(0.0001, e.touch_scroll_unit(e));
		e.touch_scroll_set(e, want);
		e.touch_scroll_bar = 1;
		// At its end: the rest of the drag goes to the next one out that can.
		float got = e.touch_scroll_get(e);
		if (fabs(got - want) > 0.0001)
		{
			float dir = (want > got) ? 1 : -1;
			entity p = TouchScroll_find(e.parent, dir);
			if (p)
			{
				TouchScroll_ended(e);
				touch_g_scroller = p;
				touch_g_o0 = p.touch_scroll_get(p);
				touch_g_p0 = pos;
			}
		}
		return;
	}
	main.mouseDrag(main, pos);
}

void TouchSheet_mouseRelease(vector pos)
{
	if (touch_sheet_keyspress)
	{
		touch_sheet_keyspress = false;
		return;
	}
	if (touch_g_pending)
	{
		// A tap: the control gets its press and release together.
		touch_g_pending = false;
		if (touch_sheet_swallow || touch_g_stopper)
		{
			touch_sheet_swallow = touch_g_stopper = false;
			return; // a pinned band off its buttons, or the tap that stopped a fling
		}
		TouchSheet_press(touch_g_p0);
		main.mouseRelease(main, touch_g_p0);
		return;
	}
	if (touch_g_scroller || touch_sheet_swallow || touch_g_stopper)
	{
		entity e = touch_g_scroller;
		touch_g_scroller = NULL;
		touch_sheet_swallow = touch_g_stopper = false;
		if (!e)
			return;
		if (time - touch_g_lasttime > 0.12)
			touch_g_vel = 0; // the finger stopped before it lifted
		if (fabs(touch_g_vel) > 0.2)
			touch_g_fling = e;
		else
			TouchScroll_ended(e);
		return;
	}
	main.mouseRelease(main, pos);
}

// Every frame: a fling slows down and stops at the end.
void TouchScroll_flingFrame()
{
	entity e = touch_g_fling;
	if (!e)
		return;
	float u = max(0.0001, e.touch_scroll_unit(e));
	float want = e.touch_scroll_get(e) + touch_g_vel * frametime / u;
	e.touch_scroll_set(e, want);
	e.touch_scroll_bar = 1;
	touch_g_vel *= pow(0.03, frametime);
	float got = e.touch_scroll_get(e);
	if (fabs(touch_g_vel) < 0.05 || fabs(got - want) > 0.0001 || (e.instanceOfDialog && e.ModalController_state != 1))
	{
		touch_g_fling = NULL;
		TouchScroll_ended(e);
	}
}

bool TouchSheet_busy()
{
	return touch_g_scroller != NULL || touch_g_fling != NULL;
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
	hi = min(hi, touch_keys_top - m);
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

// The engine's keys (touch_ui.c) are up for a menu text field: move the
// sheet so the field is above them, once each time the field or the keys
// change (a drag still scrolls it after), and back when they go.
void TouchSheet_keysFollowField(entity f)
{
	if (!f)
		return;
	entity leaf = f;
	while (leaf.focusedChild)
		leaf = leaf.focusedChild;
	if (touch_keys_top >= 1 || !leaf.instanceOfInputBox)
	{
		touch_kb_field = NULL;
		if (f.touch_sheet_kbshift)
		{
			f.touch_sheet_kbshift = 0;
			float y0 = bound(f.touch_sheet_min, f.ModalController_initialOrigin.y, f.touch_sheet_max);
			f.ModalController_initialOrigin_y = y0;
			if (f.ModalController_factor >= 1)
				f.Container_origin_y = y0;
			touch_sheet_resize = true;
		}
		return;
	}
	if (leaf == touch_kb_field && touch_keys_top == touch_kb_fieldtop)
		return;
	touch_kb_field = leaf;
	touch_kb_fieldtop = touch_keys_top;
	vector r = TouchSheet_rectInMain(leaf);
	float m = TouchSheet_margin() * 2;
	float over = r.y + r.z - (touch_keys_top - m);
	if (over <= 0)
		return;
	float y = f.ModalController_initialOrigin.y - over;
	f.touch_sheet_kbshift = 1;
	f.ModalController_initialOrigin_y = y;
	if (f.ModalController_factor >= 1)
		f.Container_origin_y = y;
	touch_sheet_resize = true;
}

void TouchSheet_preDraw()
{
	if (!touch_menu_on)
		return;
	// Where the engine's keys start, in the main window's units.
	touch_keys_frac = cvar("_touch_menu_keys_top");
	touch_keys_top = 1;
	if (touch_keys_frac > 0 && touch_keys_frac < 1)
		touch_keys_top = globalToBox(eY * (touch_keys_frac * realconheight), draw_shift, draw_scale).y;
	TouchSettings_Frame();
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

	TouchScroll_flingFrame();
	touch_bar_n = 0;
	if (touch_sheet_resize && !TouchSheet_busy())
		TouchSheet_doResize();

	TouchSheet_keysFollowField(f);
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
	TouchScroll_drawBars();
}

// Where an item is in the main window: origin, and its size in touch_box_size.
vector touch_box_size;
vector TouchSheet_boxInMain(entity it)
{
	vector o = '0 0 0', s = '1 1 0';
	for (entity e = it; e && e != main; e = e.parent)
	{
		o = boxToGlobal(o, e.Container_origin, e.Container_size);
		s = boxToGlobalSize(s, e.Container_size);
	}
	touch_box_size = s;
	return o;
}

// Lit while it moves, by a finger or anything else; fades out after.
float TouchScroll_emphasis(entity e)
{
	float off = e.touch_scroll_get(e);
	if (fabs(off - e.touch_scroll_lastoff) > 0.00001 || e == touch_g_scroller || e == touch_g_fling)
		e.touch_scroll_bar = max(e.touch_scroll_bar, 1);
	e.touch_scroll_lastoff = off;
	if (e.touch_scroll_bar > 1)
		e.touch_scroll_bar = 1.6; // hold a moment at full, then fade
	e.touch_scroll_bar = max(0, e.touch_scroll_bar - frametime * 1.6);
	return min(1, e.touch_scroll_bar);
}

void TouchScroll_drawBars()
{
	float lo = 0, hi = 1;
	entity f = TouchSheet_front();
	if (f && touch_pin_sheet == f)
	{
		lo = touch_pin_hdr_bottom;
		hi = touch_pin_ftr_top;
	}
	// The sheet's, inside its card: right of the content, left of the frame.
	TouchScroll_adopt(f);
	if (TouchScroll_is(f) && TouchSheet_scrolls(f) && f.ModalController_factor >= 1)
	{
		float a = TouchScroll_emphasis(f);
		float m = TouchSheet_margin();
		float top = max(lo, max(f.Container_origin.y, m));
		float bot = min(hi, min(f.Container_origin.y + f.Container_size.y, 1 - m));
		float right = f.marginRight * 0.6 / conwidth;
		float view = bot - top;
		TouchScroll_drawBar(eX * f.Container_origin.x + eY * top, eX * (f.Container_size.x - right) + eY * view,
			view, view + TouchScroll_room(f), TouchScroll_sheetGet(f), a);
	}
	// Each list's, inside its box.
	for (int i = 0; i < touch_bar_n; ++i)
	{
		entity e = touch_bar_list[i];
		TouchScroll_adopt(e);
		if (TouchScroll_room(e) <= 0.001)
			continue;
		float la = TouchScroll_emphasis(e);
		vector o = TouchSheet_boxInMain(e);
		vector s = touch_box_size;
		float t0 = max(o.y, lo), t1 = min(o.y + s.y, hi);
		if (t1 - t0 < s.y * 0.2)
			continue;
		// The thumb maps the whole list onto the part of the box in view.
		TouchScroll_drawBar(eX * o.x + eY * t0, eX * s.x + eY * (t1 - t0), 1, 1 + TouchScroll_room(e), e.touch_scroll_get(e), la);
	}
}

bool TouchSheet_wheel(float key)
{
	// Back (Escape) with the engine's keys up puts them away first, as Back
	// does with a system keyboard; the next Back closes the sheet.
	if (key == K_ESCAPE && touch_menu_on && touch_keys_frac > 0)
	{
		cvar_set("_touch_menu_keys_ask", "-1");
		touch_keys_frac = 0;
		return true;
	}
	// Settings showing a category: back to its list.
	if (key == K_ESCAPE && touch_menu_on && TouchSettings_Back())
		return true;
	if (!touch_menu_on || (key != K_MWHEELUP && key != K_MWHEELDOWN))
		return false;
	entity f = TouchSheet_front();
	if (!TouchSheet_scrolls(f))
		return false;
	entity leaf = TouchSheet_leafAt(menuMousePos);
	if (leaf && leaf.instanceOfListBox && leaf.getTotalHeight(leaf) > 1.001)
		return false;
	// Three rows a notch.
	float d = TouchMenuScale_dpToUnits(3 * (TOUCH_MENU_ROW_DP + TOUCH_MENU_GAP_DP) * TouchMenuScale_size()) / conheight;
	TouchSheet_setScroll(f, f.ModalController_initialOrigin.y + ((key == K_MWHEELUP) ? d : -d));
	f.touch_scroll_bar = 1;
	touch_sheet_resize = true;
	return true;
}
