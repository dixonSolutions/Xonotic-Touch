// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialog_touch_credits.qh"

#include "image.qh"
#include "button.qh"
#include "commandbutton.qh"
#include "textlabel.qh"
#include "dialog_termsofservice.qh"
#include "../item/modalcontroller.qh"

// --- drag to scroll -------------------------------------------------------
// pressed 4: a finger dragging the list itself (ListBox uses 1 to 3).

bool GodnoticDragScroll_press(entity me, vector pos)
{
	if (pos.x < 0 || pos.y < 0 || pos.x >= 1 - me.controlWidth || pos.y >= 1)
		return false;
	me.scrolling = 0; // credits: stop the auto-scroll
	me.pressed = 4;
	me.pressOffset = pos.y;
	me.previousValue = me.scrollPos;
	return true;
}

bool GodnoticDragScroll_drag(entity me, vector pos)
{
	if (me.pressed != 4)
		return false;
	float total = me.getTotalHeight(me);
	me.scrollPosTarget = bound(0, me.previousValue + (me.pressOffset - pos.y), max(0, total - 1));
	me.scrollPos = me.scrollPosTarget;
	return true;
}

bool GodnoticDragScroll_release(entity me, vector pos)
{
	if (me.pressed != 4)
		return false;
	me.pressed = 0;
	return true;
}

bool GodnoticCreditsList_mousePress(entity me, vector pos)
{
	if (GodnoticDragScroll_press(me, pos))
		return true;
	return SUPER(GodnoticCreditsList).mousePress(me, pos);
}
float GodnoticCreditsList_mouseDrag(entity me, vector pos)
{
	if (GodnoticDragScroll_drag(me, pos))
		return 1;
	return SUPER(GodnoticCreditsList).mouseDrag(me, pos);
}
float GodnoticCreditsList_mouseRelease(entity me, vector pos)
{
	if (GodnoticDragScroll_release(me, pos))
		return 1;
	return SUPER(GodnoticCreditsList).mouseRelease(me, pos);
}

bool GodnoticDragTextBox_mousePress(entity me, vector pos)
{
	if (GodnoticDragScroll_press(me, pos))
		return true;
	return SUPER(GodnoticDragTextBox).mousePress(me, pos);
}
float GodnoticDragTextBox_mouseDrag(entity me, vector pos)
{
	if (GodnoticDragScroll_drag(me, pos))
		return 1;
	return SUPER(GodnoticDragTextBox).mouseDrag(me, pos);
}
float GodnoticDragTextBox_mouseRelease(entity me, vector pos)
{
	if (GodnoticDragScroll_release(me, pos))
		return 1;
	return SUPER(GodnoticDragTextBox).mouseRelease(me, pos);
}

// --- the credits list -----------------------------------------------------
// "**" a title, "*" a heading, anything else a plain line (credits.qc).

void GodnoticCredits_add(int buf, string s)
{
	bufstr_add(buf, s, true);
}

void GodnoticCreditsList_configureXonoticCreditsList(entity me)
{
	SUPER(GodnoticCreditsList).configureXonoticCreditsList(me);
	int xon = me.bufferIndex;
	int n = buf_create();
	GodnoticCredits_add(n, strcat("**", "Godnotic"));
	GodnoticCredits_add(n, _("A touch-first fork of the Xonotic arena shooter"));
	GodnoticCredits_add(n, _("for phones, tablets and desktops"));
	GodnoticCredits_add(n, "github.com/dixonSolutions/Godnotic");
	GodnoticCredits_add(n, "");
	GodnoticCredits_add(n, strcat("*", _("Components")));
	GodnoticCredits_add(n, _("Engine: github.com/dixonSolutions/Godnotic-darkplaces"));
	GodnoticCredits_add(n, _("Game code and data: github.com/dixonSolutions/Godnotic-data"));
	GodnoticCredits_add(n, "");
	GodnoticCredits_add(n, strcat("*", _("Licences")));
	GodnoticCredits_add(n, _("Godnotic's own code: GPL-3.0-or-later"));
	GodnoticCredits_add(n, _("Xonotic game code and data: GPL-3.0-or-later"));
	GodnoticCredits_add(n, _("DarkPlaces engine: GPL-2.0-or-later"));
	GodnoticCredits_add(n, _("Lua 5.4 (mods): MIT"));
	GodnoticCredits_add(n, _("zstd decoder: BSD-3-Clause"));
	GodnoticCredits_add(n, _("d0_blind_id (player IDs): BSD"));
	GodnoticCredits_add(n, _("GMQCC (QuakeC compiler): MIT"));
	GodnoticCredits_add(n, _("Xolonium font, Xonotic edition: GPL-2.0-or-later with font exception"));
	GodnoticCredits_add(n, _("Xonotic emblem: LGPL-2.1-or-later or CC-BY-3.0"));
	GodnoticCredits_add(n, _("Full texts: LICENSE and engine/COPYING in the repository"));
	GodnoticCredits_add(n, "");
	GodnoticCredits_add(n, strcat("**", _("Xonotic")));
	GodnoticCredits_add(n, "");
	for (int i = 0, c = buf_getsize(xon); i < c; ++i)
		GodnoticCredits_add(n, bufstr_get(xon, i));
	buf_del(xon);
	me.bufferIndex = n;
	me.nItems = buf_getsize(n);
}

