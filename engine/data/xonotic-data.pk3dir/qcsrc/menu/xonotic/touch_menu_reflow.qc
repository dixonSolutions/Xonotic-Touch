// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_reflow.qh"

#include "../item/modalcontroller.qh"
#include "../item/checkbox.qh"
#include "../item/inputbox.qh"
#include "touch_menu_scale.qh"
#include "touch_menu_sheet.qh"
#include "touchbutton.qh"
#include "dialog.qh"
#include "dialog_touch_lan.qh"
#include "dialog_touch_mods.qh"
#include "serverlist.qh"

// configureDialog's grid for the dialog laid out with `rows` rows: sets its
// itemOrigin, itemSize and itemSpacing, and returns its height then.
float TouchReflow_grid(entity me, float rows)
{
	float absWidth = me.intendedWidth * conwidth;
	float absHeight = me.borderLines * me.titleHeight + me.marginTop + rows * me.rowHeight + (rows - 1) * me.rowSpacing + me.marginBottom;
	me.itemOrigin = eX * (me.marginLeft / absWidth)
		+ eY * ((me.borderLines * me.titleHeight + me.marginTop) / absHeight);
	me.itemSize = eX * ((1 - (me.marginLeft + me.marginRight + me.columnSpacing * (me.columns - 1)) / absWidth) / me.columns)
		+ eY * (me.rowHeight / absHeight);
	me.itemSpacing = me.itemSize
		+ eX * (me.columnSpacing / absWidth)
		+ eY * (me.rowSpacing / absHeight);
	return absHeight / conheight;
}

// An item put where TD would put it, in the grid TouchReflow_grid set.
void TouchReflow_place(entity me, entity e, float row, float col, float rowspan, float colspan)
{
	e.Container_origin = me.itemOrigin + eX * (col * me.itemSpacing.x) + eY * (row * me.itemSpacing.y);
	e.Container_size = me.itemSize + eX * ((colspan - 1) * me.itemSpacing.x) + eY * ((rowspan - 1) * me.itemSpacing.y);
}

// Out of sight and out of reach: no alpha, below the dialog (itemFromPoint
// does not look at alpha), its size kept so nothing divides by zero.
void TouchReflow_hide(entity me, entity e)
{
	me.setAlphaOf(me, e, 0);
	e.Container_origin_y = 1.5;
}

void TouchReflow_show(entity me, entity e)
{
	me.setAlphaOf(me, e, 1);
}

// ---- Settings: a list of categories, each opening as the whole sheet ----

const int TOUCH_SETTINGS_TABS = 12;
entity touch_settings_dialog;
entity touch_settings_mc;
entity touch_settings_backbtn;
entity touch_settings_btn[TOUCH_SETTINGS_TABS];
int touch_settings_nbtn;
entity touch_settings_shown;     // the category shown last frame
bool touch_settings_cat;         // a category is open
bool touch_settings_was_shown;
string touch_settings_title;

void TouchSettings_setTitle(string t)
{
	entity me = touch_settings_dialog;
	if (!me.border)
		return;
	strcpy(touch_settings_title, t);
	me.border.setText(me.border, touch_settings_title);
}

