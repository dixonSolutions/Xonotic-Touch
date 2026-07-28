#include "dialog_gamemenu.qh"

#include "textlabel.qh"
#include "commandbutton.qh"
#include "leavematchbutton.qh"
#include "button.qh"

void XonoticGameMenuDialog_destroy(entity me)
{
	cvar_set("_menu_gamemenu_dialog_available", "0");
}

void XonoticGameMenuDialog_draw(entity me)
{
	entity btn = me.joinButton;
	if(cvar("g_campaign"))
	{
		if (btn.toString(btn) == _("Join!"))
		{
			btn.setText(btn, _("Restart level"));
			btn.onClickCommand = "resetmatch";
		}
	}
	else
	{
		if (btn.toString(btn) == _("Restart level"))
		{
			btn.setText(btn, _("Join!"));
			btn.onClickCommand = "join";
		}
	}
	SUPER(XonoticGameMenuDialog).draw(me);
}

void XonoticGameMenuDialog_fill(entity me)
{
	registercvar("_menu_gamemenu_dialog_available", "0", 0);
	cvar_set("_menu_gamemenu_dialog_available", "1");

	entity e;
	// Always first — Escape equivalent for touch (and harmless with a keyboard).
	me.TR(me);
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Resume"), '0 0 0', "togglemenu 0", COMMANDBUTTON_CLOSE));
	me.TR(me);
		// Leave the match — never COMMANDBUTTON_CLOSE (that m_hide()s while still
		// connected and dumps the player back into the live/paused game).
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Main menu"), '0 0 0', LEAVEMATCH_CMD, 0));
		// Mid-match tweaks: use menu_cmd <name> (hideMenuOnClose=false) so closing
		// Settings/etc returns to GameMenu instead of unpausing into the match.
	me.TR(me);
		me.TDempty(me, 0.1);
		me.TD(me, 1, 0.8, e = makeXonoticCommandButton(_("Servers"), '0 0 0', "menu_cmd servers", 0));
	me.TR(me);
		me.TDempty(me, 0.1);
		me.TD(me, 1, 0.8, e = makeXonoticCommandButton(_("Profile"), '0 0 0', "menu_cmd profile", 0));
	me.TR(me);
		me.TDempty(me, 0.1);
		me.TD(me, 1, 0.8, e = makeXonoticCommandButton(_("Settings"), '0 0 0', "menu_cmd settings", 0));
	me.TR(me);
		me.TDempty(me, 0.1);
		// XONOTIC_UT_TOUCH_GAMEMENU
		me.TD(me, 1, 0.8, e = makeXonoticCommandButton(_("Touch controls"), '0 0 0', "menu_cmd touchsettings", 0));
	me.TR(me);
		me.TDempty(me, 0.1);
		me.TD(me, 1, 0.8, e = makeXonoticCommandButton(_("Input"), '0 0 0', "menu_cmd inputsettings", 0));
	me.TR(me);
		me.TDempty(me, 0.1);
		me.TD(me, 1, 0.8, e = makeXonoticCommandButton(_("Guide"), '0 0 0', "menu_cmd guide", 0));
	me.TR(me);
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Quick menu"), '0 0 0', "quickmenu", COMMANDBUTTON_CLOSE));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 1, e = me.joinButton = makeXonoticCommandButton(_("Join!"), '0 0 0', "join", COMMANDBUTTON_CLOSE));
	me.TR(me);
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Spectate"), '0 0 0', "spec", COMMANDBUTTON_CLOSE));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 1, e = makeXonoticLeaveMatchButton('0 0 0', 0));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 1, e = makeXonoticCommandButton(_("Quit"), '1 0 0', "menu_showquitdialog", 0));
}