entity makeGodnoticCreditsList()
{
	entity me = NEW(GodnoticCreditsList);
	me.configureXonoticCreditsList(me);
	return me;
}

void GodnoticCreditsDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 3, me.columns, e = makeXonoticImage("/gfx/xonotic_logo", 3));
	me.gotoRC(me, 3.2, 0);
		me.TD(me, 1.5, 1, e = makeXonoticCommandButton_T(_("GitHub"), '0 0 0', strcat("openurl ", GODNOTIC_GITHUB_URL), 0,
			_("Open github.com/dixonSolutions/Godnotic in the browser")));
		me.TD(me, 1.5, 1, e = makeXonoticCommandButton_T(_("Licence"), '0 0 0', strcat("openurl ", GODNOTIC_GITHUB_URL, "/blob/main/LICENSE"), 0,
			_("Open the GPL-3.0 licence text on GitHub")));
		me.TD(me, 1.5, 1, e = makeXonoticButton_T(_("Terms of service"), '0 0 0',
			_("Xonotic's terms of service, which govern XonStat (stats.xonotic.org)")));
			e.onClick = GodnoticTerms_Open;
			e.onClickEntity = e;
	me.gotoRC(me, 5, 0);
		me.TD(me, me.rows - 6.2, me.columns, me.creditsList = makeGodnoticCreditsList());
	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 1, me.columns, e = makeXonoticButton(_("OK"), '0 0 0'));
			e.onClick = Dialog_Close;
			e.onClickEntity = me;
}

// --- the terms viewer -----------------------------------------------------

void GodnoticTerms_OnGet(entity fh, entity me, int status)
{
	switch (status)
	{
		case URL_READY_ERROR:
			me.textBox.setText(me.textBox, _("Could not reach update.xonotic.org. The terms are at https://update.xonotic.org/tos.txt"));
			break;
		case URL_READY_CANREAD:
		{
			string temp = "";
			for (string s; (s = url_fgets(fh)); )
				temp = (temp != "") ? strcat(temp, "\n", s) : s;
			url_fclose(fh);
			me.textBox.setText(me.textBox, temp);
			break;
		}
		default:
			break;
	}
}

void GodnoticTerms_Open(entity btn, entity unused)
{
	entity dlg = godnotic_terms_dialog;
	if (!dlg)
		return;
	dlg.textBox.setText(dlg.textBox, _("Loading..."));
	url_single_fopen(termsofservice_url, FILE_READ, GodnoticTerms_OnGet, dlg);
	DialogOpenButton_Click(btn, dlg);
}

void GodnoticTermsDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 2, me.columns, e = makeXonoticTextLabel(0, _("These apply to XonStat player statistics (stats.xonotic.org). Godnotic sends statistics only when you allow it in Settings > Mobile > Privacy, and never asks you to accept anything to play.")));
			e.allowWrap = true;
	me.gotoRC(me, 2.2, 0);
		me.textBox = NEW(GodnoticDragTextBox);
		me.textBox.configureXonoticListBox(me.textBox);
		me.textBox.allowColors = true;
		me.TD(me, me.rows - 3.4, me.columns, me.textBox);
	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 1, me.columns, e = makeXonoticButton(_("Close"), '0 0 0'));
			e.onClick = Dialog_Close;
			e.onClickEntity = me;
}
