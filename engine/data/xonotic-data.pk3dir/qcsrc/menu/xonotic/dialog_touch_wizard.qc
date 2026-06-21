#include "dialog_touch_wizard.qh"

#include "touch_menu_layout.qh"
#include "textlabel.qh"
#include "commandbutton.qh"
#include "checkbox.qh"
#include "button.qh"

bool XonoticTouchSetupDialog_shouldShow()
{
	return cvar("vid_touchscreen") && !cvar("touch_setup_done");
}

void TouchSetup_Finish_Click(entity btn, entity me)
{
	TouchMenu_WriteLayoutFile("touch.layout.cfg");
	cvar_set("touch_setup_done", "1");
	localcmd("saveconfig\n");
	Dialog_Close(btn, me);
}

void XonoticTouchSetupDialog_fill(entity me)
{
	entity e;

	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Welcome to Xonotic Touch! Pick a control preset and handedness. You can drag buttons to reposition them later in Settings → Touch or in-game customize mode.")));
		e.allowWrap = true;
		e.allowWrap_spacing = 0.5;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticHeaderLabel(_("Choose a preset")));
	me.TR(me);
		me.TD(me, 1, 2, e = makeXonoticCommandButton(_("Standard"), '0 0 0', "exec touch/profiles/standard.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 2, e = makeXonoticCommandButton(_("Casual"), '0 0 0', "exec touch/profiles/casual.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 2, e = makeXonoticCommandButton(_("Competitive"), '0 0 0', "exec touch/profiles/competitive.cfg; menu_cmd sync", 0));
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticCommandButton(_("Left-handed"), '0 0 0', "exec touch/profiles/left.cfg; menu_cmd sync", 0));
		me.TD(me, 1, 3, e = makeXonoticCommandButton(_("Minimal"), '0 0 0', "exec touch/profiles/minimal.cfg; menu_cmd sync", 0));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticCheckBox_T(0, "touch_handedness", _("Left-handed (mirror layout)"), string_null));
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0.5, _("Tip: use Play now for a quick bot match, then Pause → Touch controls to fine-tune.")));
		e.allowWrap = true;
	me.TR(me);
	me.TR(me);
	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 1, 6, e = makeXonoticButton(_("Save & continue"), '0 0 0'));
			e.onClick = TouchSetup_Finish_Click;
			e.onClickEntity = me;
}