void TouchSettings_Show(bool cat)
{
	entity me = touch_settings_dialog;
	entity mc = touch_settings_mc;
	float h;
	int i;
	touch_settings_cat = cat;
	if (cat)
	{
		// The category alone, as tall as the dialog used to give it.
		h = TouchReflow_grid(me, me.rows - 3.5);
		for (i = 0; i < touch_settings_nbtn; ++i)
			TouchReflow_hide(me, touch_settings_btn[i]);
		TouchReflow_show(me, mc);
		TouchReflow_place(me, mc, 0, 0, me.rows - 3.5, me.columns);
		// "< Back" in the title bar's left end.
		entity b = touch_settings_backbtn;
		TouchReflow_show(me, b);
		float th = me.borderLines * me.titleHeight / (h * conheight);
		b.Container_origin = eX * 0.012 + eY * (0.1 * th);
		b.Container_size = eX * 0.16 + eY * (0.8 * th);
		string t = _("Settings");
		for (entity x = mc.firstChild; x; x = x.nextSibling)
			if (x.ModalController_state == 1 && x.tabSelectingButton)
				t = sprintf("%s: %s", _("Settings"), x.tabSelectingButton.text);
		TouchSettings_setTitle(t);
		me.touch_sheet_nopin = false;
	}
	else
	{
		// Big buttons, three or five to a row.
		int per = (touch_settings_nbtn > 9) ? 5 : 3;
		int nrows = ceil(touch_settings_nbtn / per);
		h = TouchReflow_grid(me, nrows * 1.85 - 0.25);
		for (i = 0; i < touch_settings_nbtn; ++i)
		{
			entity e = touch_settings_btn[i];
			TouchReflow_show(me, e);
			TouchReflow_place(me, e, floor(i / per) * 1.85, (i % per) * (me.columns / per), 1.6, me.columns / per);
			e.fontSize = SKINFONTSIZE_TITLE;
		}
		TouchReflow_hide(me, mc);
		TouchReflow_hide(me, touch_settings_backbtn);
		TouchSettings_setTitle(_("Settings"));
		// Its last row of categories is no action row.
		me.touch_sheet_nopin = true;
	}
	me.intendedHeight = h;
	TouchSheet_resized(me);
}

void TouchSettings_TabClick(entity btn, entity tab)
{
	TabButton_Click(btn, tab);
	TouchSettings_Show(true);
}

void TouchSettings_BackClick(entity btn, entity me)
{
	TouchSettings_Show(false);
}

bool TouchSettings_Back()
{
	entity me = touch_settings_dialog;
	if (!me || !touch_settings_cat || me.ModalController_state != 1)
		return false;
	m_play_click_sound(MENU_SOUND_CLOSE);
	TouchSettings_Show(false);
	return true;
}

void TouchSettings_Reflow(entity me, entity mc)
{
	if (!touch_menu_on)
		return;
	touch_settings_dialog = me;
	touch_settings_mc = mc;
	touch_settings_nbtn = 0;
	for (entity e = me.firstChild; e; e = e.nextSibling)
		if (e.instanceOfButton && e.onClick == TabButton_Click && touch_settings_nbtn < TOUCH_SETTINGS_TABS)
		{
			e.onClick = TouchSettings_TabClick;
			// Mods: the manager, on a sheet of its own with the whole height.
			if (e.onClickEntity.name == "touchmodstab")
			{
				e.onClick = TouchMods_OpenManager;
				e.onClickEntity = me;
			}
			touch_settings_btn[touch_settings_nbtn] = e;
			++touch_settings_nbtn;
		}
	entity b = touch_settings_backbtn = makeGodnoticButton(_("< Back"), '0 0 0');
	b.onClick = TouchSettings_BackClick;
	b.onClickEntity = me;
	b.touch_sheet_titlebar = true;
	me.addItem(me, b, '0 0 0', '0.1 0.05 0', 0);
	// addTab showed the first category: anything else shown when Settings
	// opens came from m_goto (directmenu mobilemenu) and opens as itself.
	touch_settings_shown = mc.firstChild;
	TouchSettings_Show(false);
}

void TouchSettings_Frame()
{
	entity me = touch_settings_dialog;
	if (!me)
		return;
	bool shown = (me.ModalController_state != 0);
	entity cur = NULL;
	for (entity x = touch_settings_mc.firstChild; x; x = x.nextSibling)
		if (x.ModalController_state == 1)
			cur = x;
	if (shown && !touch_settings_was_shown)
		TouchSettings_Show(cur != touch_settings_shown);
	else if (shown && cur != touch_settings_shown && !touch_settings_cat)
		TouchSettings_Show(true);
	touch_settings_shown = cur;
	touch_settings_was_shown = shown;
}

