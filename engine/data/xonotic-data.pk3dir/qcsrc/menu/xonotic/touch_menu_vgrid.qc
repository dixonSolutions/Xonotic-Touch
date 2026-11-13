// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_vgrid.qh"

#include "touch_menu_scale.qh"
#include "touch_menu_sheet.qh"
#include "textlabel.qh"
#include "dialog.qh"
#include "../item/modalcontroller.qh"
#include <common/mapinfo.qh>

// ---- The virtual grid ---------------------------------------------------

int TouchGrid_count(entity me)
{
	return (me.vg_count != func_null) ? me.vg_count(me) : 0;
}

int TouchGrid_columns(entity me)
{
	if (me.vg_rowDp > 0 || me.vg_abs.x <= 0)
		return 1;
	return max(1, floor(me.vg_abs.x / TouchMenuScale_dpToUnits(me.vg_cellDp * TouchMenuScale_size())));
}

// A cell's size in the grid's own units (its width and height fractions).
vector TouchGrid_cell(entity me)
{
	if (me.vg_abs.y <= 0)
		return '1 1 0';
	int cols = TouchGrid_columns(me);
	float h = (me.vg_rowDp > 0)
		? TouchMenuScale_dpToUnits(me.vg_rowDp * TouchMenuScale_size())
		: me.vg_abs.x / cols * me.vg_aspect;
	return eX * (1 / cols) + eY * (h / me.vg_abs.y);
}

float TouchGrid_total(entity me)
{
	int cols = TouchGrid_columns(me);
	return ceil(TouchGrid_count(me) / cols) * TouchGrid_cell(me).y;
}

// As a scroller of the gesture layer: offset in own heights.
float TouchGrid_get(entity me) { return me.vg_scroll; }
float TouchGrid_max(entity me) { return max(0, TouchGrid_total(me) - 1); }
void TouchGrid_set(entity me, float o) { me.vg_scroll = bound(0, o, TouchGrid_max(me)); }
float TouchGrid_unit(entity me) { return TouchSheet_rectInMain(me).z; }

void GodnoticVirtualGrid_resizeNotify(entity me, vector relOrigin, vector relSize, vector absOrigin, vector absSize)
{
	SUPER(GodnoticVirtualGrid).resizeNotify(me, relOrigin, relSize, absOrigin, absSize);
	me.vg_abs = absSize;
	me.touch_scroll_get = TouchGrid_get;
	me.touch_scroll_max = TouchGrid_max;
	me.touch_scroll_set = TouchGrid_set;
	me.touch_scroll_unit = TouchGrid_unit;
	me.vg_scroll = bound(0, me.vg_scroll, TouchGrid_max(me));
}

int TouchGrid_cellAt(entity me, vector pos)
{
	if (pos.x < 0 || pos.x >= 1 || pos.y < 0 || pos.y >= 1)
		return -1;
	vector c = TouchGrid_cell(me);
	int col = floor(pos.x / c.x), row = floor((pos.y + me.vg_scroll) / c.y);
	int i = row * TouchGrid_columns(me) + col;
	return (i < TouchGrid_count(me)) ? i : -1;
}

void GodnoticVirtualGrid_draw(entity me)
{
	int n = TouchGrid_count(me);
	int cols = TouchGrid_columns(me);
	vector c = TouchGrid_cell(me);
	// The rows in view, and one more each side: nothing else is touched.
	int r0 = max(0, floor(me.vg_scroll / c.y) - 1);
	int r1 = floor((me.vg_scroll + 1) / c.y) + 1;
	draw_SetClip();
	for (int r = r0; r <= r1; ++r)
		for (int k = 0; k < cols; ++k)
		{
			int i = r * cols + k;
			if (i >= n)
				break;
			vector o = eX * (k * c.x) + eY * (r * c.y - me.vg_scroll);
			if (me.vg_drawCell != func_null)
				me.vg_drawCell(me, i, o, c, (i == me.vg_focused && me.focused));
		}
	draw_ClearClip();
	if (me.vg_window != func_null)
		me.vg_window(me, r0 * cols, min(n, (r1 + 1) * cols) - 1);
	SUPER(GodnoticVirtualGrid).draw(me);
}

bool GodnoticVirtualGrid_mousePress(entity me, vector pos)
{
	me.vg_pressed = TouchGrid_cellAt(me, pos);
	return true;
}

float GodnoticVirtualGrid_mouseRelease(entity me, vector pos)
{
	int i = TouchGrid_cellAt(me, pos);
	if (i >= 0 && i == me.vg_pressed && me.vg_click != func_null)
		me.vg_click(me, i);
	me.vg_pressed = -1;
	return 1;
}

