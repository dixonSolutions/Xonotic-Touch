// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_uib_touch.qh"

// Godnotic UI bridge: Xonotic's "use your name in player statistics?"
// question (the stock Uid2Name dialog) as a description, so a native renderer
// shows it as its own dialog: id "uid2name", a modal in the game context.
//
// The game code asks for it on a server that keeps statistics while
// cl_allow_uid2name is -1, and asks again on every frame the menu is not on
// screen (HUD_Vote: "menu_cmd directmenu Uid2Name"). With a native front end
// the stock dialog is never drawn, so the native screen showed the pause page
// instead, and Resume closed it only for the game code to open the menu again
// on the next frame: the pause page could not be left. Answering here sets
// the cvar, which ends the asking.

bool UIB_Uid2NameUp()
{
	return strstrofs(strcat(" ", uibn_modal, " "), " uid2name ", 0) >= 0;
}

void UIB_BuildUid2Name()
{
	if (!UIB_Screen("uid2name", _("Show your name in player statistics?")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 0);   // a choice is the only way out; Back answers No
	uib_s("subtitle", _("With No you appear as \"Anonymous player\". Settings > Privacy changes it later."));
	uib_j("primary", UIB_AS("uid2name.yes", _("Yes"), "check", "primary"));
	UIB_Button("no", _("No"), UIB_A("uid2name.no", _("No"), ""), "secondary");
	UIB_Done();
}

// True when a bridge renderer shows the menu and the question is up; called
// for every "menu_cmd directmenu Uid2Name" (TouchGameMenu_Route).
bool UIB_Uid2NameOpen()
{
	if (!UIB_EngineHasBridge() || !uib_native())
		return false;
	if (cvar("cl_allow_uid2name") != -1)
		return true;   // answered meanwhile: nothing to show, and no stock dialog either
	if (!UIB_Uid2NameUp())
	{
		if (!uib_inited)
			UIB_Init();
		UIB_Tick();   // the context may still read "loading" right after a join
		if (!uib_inited || uibn_context != "game")
			return false;
		UIB_Modal("uid2name");
	}
	if (!Menu_Active)
		m_display();
	return true;
}

void UIB_Uid2NameAnswer(bool yes)
{
	// the cvar first, so the game code's next frame does not ask again; then the
	// stock dialog's own commands (vyes/vno clear the vote panel's copy of the
	// question, setreport tells the server)
	cvar_set("cl_allow_uid2name", yes ? "1" : "0");
	localcmd(yes ? "\nvyes; setreport cl_allow_uid2name 1\n" : "\nvno; setreport cl_allow_uid2name 0\n");
	if (UIB_TopScreen() == "uid2name")
	{
		UIB_DropLast(true);
		UIB_Nav();
	}
	// back to the match, not to the pause menu under the question
	if (uibn_context == "game" && uibn_modal == "")
		m_hide();
}

bool UIB_Uid2NameAction(string a)
{
	if (a == "uid2name.yes") { UIB_Uid2NameAnswer(true); return true; }
	if (a == "uid2name.no") { UIB_Uid2NameAnswer(false); return true; }
	return false;
}

bool UIB_Uid2NameBack()
{
	if (UIB_TopScreen() != "uid2name")
		return false;
	UIB_Uid2NameAnswer(false);
	return true;
}
