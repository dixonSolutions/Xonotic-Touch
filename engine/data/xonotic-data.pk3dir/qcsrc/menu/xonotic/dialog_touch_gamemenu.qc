#include "dialog_touch_gamemenu.qh"

#include "touchbutton.qh"
#include "dialog_touch_lan.qh"
#include "leavematchbutton.qh"
#include "touch_menu_scale.qh"
#include "dialog_touch_join.qh"
#include "touch_uib_touch.qh"

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
	// The Welcome dialog a newer server's game code opens on connect has
	// Join! and Spectate itself: the engine's own join dialog is then not
	// owed for this match (touch_hudmode.c).
	if (itemname == "Welcome" && (gamestatus & (GAME_ISSERVER | GAME_CONNECTED)))
		localcmd("\ntouch_join_answered\n");
	// Xonotic's join dialog (team_selection_show, the engine's
	// touch_join_dialog): the bridge's popup where a bridge renderer draws
	// the menu, else the finger-sized dialog on a touchscreen.
	// (Before menuInitialized too: with a native front end owning the menu
	// from the start the menu VM never draws, so it never initialises, and
	// m_goto would only remember the name. The popup needs none of it.)
	if (itemname == "TeamSelect")
	{
		// The popup is the bridge's own: the stock menu shows its root, in
		// whose place the bridge renderer draws (and which TouchUIB_Classic
		// does not take for a stock dialog).
		if (UIB_JoinOpen())
			return "nexposee";
		if (menuInitialized && TouchGameMenu_Wanted())
			return "TouchJoin";
	}
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

// Move to spectators: straight away, or, when the engine says it would forfeit
// the round or the objective carried (_touch_act_spectate 3), a second tap
// within 3 s.
.float touch_spec_armed;
void TouchGameMenu_Spectate(entity btn, entity me)
{
	if (cvar("_touch_act_spectate") == 3 && time >= btn.touch_spec_armed)
	{
		btn.touch_spec_armed = time + 3;
		btn.setText(btn, _("Tap again: you forfeit"));
		return;
	}
	btn.touch_spec_armed = 0;
	btn.setText(btn, _("Spectate"));
	localcmd("\ntouch_action spectate confirm\n");
}

void GodnoticGameMenuDialog_fill(entity me)
{
	entity e;
	// What a player came for is on the first screen of a phone (2340x1080):
	// Resume, then Join game / Spectate. Leave and Quit are the last row, which
	// the touch sheet pins to the bottom edge with the row above it
	// (touch_menu_sheet.qc), so they show however far the sheet is scrolled.
	me.TR(me);
		me.TD(me, 1.5, 2, e = makeGodnoticCommandButton(_("Resume"), '0 0 0', "togglemenu 0", COMMANDBUTTON_CLOSE));
	me.TR(me);
	me.TR(me);
		// Join and Move to spectators follow the engine's action list
		// (touch_hudmode.c, _touch_act_*: 0 hidden, 1 enabled, 2 waiting,
		// 3 asks first), the same list the native pause menus show.
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Join game"), '0 0 0', "touch_action join", 0));
			setDependent(e, "_touch_act_join", 1, 1);
		me.TD(me, 1.5, 1, e = makeGodnoticButton(_("Spectate"), '0 0 0'));
			e.onClick = TouchGameMenu_Spectate;
			e.onClickEntity = e;
			setDependent(e, "_touch_act_spectate", 1, 3);
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Change team"), '0 0 0', "touch_action team", 0));
			setDependent(e, "_touch_act_team", 1, 1);
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Settings"), "settings"));
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Mobile settings"), "touchsettings"));
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Quick menu"), '0 0 0', "quickmenu", COMMANDBUTTON_CLOSE));
	me.TR(me);
	me.TR(me);
		// Hosting on the LAN (Multiplayer > Create, LAN box): where others
		// join, and Stop.
		me.TD(me, 1.5, 1.4, TouchLan_StatusLabel());
		me.TD(me, 1.5, 0.6, e = TouchLan_StopButton());
	me.TR(me);
	me.TR(me);
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Servers"), "servers"));
		me.TD(me, 1.5, 1, e = TouchGameMenu_SubButton(_("Profile"), "profile"));
	me.TR(me);
	me.TR(me);
		// Never COMMANDBUTTON_CLOSE: that hides the menu while still connected.
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Leave to main menu"), '1 0.55 0.55', LEAVEMATCH_CMD, 0));
		me.TD(me, 1.5, 1, e = makeGodnoticCommandButton(_("Quit"), '1 0 0', "menu_showquitdialog", 0));
}