float GodnoticVirtualGrid_mouseMove(entity me, vector pos)
{
	me.vg_focused = TouchGrid_cellAt(me, pos);
	return 1;
}

float GodnoticVirtualGrid_keyDown(entity me, float key, float ascii, float shift)
{
	int n = TouchGrid_count(me), cols = TouchGrid_columns(me);
	int f = max(0, me.vg_focused);
	switch (key)
	{
		case K_LEFTARROW: f -= 1; break;
		case K_RIGHTARROW: f += 1; break;
		case K_UPARROW: f -= cols; break;
		case K_DOWNARROW: f += cols; break;
		case K_ENTER:
		case K_KP_ENTER:
			if (me.vg_focused >= 0 && me.vg_click != func_null)
				me.vg_click(me, me.vg_focused);
			return 1;
		default: return 0;
	}
	me.vg_focused = bound(0, f, n - 1);
	// Keep it in view.
	vector c = TouchGrid_cell(me);
	float y = floor(me.vg_focused / cols) * c.y;
	if (y < me.vg_scroll)
		TouchGrid_set(me, y);
	else if (y + c.y > me.vg_scroll + 1)
		TouchGrid_set(me, y + c.y - 1);
	return 1;
}

// ---- The map picker ------------------------------------------------------

const int TOUCH_MP_MAX = 512;
string tm_mp_bsp[TOUCH_MP_MAX];
string tm_mp_title[TOUCH_MP_MAX];
string tm_mp_modes[TOUCH_MP_MAX];
string tm_mp_pic[TOUCH_MP_MAX];      // the picture loaded for it; "" none
int tm_mp_n;
int tm_mp_sel = -1;
entity tm_mp_dialog, tm_mp_preview, tm_mp_grid;
var void(string bsp) tm_mp_onPick;

// Title and modes, read once per map, when it first shows.
void TouchMapPick_info(int idx)
{
	if (tm_mp_title[idx] != "")
		return;
	string t = tm_mp_bsp[idx], modes = "";
	if (MapInfo_Get_ByName(tm_mp_bsp[idx], 0, NULL))
	{
		t = strdecolorize(MapInfo_Map_title);
		int k = 0;
		FOREACH(Gametypes, it.gametype_flags & MapInfo_Map_supportedGametypes, {
			if (k < 6)
				modes = strcat(modes, ((modes != "") ? " " : ""), MapInfo_Type_ToString(it));
			++k;
		});
		MapInfo_ClearTemps();
	}
	strcpy(tm_mp_title[idx], (t != "") ? t : tm_mp_bsp[idx]);
	strcpy(tm_mp_modes[idx], modes);
}

string TouchMapPick_picture(int i)
{
	if (tm_mp_pic[i] != "")
		return tm_mp_pic[i];
	string p = strcat("/maps/", tm_mp_bsp[i]);
	if (draw_PictureSize(p) == '0 0 0')
	{
		p = strcat("/levelshots/", tm_mp_bsp[i]);
		if (draw_PictureSize(p) == '0 0 0')
			p = "nopreview_map";
	}
	strcpy(tm_mp_pic[i], p);
	return tm_mp_pic[i];
}

// Pictures well away from the view are freed; the near ones load as drawn.
void TouchMapPick_window(entity me, int first, int last)
{
	int keep = 4 * TouchGrid_columns(me);
	for (int i = 0; i < tm_mp_n; ++i)
		if (tm_mp_pic[i] != "" && (i < first - keep || i > last + keep))
		{
			if (tm_mp_pic[i] != "nopreview_map")
				freepic(substring(tm_mp_pic[i], 1, -1));
			strfree(tm_mp_pic[i]);
			tm_mp_pic[i] = "";
		}
}

int TouchMapPick_count(entity me) { return tm_mp_n; }

