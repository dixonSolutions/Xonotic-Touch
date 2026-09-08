#include "dialog_touch_wizard.qh"

#include "touch_menu_layout.qh"
#include "touchbutton.qh"
#include "textlabel.qh"
#include "checkbox.qh"

bool XonoticTouchSetupDialog_shouldShow()
{
	return cvar("vid_touchscreen") && !cvar("touch_setup_done");
}

void TouchSetup_Finish_Click(entity btn, entity me)
{
	TouchMenu_WriteLayoutFile("touch.layout.cfg");
	// seta, not cvar_set: touch_setup_done is registered by the client VM in
	// touch_init.qc, which has not loaded while the menu is up, so cvar_set
	// found nothing and the wizard reopened on every launch. seta creates the
	// cvar and archives it, the same way dialog_firstrun.qc sets
	// touch_profile_done.
	localcmd("seta touch_setup_done 1\n");
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
		me.TD(me, 2, 2, e = makeXonoticTouchCommandButton(_("Standard"), '0 0 0', "exec touch/profiles/standard.cfg; menu_cmd sync", 0));
		me.TD(me, 2, 2, e = makeXonoticTouchCommandButton(_("Casual"), '0 0 0', "exec touch/profiles/casual.cfg; menu_cmd sync", 0));
		me.TD(me, 2, 2, e = makeXonoticTouchCommandButton(_("Competitive"), '0 0 0', "exec touch/profiles/competitive.cfg; menu_cmd sync", 0));
	me.TR(me);
		me.TD(me, 2, 3, e = makeXonoticTouchCommandButton(_("Left-handed"), '0 0 0', "exec touch/profiles/left.cfg; menu_cmd sync", 0));
		me.TD(me, 2, 3, e = makeXonoticTouchCommandButton(_("Minimal"), '0 0 0', "exec touch/profiles/minimal.cfg; menu_cmd sync", 0));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticCheckBox_T(0, "touch_handedness", _("Left-handed (mirror layout)"), string_null));
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0.5, _("Tip: use Singleplayer or Multiplayer like desktop Xonotic. Pause → Touch controls to fine-tune the HUD.")));
		e.allowWrap = true;
	me.TR(me);
	me.TR(me);
	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 2, 6, e = makeXonoticTouchButton(_("Save & continue"), '0 0 0'));
			e.onClick = TouchSetup_Finish_Click;
			e.onClickEntity = me;
}
