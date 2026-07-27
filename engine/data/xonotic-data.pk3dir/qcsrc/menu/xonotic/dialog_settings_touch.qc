#include "dialog_settings_touch.qh"

#include "touch_menu_layout.qh"
#include "textlabel.qh"
#include "slider.qh"
#include "checkbox.qh"
#include "commandbutton.qh"
#include "button.qh"
#include "mixedslider.qh"
#include "touchconsolepreview.qh"

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

	registercvar("touch_kb_preview_show", "1", 0);

	// Tab height MUST be 15.5 to match Settings TabController (settings.rows - 3.5).
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Console & chat")));
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticCheckBox_T(0, "touch_kb_preview_show", _("Show console preview"), _("Live preview of the real touch console — uses the same engine sheet as in-game")));
	me.TR(me);
		// Empty cell: engine TouchUI_DrawPreview paints the real sheet here.
		me.TD(me, 4, me.columns, e = makeXonoticTouchConsolePreview());
	me.TR(me);
	me.TR(me);
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Keyboard height:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.28, 0.70, 0.01, "touch_kb_height", _("Fraction of the sheet used by keys")));
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Key opacity:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.3, 1.0, 0.05, "touch_kb_opacity", string_null));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Shade:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.2, 0.9, 0.05, "touch_conui_shade", _("Console background dim")));
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Key gap:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(0.002, 0.020, 0.001, "touch_kb_gap", _("Spacing between keys")));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCheckBox_T(0, "touch_kb_split", _("Split keyboard (landscape thumbs)"), string_null));
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Console text:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(8, 18, 1, "con_textsize", _("Scrollback / input text size")));

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Preset")));
	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Standard"), '0 0 0', "exec touch/profiles/standard.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Casual"), '0 0 0', "exec touch/profiles/casual.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Competitive"), '0 0 0', "exec touch/profiles/competitive.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 1.5, e = makeXonoticCommandButton(_("Left hand"), '0 0 0', "exec touch/profiles/left.cfg; menu_cmd sync", 0));

	me.TR(me);
		me.TD(me, 1, 1.5, e = makeXonoticTextLabel(0, _("Sensitivity:")));
		me.TD(me, 1, 1.5, e = makeXonoticSlider_T(1.5, 5.0, 0.1, "touch_sens_base", _("DPI-normalized look speed")));
		me.TD(me, 1, 3, e = makeXonoticCommandButton(_("Customize layout (in game)"), '0 0 0', "touch_customize 1; togglemenu 0", COMMANDBUTTON_CLOSE));

	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticButton(_("Save layout"), '0 0 0'));
			e.onClick = TouchMenu_SaveLayout_Click;
		me.TD(me, 1, 2, e = makeXonoticCommandButton(_("Apply & refresh"), '0 0 0', "menu_cmd sync", COMMANDBUTTON_APPLY));
		me.TD(me, 1, 2, e = makeXonoticButton(_("Reset preset"), '0 0 0'));
			e.onClick = TouchMenu_ResetPreset_Click;
}
