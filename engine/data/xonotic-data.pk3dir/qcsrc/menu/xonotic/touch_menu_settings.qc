// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_menu_settings.qh"

#include "touch_menu_scale.qh"
#include "textlabel.qh"
#include "slider.qh"
#include "checkbox.qh"
#include "button.qh"
#include "radiobutton.qh"

float touch_menu_size_at_init; // menu_touch_size this menu was built with

entity makeGodnoticMenuSizeTab()
{
	entity me = NEW(GodnoticMenuSizeTab);
	me.configureDialog(me);
	return me;
}

void GodnoticMenuSizePreview_resizeNotify(entity me, vector relOrigin, vector relSize, vector absOrigin, vector absSize)
{
	SUPER(GodnoticMenuSizePreview).resizeNotify(me, relOrigin, relSize, absOrigin, absSize);
}

void GodnoticMenuSizePreview_draw(entity me)
{
	if (!touch_menu_size_at_init)
		touch_menu_size_at_init = TouchMenuScale_size();
	float now = touch_menu_on ? touch_menu_size_at_init : 0;
	float want = (autocvar_menu_touch_size < 0) ? 0 : TouchMenuScale_size();
	if (me.size.x <= 0 || me.size.y <= 0)
		return;
	// Today's rows are TOUCH_MENU_ROW_DP at the size the menu was built with.
	float row = touch_menu_on ? TouchMenuScale_dpToUnits(TOUCH_MENU_ROW_DP * now) : SKINFONTSIZE_NORMAL * SKINHEIGHT_NORMAL;
	// Two samples, today's and the new size, side by side.
	for (int i = 0; i < 2; ++i)
	{
		float f = (i == 0) ? 1 : ((now > 0 && want > 0) ? want / now : 1);
		string cap = (i == 0) ? _("Now") : _("New size");
		if (i == 1 && want <= 0)
			cap = _("New: desktop layout");
		float x0 = i * 0.5 + 0.01;
		float h = min(1, row * f / me.size.y);
		float fs = SKINFONTSIZE_NORMAL * f;
		vector fsz = eX * (fs / me.size.x) + eY * (fs / me.size.y);
		vector csz = eX * (SKINFONTSIZE_NORMAL * 0.8 / me.size.x) + eY * (SKINFONTSIZE_NORMAL * 0.8 / me.size.y);
		draw_Text(eX * x0, cap, csz, '0.7 0.8 1', 0.9, false);
		vector o = eX * x0 + eY * (csz.y * 1.4);
		vector s = eX * 0.46 + eY * min(h, 1 - o.y);
		draw_Fill(o, s, '0.1 0.3 0.55', 0.6);
		draw_Fill(o, eX * s.x + eY * (s.y * 0.06), '0.5 0.75 1', 0.8);
		draw_CenterText(o + eX * (s.x * 0.5) + eY * (0.5 * (s.y - fsz.y)), _("Sample button"), fsz, '1 1 1', 1, false);
	}
}

void TouchMenuSize_Apply(entity btn, entity me)
{
	// The layout is fixed once built: restart the menu into the new size and
	// come back to this page.
	localcmd("\nmenu_restart\nmenu_cmd directmenu mobilemenu\n");
}

void GodnoticMenuSizeTab_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Menu size")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Size:")));
		me.TD(me, 1, 3, e = makeXonoticSlider(0.6, 1.6, 0.1, "menu_touch_size"));
			e.tooltip = _("How big the menu is on this screen. 1 is sized from the screen's density");
		setDependent(e, "menu_touch_size", 0, 2);
		me.TD(me, 1, 1.5, e = makeXonoticButton(_("Apply"), '0.6 1 0.6'));
			e.onClick = TouchMenuSize_Apply;
			e.onClickEntity = me;
	me.TR(me);
		me.TD(me, 4, me.columns, e = NEW(GodnoticMenuSizePreview));
	me.TR(me);
	me.TR(me);
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBoxEx(-1, 1, "menu_touch_size", _("Use desktop layout")));
			e.tooltip = _("The stock Xonotic menu, sized for a mouse. Apply to switch");
		me.TD(me, 1, 3, e = makeXonoticTextLabel(0, strzone(sprintf(_("Screen: %d dpi, rows %d dp"), rint(TouchMenuScale_dpi()), TOUCH_MENU_ROW_DP)))); // kept: a label holds on to its text
			e.allowCut = true;
	// Which keyboard a menu text field gets (touch_ui.c, touch_menu_keyboard).
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Keyboard for text fields")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(1, "touch_menu_keyboard", "0", _("Automatic"),
			_("The game's keys for languages written in Latin letters, the system keyboard for the others")));
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(1, "touch_menu_keyboard", "1", _("Game keys"),
			_("The same keys as the console and chat, with accents on a long press")));
		me.TD(me, 1, 2, e = makeXonoticRadioButton_T(1, "touch_menu_keyboard", "2", _("System keyboard"),
			_("The phone's own keyboard, with its input methods")));
	me.TR(me);
		me.TD(me, 2, me.columns, e = makeXonoticTextLabel(0, _("The game's keys type Latin letters and their accents (hold a key). Chinese, Japanese, Korean, Cyrillic, Greek and other scripts need the system keyboard's input method: Automatic picks it when the game or the system is set to such a language.")));
			e.allowWrap = true;
}
