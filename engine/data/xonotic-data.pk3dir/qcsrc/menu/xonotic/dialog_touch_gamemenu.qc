#include "dialog_touch_gamemenu.qh"

#include "touchbutton.qh"
#include "leavematchbutton.qh"
#include "touch_menu_scale.qh"

void GodnoticGameMenuDialog_configureDialog(entity me)
{
	// Its buttons span 1.5 and 2 rows, sized for the stock rows. With the
	// touch layer's 48 dp rows (touch_menu_scale.qc) a 30 dp row keeps the
	// 1.5-row buttons at 49 dp and the whole menu on a phone's screen.
	if (touch_menu_on)
		me.rowHeight = TouchMenuScale_dpToUnits(30 * TouchMenuScale_size());
	SUPER(GodnoticGameMenuDialog).configureDialog(me);
}

// Set while a dialog opened from TouchGameMenu is up.
bool touch_gamemenu_return;

bool TouchGameMenu_Wanted()
{
	return cvar("vid_touchscreen") || cvar("vid_touchscreen_detected");
}

string TouchGameMenu_Route(string itemname)
{
	if (itemname == "GameMenu" && TouchGameMenu_Wanted())
		return "TouchGameMenu";
	return itemname;
}

bool TouchGameMenu_Return(entity dialog)
{
	if (!touch_gamemenu_return || dialog.name == "TouchGameMenu")
		return false;
	touch_gamemenu_return = false;
	if (!(gamestatus & (GAME_ISSERVER | GAME_CONNECTED)))
		return false;
	m_goto("TouchGameMenu", true);
	return true;
}

void TouchGameMenu_Disconnected()
{
	touch_gamemenu_return = false;
	if (TouchGameMenu_Wanted())
		m_goto("nexposee", false);
}

void TouchGameMenu_Open(entity btn, entity me)
{
	touch_gamemenu_return = true;
	m_goto(btn.cvarName, false);
}

entity TouchGameMenu_SubButton(string label, string dialogname)
{
	entity e = makeGodnoticButton(label, '0 0 0');
	e.cvarName = dialogname; // the dialog to open, reusing an unused string field
	e.onClick = TouchGameMenu_Open;
	e.onClickEntity = e;
	return e;
}

void GodnoticGameMenuDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 2, 2, e = makeGodnoticCommandButton(_("Resume"), '0 0 0', "togglemenu 0", COMMANDBUTTON_CLOSE));
	me.TR(me);
	me.TR(me);
		// Never COMMANDBUTTON_CLOSE: that hides the menu while still connected.
		me.TD(me, 2, 2, e = makeGodnoticCommandButton(_("Leave to main menu"), '1 0.55 0.55', LEAVEMATCH_CMD, 0));
	me.TR(me);
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Join"), '0 0 0', "join", COMMANDBUTTON_CLOSE));
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Spectate"), '0 0 0', "spec", COMMANDBUTTON_CLOSE));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Settings"), "settings"));
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Mobile settings"), "touchsettings"));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Servers"), "servers"));
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Quick menu"), '0 0 0', "quickmenu", COMMANDBUTTON_CLOSE));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Profile"), "profile"));
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Quit"), '1 0 0', "menu_showquitdialog", 0));
	me.TR(me);
	me.TR(me);
		// Host a game for the LAN, or while hosting see who is in, the addresses and Stop.
		me.TD(me, 1.5, 2, e = TouchGameMenu_SubButton(_("LAN game: host, players, stop"), "TouchLanHost"));
			setDependent(e, "touch_lan_available", 1, 1);
}
