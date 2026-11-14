// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_uib_touch.qh"
#include "dialog_touch_join.qh"

// Godnotic UI bridge: the join popup. Xonotic's join dialog (auto-select, the
// teams the server offers, spectate) as a description, so a native renderer
// shows it as its own dialog: id "join", a modal in the game context.
// UIB_JoinOpen is what "menu_cmd directmenu TeamSelect" becomes when a native
// renderer draws the menu (TouchGameMenu_Route); the engine sends that for an
// observer when a match starts and from Join game / Change team. Without a
// native renderer the dialog is dialog_touch_join.qc.
// Each button's action is "join.<choice>" and runs "touch_join <choice>".

bool uib_join_watching;   // opened for a player who was watching: closes once they play

void UIB_BuildJoin()
{
	if (!UIB_Screen("join", _("Join game")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 1);
	float teams = cvar("_teams_available");
	bool teamgame = (cvar("_touch_hud_flags") & 4) || teams;
	if (teamgame)
		uib_s("subtitle", _("Pick a team, or let the game pick"));
	uib_j("primary", UIB_AS("join.auto", (teamgame ? _("Auto-select team") : _("Join")), "play", "primary"));
	if (teams)
	{
		UIB_Open("teams", "grid");
			uib_f("minItem", 140);
			if (teams & 1) UIB_Button("red", _("Red"), UIB_A("join.red", _("Red"), "team"), "secondary");
			if (teams & 2) UIB_Button("blue", _("Blue"), UIB_A("join.blue", _("Blue"), "team"), "secondary");
			if (teams & 4) UIB_Button("yellow", _("Yellow"), UIB_A("join.yellow", _("Yellow"), "team"), "secondary");
			if (teams & 8) UIB_Button("pink", _("Pink"), UIB_A("join.pink", _("Pink"), "team"), "secondary");
		UIB_Close();
	}
	UIB_Button("spectate", _("Spectate"), UIB_A("join.spectate", _("Spectate"), "esp"), "secondary");
	UIB_Done();
}

bool UIB_JoinUp()
{
	return strstrofs(strcat(" ", uibn_modal, " "), " join ", 0) >= 0;
}

void UIB_JoinClose()
{
	if (UIB_TopScreen() == "join")
	{
		UIB_DropLast(true);
		UIB_Nav();
	}
	// back to the match, not to the pause menu under the popup
	if (uibn_context == "game" && uibn_modal == "")
		m_hide();
}

// True when a bridge renderer shows the menu and the popup is now up.
bool UIB_JoinOpen()
{
	// A native renderer shows it as its own dialog. The QuakeC renderer leaves
	// it to the finger-sized stock dialog (dialog_touch_join.qc), which opens
	// over the bridge menu like the game menu does: its popups sit on the
	// full-screen pause page, and a join question belongs over the match.
	if (!UIB_EngineHasBridge() || !uib_native())
		return false;
	if (!uib_inited)
		UIB_Init();
	UIB_Tick();   // the context may still read "menu" or "loading" right after a join
	if (!uib_inited || uibn_context != "game")
		return false;
	uib_join_watching = TouchJoin_Watching();
	if (!UIB_JoinUp())
		UIB_Modal("join");
	else
		UIB_Build("join");
	if (!Menu_Active)
		m_display();
	return true;
}

bool UIB_JoinAction(string a)
{
	if (a == "join.open")
	{
		localcmd("\ntouch_join_dialog\n");
		return true;
	}
	if (substring(a, 0, 5) != "join." || a == "join.address")
		return false;
	localcmd(sprintf("\ntouch_join %s\n", substring(a, 5, -1)));
	UIB_JoinClose();
	return true;
}

// Back or a tap outside: as Spectate while watching (the player stays where
// they are), and never the pause menu under the popup.
bool UIB_JoinBack()
{
	if (UIB_TopScreen() != "join")
		return false;
	UIB_JoinClose();
	return true;
}

// Every tick: the popup goes once the player is in the game.
void UIB_JoinTick()
{
	if (!UIB_JoinUp())
		return;
	if (uibn_context != "game" || (uib_join_watching && cvar_string("_touch_hud_mode") == "playing"))
	{
		uib_join_watching = false;
		UIB_JoinClose();
	}
}