void TouchMapPick_drawCell(entity me, int i, vector o, vector s, bool focused)
{
	TouchMapPick_info(i);
	vector abs = me.vg_abs;
	float padx = 0.04 * s.x, pady = padx * abs.x / abs.y;
	vector fs = eX * (SKINFONTSIZE_NORMAL / abs.x) + eY * (SKINFONTSIZE_NORMAL / abs.y);
	vector fs2 = fs * 0.8;
	bool list = (me.vg_rowDp > 0);
	vector po, ps;
	if (list)
	{
		// A thumbnail on the left, the text beside it.
		ps = eY * (s.y - 2 * pady) + eX * ((s.y - 2 * pady) * abs.y / abs.x);
		po = o + eX * padx + eY * pady;
	}
	else
	{
		float side = min(s.x - 2 * padx, (s.y - 2 * pady - 2.4 * fs.y) * abs.y / abs.x);
		ps = eX * side + eY * (side * abs.x / abs.y);
		po = o + eX * (0.5 * (s.x - side)) + eY * pady;
	}
	if (i == tm_mp_sel)
		draw_Fill(o + eX * (0.5 * padx) + eY * (0.5 * pady), s - eX * padx - eY * pady, '0.3 0.6 1', 0.35);
	else if (focused)
		draw_Fill(o + eX * (0.5 * padx) + eY * (0.5 * pady), s - eX * padx - eY * pady, '1 1 1', 0.08);
	draw_Picture(po, TouchMapPick_picture(i), ps, '1 1 1', 1);
	vector to = list ? (po + eX * (ps.x + padx) + eY * (0.5 * ps.y - fs.y)) : (po + eY * (ps.y + 0.3 * fs.y));
	float tw = list ? (o.x + s.x - to.x - padx) : (s.x - 2 * padx);
	if (!list)
		to.x = o.x + padx;
	draw_Text(to, draw_TextShortenToWidth(tm_mp_title[i], tw, false, fs), fs, '1 1 1', 1, false);
	draw_Text(to + eY * (fs.y * 1.15), draw_TextShortenToWidth(tm_mp_modes[i], tw, false, fs2), fs2, '0.6 0.75 0.9', 1, false);
}

void TouchMapPick_click(entity me, int i)
{
	tm_mp_sel = i;
	m_play_click_sound(MENU_SOUND_OPEN);
	if (tm_mp_preview)
		DialogOpenButton_Click_withCoords(NULL, tm_mp_preview, menuMousePos - '0.05 0.05 0', '0.1 0.1 0');
}

void TouchMapPick_toggleList(entity btn, entity me)
{
	if (!tm_mp_grid)
		return;
	tm_mp_grid.vg_rowDp = (tm_mp_grid.vg_rowDp > 0) ? 0 : 72;
	tm_mp_grid.vg_scroll = 0;
	btn.setText(btn, (tm_mp_grid.vg_rowDp > 0) ? _("Grid") : _("List"));
}

void GodnoticMapPickerDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 1, 4.6, e = makeXonoticTextLabel(0, _("Tap a map to see it and play it.")));
		me.TD(me, 1, 1.4, e = makeGodnoticButton(_("List"), '0 0 0'));
			e.onClick = TouchMapPick_toggleList;
			e.onClickEntity = me;
	me.TR(me);
		me.TD(me, me.rows - 2.6, me.columns, e = tm_mp_grid = NEW(GodnoticVirtualGrid));
			e.vg_count = TouchMapPick_count;
			e.vg_drawCell = TouchMapPick_drawCell;
			e.vg_click = TouchMapPick_click;
			e.vg_window = TouchMapPick_window;
	me.gotoRC(me, me.rows - 1.3, 2);
		me.TD(me, 1.3, 2, e = makeGodnoticButton(_("Cancel"), '0 0 0'));
			e.onClick = Dialog_Close;
			e.onClickEntity = me;
}

// ---- The preview page --------------------------------------------------

void GodnoticMapPreviewItem_draw(entity me)
{
	if (tm_mp_sel < 0 || tm_mp_sel >= tm_mp_n)
		return;
	int i = tm_mp_sel;
	TouchMapPick_info(i);
	if (me.mpRole == 0)
	{
		// The picture, square, as large as the box allows.
		float side = min(me.size.x, me.size.y);
		vector s = eX * (side / me.size.x) + eY * (side / me.size.y);
		draw_Picture(eX * (0.5 * (1 - s.x)), TouchMapPick_picture(i), s, '1 1 1', 1);
		return;
	}
	vector fs = eX * (SKINFONTSIZE_TITLE / me.size.x) + eY * (SKINFONTSIZE_TITLE / me.size.y);
	vector fn = eX * (SKINFONTSIZE_NORMAL / me.size.x) + eY * (SKINFONTSIZE_NORMAL / me.size.y);
	float y = 0;
	draw_Text(eY * y, draw_TextShortenToWidth(tm_mp_title[i], 1, false, fs), fs, '1 1 1', 1, false);
	y += fs.y * 1.4;
	string author = "", desc = "";
	if (MapInfo_Get_ByName(tm_mp_bsp[i], 0, NULL))
	{
		author = strdecolorize(MapInfo_Map_author);
		desc = MapInfo_Map_description;
	}
	if (author != "")
	{
		draw_Text(eY * y, draw_TextShortenToWidth(sprintf(_("by %s"), author), 1, false, fn), fn, '0.7 0.8 0.9', 1, false);
		y += fn.y * 1.3;
	}
	draw_Text(eY * y, draw_TextShortenToWidth(sprintf(_("Modes: %s"), tm_mp_modes[i]), 1, false, fn), fn, '0.6 0.75 0.9', 1, false);
	y += fn.y * 1.6;
	// The description, wrapped.
	getWrappedLine_remaining = desc;
	while (getWrappedLine_remaining && y + fn.y <= 1)
	{
		string l = getWrappedLine(1, fn, draw_TextWidth_WithoutColors);
		draw_Text(eY * y, l, fn, '0.9 0.9 0.9', 1, false);
		y += fn.y * 1.15;
	}
	getWrappedLine_remaining = string_null;
	MapInfo_ClearTemps();
}

