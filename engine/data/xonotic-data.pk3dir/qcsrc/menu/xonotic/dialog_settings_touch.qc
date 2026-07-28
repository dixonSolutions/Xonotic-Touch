#include "dialog_settings_touch.qh"

#include "touch_menu_layout.qh"
#include "textlabel.qh"
#include "slider.qh"
#include "checkbox.qh"
#include "commandbutton.qh"
#include "button.qh"
#include "mixedslider.qh"

void TouchMenu_SaveLayout_Click(entity me, entity other)
{
	TouchMenu_WriteLayoutFile("touch.layout.cfg");
	localcmd("saveconfig\n");
}

void TouchMenu_Export_Click(entity me, entity other)
{
	TouchMenu_WriteLayoutFile("touch_export.cfg");
}

void TouchMenu_Import_Click(entity me, entity other)
{
	TouchMenu_WriteLayoutFile("touch.layout.bak");
	localcmd("exec touch.layout.cfg\nmenu_cmd sync\n");
}

void TouchMenu_ResetPreset_Click(entity me, entity other)
{
	localcmd(strcat("exec touch/profiles/", cvar_string("touch_preset"), ".cfg\nmenu_cmd sync\n"));
}

entity makeXonoticTouchSettingsTab()
{
	entity me = NEW(XonoticTouchSettingsTab);
	me.configureDialog(me);
	return me;
}

void XonoticTouchSettingsTab_fill(entity me)
{
	entity e;

	// Tab height MUST be 15.5 to match Settings TabController (settings.rows - 3.5).
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Preset")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Standard"), '0 0 0', "exec touch/profiles/standard.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Casual"), '0 0 0', "exec touch/profiles/casual.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Competitive"), '0 0 0', "exec touch/profiles/competitive.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Left hand"), '0 0 0', "exec touch/profiles/left.cfg; menu_cmd sync", 0));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Look & appearance")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Sensitivity:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(1.5, 5.0, 0.1, "touch_sens_base", _("Look speed (DPI-normalized)")));
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Vertical:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.5, 1.5, 0.05, "touch_sens_y_mult", _("Pitch vs yaw multiplier")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCheckBox_T(0, "touch_invert_y", _("Invert look Y"), string_null));
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Opacity:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.3, 1.0, 0.05, "touch_opacity", _("Control overlay opacity")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Control scale:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.7, 1.4, 0.05, "touch_scale", _("Size of move/fire/jump widgets")));
		me.TD(me, 1, 3, e = makeXonoticCommandButton(_("Customize layout (in game)"), '0 0 0', "touch_customize 1; togglemenu 0", COMMANDBUTTON_CLOSE));

	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticButton(_("Save layout"), '0 0 0'));
			e.onClick = TouchMenu_SaveLayout_Click;
		me.TD(me, 1, 2, e = makeXonoticCommandButton(_("Apply & refresh"), '0 0 0', "menu_cmd sync", COMMANDBUTTON_APPLY));
		me.TD(me, 1, 2, e = makeXonoticButton(_("Reset preset"), '0 0 0'));
			e.onClick = TouchMenu_ResetPreset_Click;
}
