// SPDX-License-Identifier: GPL-3.0-or-later
#include "dialog_touch_launcher.qh"

#include "../item/modalcontroller.qh"
#include "touchbutton.qh"
#include "touch_menu_scale.qh"
#include "dialog_singleplayer.qh"
#include "dialog_multiplayer.qh"
#include "dialog_media.qh"
#include "dialog_settings.qh"
#include "dialog_touch_credits.qh"
#include "dialog_quit.qh"

// The top of the screen belongs to the banner (the menu background's top
// layer); the tiles take the rest.
const float TOUCH_LAUNCHER_TOP = 0.27;

void TouchLauncher_Open(entity btn, entity dlg)
{
	DialogOpenButton_Click(btn, dlg);
}

void TouchLauncher_OpenLan(entity btn, entity dlg)
{
	cvar_set("net_slist_lan_only", "1");
	DialogOpenButton_Click(btn, dlg);
}

void TouchLauncher_Goto(entity btn, entity me)
{
	m_goto(btn.cvarName, false);
}

entity TouchLauncher_Tile(string label, vector color, float big)
{
	entity e = makeGodnoticButton(label, color);
	e.fontSize = big ? SKINFONTSIZE_TITLE * 1.4 : SKINFONTSIZE_NORMAL * 1.5;
	return e;
}

entity TouchLauncher_TileOpen(string label, vector color, float big, entity dlg)
{
	entity e = TouchLauncher_Tile(label, color, big);
	e.onClick = TouchLauncher_Open;
	e.onClickEntity = dlg;
	return e;
}

void GodnoticLauncherDialog_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 2, 2, e = TouchLauncher_TileOpen(_("Play"), '0.6 1 0.6', 1, me.singleplayerDialog));
			e.tooltip = _("Instant action against bots, or the campaign");
		me.TD(me, 2, 2, e = TouchLauncher_TileOpen(_("Multiplayer"), '0 0 0', 1, me.multiplayerDialog));
			e.tooltip = _("Servers on the internet and on this network");
		me.TD(me, 2, 2, e = TouchLauncher_TileOpen(_("Settings"), '0 0 0', 1, me.settingsDialog));
	me.TR(me);
	me.TR(me);
		me.TD(me, 2, 2, e = TouchLauncher_Tile(_("LAN games"), '0.55 1 0.55', 1));
			e.onClick = TouchLauncher_OpenLan;
			e.onClickEntity = me.multiplayerDialog;
			e.tooltip = _("Games on this Wi-Fi or network");
		me.TD(me, 2, 2, e = TouchLauncher_Tile(_("Host LAN"), '0.55 1 0.55', 1));
			e.cvarName = "TouchLanHost";
			e.onClick = TouchLauncher_Goto;
			e.onClickEntity = e;
			setDependent(e, "touch_lan_available", 1, 1); // not in the browser
		me.TD(me, 2, 2, e = TouchLauncher_Tile(_("Mods"), '0 0 0', 1));
			e.cvarName = "touchmods";
			e.onClick = TouchLauncher_Goto;
			e.onClickEntity = e;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 2, e = TouchLauncher_TileOpen(_("Media"), '0 0 0', 0, me.mediaDialog));
		me.TD(me, 1, 2, e = TouchLauncher_TileOpen(_("Credits"), '0 0 0', 0, me.creditsDialog));
		me.TD(me, 1, 2, e = TouchLauncher_TileOpen(_("Quit"), '1 0.6 0.6', 0, me.quitDialog));
}

entity TouchLauncher_AddSheet(entity mainwindow, entity i)
{
	i.configureDialog(i);
	mainwindow.addItemCentered(mainwindow, i, i.intendedWidth * eX + i.intendedHeight * eY, SKINALPHAS_MAINMENU_z);
	return i;
}

bool TouchLauncher_Configure(entity me)
{
	if (!touch_menu_on)
		return false;
	entity l = touch_launcher = NEW(GodnoticLauncherDialog);
	l.singleplayerDialog = TouchLauncher_AddSheet(me, NEW(XonoticSingleplayerDialog));
	l.multiplayerDialog = TouchLauncher_AddSheet(me, NEW(XonoticMultiplayerDialog));
	l.mediaDialog = TouchLauncher_AddSheet(me, NEW(XonoticMediaDialog));
	l.settingsDialog = TouchLauncher_AddSheet(me, NEW(XonoticSettingsDialog));
	l.creditsDialog = TouchLauncher_AddSheet(me, NEW(GodnoticCreditsDialog));
	l.quitDialog = TouchLauncher_AddSheet(me, NEW(XonoticQuitDialog));

	l.configureDialog(l);
	float top = TOUCH_LAUNCHER_TOP;
	float m = TouchMenuScale_dpToUnits(8) / conheight;
	me.addItem(me, l, eY * top, eX + eY * (1 - top - m), SKINALPHAS_MAINMENU_z);
	me.moveItemAfter(me, l, NULL); // first: what the menu goes back to when it hides
	// One sheet at a time: what is behind the front sheet (the launcher, a
	// sheet that opened this one) all but disappears instead of showing
	// through it at 30%.
	me.fadedAlpha = 0.02;
	me.mainNexposee = l;           // m_goto("nexposee") and the background follow it
	me.initializeDialog(me, l);
	return true;
}

void TouchLauncher_heal()
{
	if (!touch_launcher || (gamestatus & (GAME_CONNECTED | GAME_ISSERVER)))
		return;
	if (touch_launcher.ModalController_state == 0)
		main.showChild(main, touch_launcher, '0 0 0', '0 0 0', 1);
}