// ---- The server list: a filter row that fits, a slim sort bar, more rows ----

// The items whose centre is in the row starting at `row`, left to right,
// into touch_row.
entity touch_row[12];
int TouchServers_row(entity me, float row)
{
	int maxn = 12;
	float y0 = me.itemOrigin.y + row * me.itemSpacing.y;
	float y1 = y0 + me.itemSize.y;
	int n = 0;
	for (entity e = me.firstChild; e; e = e.nextSibling)
	{
		float cy = e.Container_origin.y + 0.5 * e.Container_size.y;
		if (cy < y0 || cy > y1 || n >= maxn)
			continue;
		int k = n;
		while (k > 0 && touch_row[k - 1].Container_origin.x > e.Container_origin.x)
		{
			touch_row[k] = touch_row[k - 1];
			--k;
		}
		touch_row[k] = e;
		++n;
	}
	return n;
}

void TouchServers_Reflow(entity me)
{
	if (!touch_menu_on || !main)
		return;
	int n, i;
	entity refresh = NULL, slist = NULL, label = NULL, input = NULL;
	entity boxes[8];
	int nb = 0;

	// The filter row (row 0.5 of XonoticServerListTab_fill), on two lines:
	// Filter, its field and Refresh; then the checkboxes, each with room for
	// its label (Categories, LAN, Empty, Full, Laggy, Pause).
	n = TouchServers_row(me, 0.5);
	for (i = 0; i < n; ++i)
	{
		entity e = touch_row[i];
		if (e.instanceOfCheckBox)
		{
			if (nb < 8)
				boxes[nb++] = e;
		}
		else if (e.instanceOfInputBox)
			input = e;
		else if (e.instanceOfButton)
			refresh = e;
		else if (e.instanceOfLabel)
			label = e;
	}
	if (!refresh || !input)
		return;
	slist = refresh.onClickEntity;
	if (label)
		TouchReflow_place(me, label, 0.2, 0, 1, 0.7);
	TouchReflow_place(me, input, 0.2, 0.75, 1, 4.3);
	TouchReflow_place(me, refresh, 0.2, 5.15, 1, 1.35);
	float w = me.columns / max(1, nb);
	for (i = 0; i < nb; ++i)
		TouchReflow_place(me, boxes[i], 1.2, i * w, 1, w);

	// The address row: the small Host LAN... goes (hosting is Create's LAN
	// box), the address gets its room.
	n = TouchServers_row(me, me.rows - 2);
	if (n == 5 && touch_row[4].onClick == TouchLan_OpenHost)
	{
		me.removeItem(me, touch_row[4]);
		TouchReflow_place(me, touch_row[1], me.rows - 2, 0.6, 1, 2.9);
		TouchReflow_place(me, touch_row[2], me.rows - 2, 3.5, 1, 1.5);
		TouchReflow_place(me, touch_row[3], me.rows - 2, 5.0, 1, 1.5);
	}

	// A slim sort bar right over the list, the list down to the address row.
	float bar = 0.72;
	float bartop = 2.3;
	float listtop = bartop + bar;
	entity sb[5];
	sb[0] = slist.sortButton1; sb[1] = slist.sortButton2; sb[2] = slist.sortButton3;
	sb[3] = slist.sortButton4; sb[4] = slist.sortButton5;
	for (i = 0; i < 5; ++i)
		if (sb[i])
		{
			sb[i].Container_origin_y = me.itemOrigin.y + bartop * me.itemSpacing.y;
			sb[i].Container_size_y = bar * me.itemSize.y;
			sb[i].fontSize = SKINFONTSIZE_NORMAL * 0.85;
		}
	TouchReflow_place(me, slist, listtop, 0, me.rows - 2 - 0.15 - listtop, me.columns);
	slist.fontSize = SKINFONTSIZE_NORMAL * 0.9;
}