void TouchMapPick_use(entity btn, entity me)
{
	if (tm_mp_sel >= 0 && tm_mp_sel < tm_mp_n && tm_mp_onPick != func_null)
		tm_mp_onPick(tm_mp_bsp[tm_mp_sel]);
	if (tm_mp_preview)
		tm_mp_preview.close(tm_mp_preview);
	if (tm_mp_dialog)
		tm_mp_dialog.close(tm_mp_dialog);
}

void GodnoticMapPreviewDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, me.rows - 1.6, 2.6, e = NEW(GodnoticMapPreviewItem));
			e.mpRole = 0;
		me.TD(me, me.rows - 1.6, 3.4, e = NEW(GodnoticMapPreviewItem));
			e.mpRole = 1;
	me.gotoRC(me, me.rows - 1.3, 0);
		me.TD(me, 1.3, 2, e = makeGodnoticButton(_("< Back"), '0 0 0'));
			e.onClick = Dialog_Close;
			e.onClickEntity = me;
		me.TDempty(me, 1);
		me.TD(me, 1.3, 3, e = makeGodnoticButton(_("Use this map"), '0.55 1 0.55'));
			e.onClick = TouchMapPick_use;
			e.onClickEntity = me;
}

void TouchMapPick_ConfigureDialogs(entity mainwindow)
{
	entity i = tm_mp_dialog = NEW(GodnoticMapPickerDialog);
	i.configureDialog(i);
	mainwindow.addItemCentered(mainwindow, i, i.intendedWidth * eX + i.intendedHeight * eY, SKINALPHAS_MAINMENU_z);
	i = tm_mp_preview = NEW(GodnoticMapPreviewDialog);
	i.configureDialog(i);
	mainwindow.addItemCentered(mainwindow, i, i.intendedWidth * eX + i.intendedHeight * eY, SKINALPHAS_MAINMENU_z);
}

void TouchMapPick_Open(entity btn, void(string bsp) onPick, bool allMaps)
{
	if (!tm_mp_dialog)
		return;
	// The names now; titles and pictures as each map comes into view.
	if (allMaps)
		_MapInfo_FilterGametype(MAPINFO_TYPE_ALL, 0, 0, MapInfo_ForbiddenFlags(), 0);
	for (int i = 0; i < tm_mp_n; ++i)
	{
		if (tm_mp_pic[i] != "" && tm_mp_pic[i] != "nopreview_map")
			freepic(substring(tm_mp_pic[i], 1, -1));
		strfree(tm_mp_pic[i]);
		strfree(tm_mp_title[i]);
		strfree(tm_mp_modes[i]);
		tm_mp_pic[i] = tm_mp_title[i] = tm_mp_modes[i] = "";
	}
	tm_mp_n = 0;
	for (int i = 0; i < MapInfo_count && tm_mp_n < TOUCH_MP_MAX; ++i)
	{
		string m = MapInfo_BSPName_ByID(i);
		if (m == "")
			continue;
		strcpy(tm_mp_bsp[tm_mp_n], m);
		++tm_mp_n;
	}
	// Create's own map list follows the filter: give it back.
	entity ml = find(NULL, classname, "XonoticMapList");
	if (allMaps && ml)
		ml.refilter(ml);
	tm_mp_sel = -1;
	tm_mp_onPick = onPick;
	if (tm_mp_grid)
		tm_mp_grid.vg_scroll = 0;
	DialogOpenButton_Click(btn, tm_mp_dialog);
}
