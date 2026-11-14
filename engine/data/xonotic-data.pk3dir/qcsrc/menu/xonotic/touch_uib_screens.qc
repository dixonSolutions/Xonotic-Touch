// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_data.qh"
#include "touch_uib_screens.qh"
#include "touch_startup_util.qh"
#include "touch_uib_touch.qh" // Touch HUD: the join popup, Touch controls settings
#include <common/mapinfo.qh>
#include <common/campaign_common.qh>
#include <common/campaign_file.qh>
#include <common/campaign_setup.qh>

// Godnotic UI bridge: Home, Create (singleplayer and LAN hosting), map preview, Servers,
// server detail, password prompt, pause, loading, Credits, dialogs. Settings and Mods
// are in touch_uib_settings.qc. Designed for the bridge, not ports of the stock dialogs:
// each screen says what is on it and what can be done; renderers decide how it looks.

// ---------------------------------------------------------------- game modes

const int UIB_NMODES = 10;
string UIB_ModeId(int i)
{
	switch (i)
	{
		case 0: return "dm"; case 1: return "tdm"; case 2: return "ctf"; case 3: return "ca"; case 4: return "duel";
		case 5: return "ft"; case 6: return "dom"; case 7: return "ka"; case 8: return "kh"; case 9: return "lms";
	}
	return "dm";
}

string UIB_ModeName(string id)
{
	switch (id)
	{
		case "dm": return _("Deathmatch");
		case "tdm": return _("Team Deathmatch");
		case "ctf": return _("Capture the Flag");
		case "ca": return _("Clan Arena");
		case "duel": return _("Duel");
		case "ft": return _("Freeze Tag");
		case "dom": return _("Domination");
		case "ka": return _("Keepaway");
		case "kh": return _("Key Hunt");
		case "lms": return _("Last Man Standing");
		case "race": return _("Race");
		case "cts": return _("Race CTS");
		case "ons": return _("Onslaught");
		case "as": return _("Assault");
		case "nb": return _("Nexball");
		case "inv": return _("Invasion");
	}
	return strtoupper(id);
}

// A name that fits a card without breaking a word.
string UIB_ModeShort(string id)
{
	switch (id)
	{
		case "tdm": return _("Team DM");
		case "ctf": return _("CTF");
		case "lms": return _("Last Man");
	}
	return UIB_ModeName(id);
}

string UIB_ModeHelp(string id)
{
	switch (id)
	{
		case "dm": return _("Score as many frags as you can");
		case "tdm": return _("Help your team score the most frags");
		case "ctf": return _("Bring the enemy flag to your base");
		case "ca": return _("Wipe out the other team to win the round");
		case "duel": return _("One on one");
		case "ft": return _("Freeze the enemy team, thaw your own");
		case "dom": return _("Capture and hold the control points");
		case "ka": return _("Hold the ball to score");
		case "kh": return _("Gather all the keys to win the round");
		case "lms": return _("Survive until the others run out of lives");
	}
	return "";
}

string UIB_ModeNames(string ids)
{
	string out = "";
	int n = tokenize_console(ids);
	for (int i = 0; i < n && i < 4; ++i)
		out = strcat(out, (i ? ", " : ""), UIB_ModeName(argv(i)));
	if (n > 4)
		out = strcat(out, sprintf(_(" and %d more"), n - 4));
	return out;
}

// ---------------------------------------------------------------- navigation

bool UIB_Wanted()
{
	if (!UIB_EngineHasBridge())
		return false;
	return cvar("menu_touch_uib") || uib_native();
}

string UIB_TopScreen()
{
	string s = (uibn_modal != "") ? uibn_modal : uibn_stack;
	int n = tokenize_console(s);
	return n ? argv(n - 1) : "";
}

void UIB_Nav()
{
	int n = tokenize_console(uibn_stack);
	bool canback = n > 1 || uibn_modal != "" || uibn_context == "game";
	if (uibn_modal == "welcome") canback = false; // a name first (UIB_BuildWelcome)
	uib_nav(uibn_stack, uibn_modal, canback, uibn_context);
}

void UIB_Build(string id);

void UIB_SetStack(string stack, string modal)
{
	strcpy(uibn_stack, stack);
	strcpy(uibn_modal, modal);
	int n = tokenize_console(strcat(stack, " ", modal));
	for (int i = 0; i < n && i < 16; ++i)
		uib_ids[i] = strzone(argv(i));
	for (int i = 0; i < n && i < 16; ++i)
	{
		UIB_Build(uib_ids[i]);
		strunzone(uib_ids[i]);
	}
	UIB_Nav();
}

void UIB_Push(string id)
{
	if (UIB_TopScreen() == id)
	{
		UIB_Build(id);
		return;
	}
	UIB_Build(id);
	string s = strcat(uibn_stack, " ", id);
	strcpy(uibn_stack, s);
	UIB_Nav();
}

void UIB_Modal(string id)
{
	UIB_Build(id);
	string s = (uibn_modal == "") ? id : strcat(uibn_modal, " ", id);
	strcpy(uibn_modal, s);
	UIB_Nav();
}

void UIB_DropLast(bool modal)
{
	string s = modal ? uibn_modal : uibn_stack;
	int n = tokenize_console(s);
	string out = "";
	for (int i = 0; i < n - 1; ++i)
		out = strcat(out, (i ? " " : ""), argv(i));
	if (modal) strcpy(uibn_modal, out);
	else strcpy(uibn_stack, out);
}

bool uib_narrow;  // the QuakeC renderer's own layout is one column

void UIB_Back()
{
	if (UIB_JoinBack())
		return;
	if (uibn_modal == "" && UIB_TopScreen() == "settings" && UIB_VMf("settings.open")
		&& (uib_caps("widthClass") == "compact" || uib_narrow))
	{
		// one column: back from a section to the list of sections
		UIB_VMSet("settings.open", "0");
		UIB_RebuildAll();
		return;
	}
	if (uibn_modal == "welcome")
		return;
	if (uibn_modal != "")
		UIB_DropLast(true);
	else if (tokenize_console(uibn_stack) > 1)
		UIB_DropLast(false);
	else if (uibn_context == "game")
	{
		m_hide();
		return;
	}
	UIB_Nav();
}

void UIB_RebuildAll()
{
	int n = tokenize_console(strcat(uibn_stack, " ", uibn_modal));
	for (int i = 0; i < n && i < 16; ++i)
		uib_ids[i] = strzone(argv(i));
	for (int i = 0; i < n && i < 16; ++i)
	{
		UIB_Build(uib_ids[i]);
		strunzone(uib_ids[i]);
	}
	UIB_Nav();
}

// ---------------------------------------------------------------- home

string UIB_MapTitle(string map)
{
	string title = UIB_VM(strcat("map.title.", map));
	if (title == "") title = strcat(strtoupper(substring(map, 0, 1)), substring(map, 1, -1));
	return title;
}

// The campaign: the stock file loader (common/campaign_file.qc). Loaded when the
// menu context starts and when the Campaign screen opens; the stock campaign
// list reloads it for itself the same way.
int uib_camp_index;      // the next level to play (the first one not won)
bool uib_camp_loaded;
void UIB_CampaignLoad()
{
	strcpy(campaign_name, cvar_string("g_campaign_name"));
	string cv = strcat("g_campaign", campaign_name, "_index");
	registercvar(cv, "", 0);
	CampaignFile_Unload();
	CampaignFile_Load(0, CAMPAIGN_MAX_ENTRIES);
	uib_camp_index = bound(0, cvar(cv), max(0, campaign_entries - 1));
	uib_camp_loaded = true;
}

string UIB_CampaignLevelTitle(int i)
{
	string t = campaign_shortdesc[i];
	if (t == "") t = UIB_MapTitle(campaign_mapname[i]);
	return t;
}

void UIB_CampaignRows()
{
	uib_rows_begin("campaign");
	int n = min(uib_camp_index + 2, campaign_entries);
	for (int i = 0; i < n; ++i)
	{
		string state = (i < uib_camp_index) ? "done" : (i == uib_camp_index) ? "next" : "locked";
		uib_row(ftos(i), sprintf("{\"n\":%d,\"title\":%s,\"map\":%s,\"mode\":%s,\"modeName\":%s,\"state\":\"%s\",\"locked\":%s}",
			i + 1, uib_q(sprintf(_("%d. %s"), i + 1, UIB_CampaignLevelTitle(i))), uib_qv(campaign_mapname[i]), uib_qv(campaign_gametype[i]),
			uib_q(UIB_ModeName(campaign_gametype[i])), state, (i > uib_camp_index) ? "true" : "false"),
			strcat("pk3:maps/", campaign_mapname[i]));
	}
	uib_rows_end();
}

// "N servers · M players": the engine's own figure (_slist_summary, docs/NETWORK.md),
// filled from the saved list in the first frame and live from the first refresh on.
// _slist_refreshing says a refresh is running. True when either changed.
string uib_mp_summary;
float uib_mp_refreshing = -1;
bool UIB_ServerSummary()
{
	string out = cvar_string("_slist_summary");
	float r = cvar("_slist_refreshing");
	if (out == uib_mp_summary && r == uib_mp_refreshing)
		return false;
	strcpy(uib_mp_summary, out);
	uib_mp_refreshing = r;
	return true;
}

// Home: the banner and exactly three cards. Settings, Credits and Quit are the
// screen's actions (small, in the header), never cards.
void UIB_BuildHome()
{
	if (!UIB_Screen("home", _("Godnotic")))
		return;
	uib_s("kind", "home");
	uib_s("background", "asset:background");
	uib_push("actions", UIB_A("settings", _("Settings"), "settings"));
	uib_push("actions", UIB_A("credits", _("Credits"), "credits"));
	uib_push("actions", UIB_AC("quit", _("Quit"), "quit", "secondary", _("Quit Godnotic?"), _("Quit"), false));

	UIB_Open("banner", "image");
		uib_s("ref", "asset:banner");
		uib_s("role", "banner");
		uib_s("emphasis", "high");
		uib_s("aspect", "21:9");
		uib_s("fit", "contain");
		uib_s("alt", "Godnotic");
	UIB_Close();

	string map = cvar_string("touch_lan_map");
	if (map == "") map = "stormkeep";
	string mode = cvar_string("touch_lan_gametype");
	if (mode == "") mode = "dm";
	float bots = cvar("touch_lan_bots");
	if (!uib_camp_loaded)
		UIB_CampaignLoad();
	UIB_ServerSummary();
	// Home's cards are always one horizontal row, never stacked and never
	// scrolled down to. The compact breakpoint: a renderer too narrow for three
	// cards side by side (widthClass compact, under 600 dp) gets a row of two:
	// Play, with Singleplayer and Campaign as its two actions, beside
	// Multiplayer. Wider renderers get the row of three.
	bool compact = (uib_caps("widthClass") == "compact" || uib_narrow);
	UIB_Open("tiles", "grid");
		uib_f("minItem", (compact ? 150 : 200));
		uib_f("maxColumns", (compact ? 2 : 3));   // one row: every card of the grid side by side
		if (compact)
		{
			UIB_Open("play", "card");
				uib_s("title", _("Play"));
				if (campaign_entries > 0)
					uib_s("subtitle", sprintf(_("%s · Campaign level %d of %d"), UIB_MapTitle(map), uib_camp_index + 1, campaign_entries));
				else
					uib_s("subtitle", sprintf(_("%s · %s"), UIB_MapTitle(map), UIB_ModeName(mode)));
				uib_s("icon", "play");
				uib_s("image", strcat("pk3:maps/", map));
				uib_s("aspect", "16:9");
				uib_b("featured", 1);
				uib_j("primary", UIB_A("sp", _("Singleplayer"), "singleplayer"));
				uib_push("actions", UIB_A("sp", _("Singleplayer"), "singleplayer"));
				uib_push("actions", UIB_A("campaign", _("Campaign"), "trophy"));
			UIB_Close();
		}
		else
		{
		UIB_Open("sp", "card");
			uib_s("title", _("Singleplayer"));
			uib_s("subtitle", sprintf(_("%s · %s · %d bots"), UIB_MapTitle(map), UIB_ModeName(mode), bots));
			uib_s("icon", "singleplayer");
			uib_s("image", strcat("pk3:maps/", map));
			uib_s("aspect", "16:9");
			uib_b("featured", 1);
			uib_j("primary", UIB_A("sp", _("Open"), "chevron"));
		UIB_Close();
		UIB_Open("campaign", "card");
			uib_s("title", _("Campaign"));
			if (campaign_entries > 0)
			{
				uib_s("subtitle", sprintf(_("Level %d of %d · %s"), uib_camp_index + 1, campaign_entries, UIB_CampaignLevelTitle(uib_camp_index)));
				uib_s("image", strcat("pk3:maps/", campaign_mapname[uib_camp_index]));
				uib_f("progress", uib_camp_index / campaign_entries);
			}
			else
			{
				uib_s("subtitle", _("Fight your way through the levels"));
				uib_s("image", "pk3:maps/afterslime");
			}
			uib_s("icon", "trophy");
			uib_s("aspect", "16:9");
			uib_b("featured", 1);
			uib_j("primary", UIB_A("campaign", _("Open"), "chevron"));
		UIB_Close();
		}
		UIB_Open("mp", "card");
			uib_s("title", _("Multiplayer"));
			uib_s("subtitle", (uib_mp_summary != "") ? uib_mp_summary : _("Join a server, or create a game"));
			uib_s("icon", "multiplayer");
			uib_s("image", "pk3:maps/xoylent");
			uib_s("aspect", "16:9");
			uib_b("featured", 1);
			uib_b("live", 1);
			uib_j("primary", UIB_A("mp", _("Open"), "chevron"));
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

// ---------------------------------------------------------------- campaign

void UIB_BuildCampaign()
{
	if (!uib_camp_loaded)
		UIB_CampaignLoad();
	int sel = bound(0, UIB_VMf("campaign.sel"), min(uib_camp_index, max(0, campaign_entries - 1)));
	if (!UIB_Screen("campaign", _("Campaign")))
		return;
	uib_s("kind", "page");
	if (campaign_title != "") uib_s("subtitle", campaign_title);
	uib_j("header", "{\"collapsible\":true}");
	if (campaign_entries <= 0)
	{
		UIB_Open("none", "status");
			uib_s("icon", "trophy");
			uib_s("title", _("No campaign in this game data"));
		UIB_Close();
		UIB_Done();
		return;
	}
	uib_j("primary", UIB_AS("campaign.play", _("Play"), "play", "primary"));
	UIB_Open("sel", "preview");
		uib_s("title", sprintf(_("%d. %s"), sel + 1, UIB_CampaignLevelTitle(sel)));
		uib_s("subtitle", sprintf(_("%s on %s"), UIB_ModeName(campaign_gametype[sel]), UIB_MapTitle(campaign_mapname[sel])));
		uib_s("image", strcat("pk3:maps/", campaign_mapname[sel]));
		uib_s("aspect", "16:9");
		uib_s("density", "compact");
		uib_s("body", strreplace("\n", " ", campaign_longdesc[sel]));
		if (sel < uib_camp_index) uib_push("badges", UIB_Badge("check", _("Won"), "positive"));
		else uib_push("badges", UIB_Badge("flag", _("Next"), "accent"));
	UIB_Close();
	UIB_Section("levels", _("Levels"), true);
		uib_s("subtitle", sprintf(_("%d of %d won · tap to choose, double tap to play"), uib_camp_index, campaign_entries));
		UIB_Open("list", "list");
			uib_s("source", "campaign");
			uib_s("display", "cards");
			uib_s("rowHeight", "card");
			uib_f("minItem", 220);
			uib_s("aspectItem", "16:9");
			uib_s("state", "ready");
			uib_s("selection", "single");
			uib_j("selected", sprintf("[\"%d\"]", sel));
			uib_push("columns", UIB_Col("title", "text", _("Level"), "fill", 1, false, false));
			uib_push("columns", UIB_Col("modeName", "text", _("Mode"), "m", 2, false, false));
			uib_push("columns", UIB_Col("state", "text", _("State"), "s", 3, false, false));
			uib_j("rowSelect", UIB_A("campaign.select", _("Choose"), "check"));
			uib_j("rowPrimary", UIB_AS("campaign.play", _("Play"), "play", "primary"));
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

void UIB_StartCampaign(int n)
{
	if (n < 0 || n >= campaign_entries || n > uib_camp_index)
	{
		uib_toast(_("Win the level before this one first"), "warning", "lock", 3);
		return;
	}
	// a campaign level is a local game: never on the network
	localcmd("\nsv_public -1\nset sv_touch_lan_password \"\"\n");
	CampaignSetup(n);
}

// ---------------------------------------------------------------- create (singleplayer, LAN hosting)

// The map the picker shows as chosen: its text from the maps source.
string uib_map_title, uib_map_author, uib_map_desc, uib_map_modes;
void UIB_MapInfo(string map)
{
	strcpy(uib_map_title, map);
	strcpy(uib_map_author, "");
	strcpy(uib_map_desc, "");
	strcpy(uib_map_modes, "");
	if (uib_window("maps", 0, 8, "", 0, map) > 0)
		for (int i = 0; i < 8; ++i)
			if (uib_wkey(i) == map)
			{
				strcpy(uib_map_title, uib_wcell(i, "title"));
				strcpy(uib_map_author, uib_wcell(i, "author"));
				strcpy(uib_map_desc, uib_wcell(i, "description"));
				strcpy(uib_map_modes, uib_wcell(i, "modes"));
				break;
			}
	UIB_VMSet(strcat("map.title.", map), uib_map_title);
}

// Create is one screen (id "sp") used two ways. From Home's Singleplayer card it
// makes a local game: mode, map, bots; nothing about the network, and nobody
// else can find or join it. From Multiplayer's Create it also offers player
// slots and the LAN checkbox (Port, optional Password); unticked it is the same
// local game.
bool UIB_CreateFromMP() { return UIB_VM("create.from") == "mp"; }
bool UIB_CreateLan() { return UIB_CreateFromMP() && UIB_VMf("create.lan"); }

void UIB_BuildCreate()
{
	string mode = cvar_string("touch_lan_gametype");
	if (mode == "") mode = "dm";
	bool frommp = UIB_CreateFromMP();
	bool lan = UIB_CreateLan();
	bool pwon = UIB_VMf("create.pwon");
	bool pwset = cvar_string("touch_lan_host_password") != "" || cvar("touch_lan_host_password_set");
	string map = cvar_string("touch_lan_map");
	if (map == "") map = "stormkeep";
	UIB_MapInfo(map);
	if (!UIB_Screen("sp", frommp ? _("Create a game") : _("Singleplayer")))
		return;
	uib_s("kind", "page");
	uib_s("subtitle", lan ? _("On your network: others can join") : _("Local game: only you and the bots"));
	uib_j("header", "{\"collapsible\":true}");
	uib_j("primary", lan ? UIB_AS("play", _("Host on LAN"), "host", "primary") : UIB_AS("play", _("Play"), "play", "primary"));

	UIB_Section("modes", _("Game mode"), false);
		UIB_Open("grid", "grid");
			uib_s("scroll", "horizontal");
			uib_f("minItem", 140);
			for (int i = 0; i < UIB_NMODES; ++i)
			{
				string id = UIB_ModeId(i);
				UIB_Open(id, "card");
					uib_s("title", UIB_ModeShort(id));
					uib_s("a11y", UIB_ModeName(id));
					uib_s("subtitle", (UIB_ModeShort(id) != UIB_ModeName(id)) ? strcat(UIB_ModeName(id), ": ", UIB_ModeHelp(id)) : UIB_ModeHelp(id));
					uib_s("image", strcat("pk3:gfx/menu/luma/gametype_", id));
					uib_s("aspect", "1:1");
					if (id == mode) uib_b("selected", 1);
					uib_j("primary", UIB_A(strcat("mode.", id), UIB_ModeName(id), ""));
				UIB_Close();
			}
		UIB_Close();
	UIB_Close();

	UIB_Section("maps", _("Map"), true);
		uib_s("subtitle", _("Tap to choose, double tap to play"));
		UIB_Open("list", "list");
			string msrc = cvar_string("_uib_debug_maps");      // stress tests: a synthetic source
			uib_s("source", (msrc != "") ? msrc : strcat("maps:", mode));
			uib_s("display", "cards");
			uib_s("rowHeight", "card");
			// two full rows of tiles (picture and name) in view, more by scrolling sideways
			uib_f("rows", 2);
			uib_s("scroll", "horizontal");
			uib_f("minItem", 160);
			uib_s("aspectItem", "16:9");
			uib_s("state", "ready");
			uib_s("selection", "single");
			uib_j("selected", sprintf("[%s]", uib_qv(map)));
			uib_push("columns", UIB_Col("title", "text", _("Map"), "fill", 1, true, false));
			uib_push("columns", UIB_Col("author", "text", _("Author"), "m", 3, false, false));
			// one tap chooses (event row, kind select), a double tap plays (kind activate)
			uib_j("rowSelect", UIB_A("map.select", _("Choose"), "check"));
			uib_j("rowPrimary", UIB_AS("map.play", _("Play"), "play", "primary"));
			uib_push("rowActions", UIB_A("map.preview", _("Details"), "info"));
			uib_j("empty", sprintf("{\"title\":%s,\"icon\":\"map\"}", uib_q(_("No maps for this mode"))));
		UIB_Close();
	UIB_Close();

	UIB_Section("match", _("Match"), false);
		UIB_Open("form", "form");
			uib_s("density", "compact");
			UIB_Stepper("bots", _("Bots"), "cvar:touch_lan_bots", 0, 15, 1, "");
			UIB_Choice("skill", _("Bot skill"), "cvar:skill", "segmented", strcat("[", UIB_Opt("1", _("Easy")), ",", UIB_Opt("4", _("Normal")), ",",
				strcat(UIB_Opt("7", _("Hard")), ",", UIB_Opt("10", _("Godlike")), "]")), "");
			UIB_Choice("time", _("Time limit"), "cvar:timelimit_override", "dropdown", strcat("[", UIB_Opt("-1", _("Map default")), ",", UIB_Opt("5", _("5 minutes")), ",",
				strcat(UIB_Opt("10", _("10 minutes")), ",", UIB_Opt("20", _("20 minutes")), ",", UIB_Opt("0", _("No limit")), "]")), "");
			UIB_Choice("weapons", _("Weapons"), "vm:create.weapons", "dropdown", strcat("[", UIB_Opt("", _("Normal")), ",", UIB_Opt("instagib", _("InstaGib")), ",",
				strcat(UIB_Opt("nix", _("NIX")), ",", UIB_Opt("rockets", _("Rocket arena")), "]")), "");
			if (frommp)
			{
				UIB_Open("lan", "switch");
					uib_s("label", _("LAN"));
					uib_s("style", "checkbox");
					uib_s("icon", "lan");
					uib_s("bind", "vm:create.lan");
					uib_b("value", lan);
					uib_s("help", lan ? _("Others on your Wi-Fi or VPN can find and join this game") : _("Off: a local game nobody else can see or join"));
				UIB_Close();
				UIB_Open("slots", "stepper");
					uib_s("label", _("Player slots"));
					uib_s("bind", "cvar:touch_lan_maxplayers");
					uib_f("min", 2);
					uib_f("max", 32);
					uib_f("step", 1);
					uib_s("format", "int");
					if (!lan) uib_b("hidden", 1);
				UIB_Close();
				UIB_Open("port", "stepper");
					uib_s("label", _("Port"));
					uib_s("bind", "cvar:touch_lan_host_port");
					uib_f("min", 1024);
					uib_f("max", 65535);
					uib_f("step", 1);
					uib_s("format", "int");
					if (cvar("_touch_lan_port_free"))
						uib_s("help", sprintf(_("%d is free"), cvar("touch_lan_host_port")));
					else
					{
						uib_s("help", sprintf(_("In use; %d is free"), cvar("_touch_lan_port_suggest")));
						uib_s("error", _("Another game or app uses this port"));
					}
					if (!lan) uib_b("hidden", 1);
				UIB_Close();
				UIB_Open("pwon", "switch");
					uib_s("label", _("Password"));
					uib_s("bind", "vm:create.pwon");
					uib_b("value", pwon);
					bool locked = pwon && pwset;
					uib_s("icon", locked ? "lock" : "unlock");
					uib_s("help", locked ? _("Players need the password to join") : _("Anyone on your network can join"));
					if (!lan) uib_b("hidden", 1);
				UIB_Close();
				UIB_Open("password", "field");
					uib_s("label", _("Password"));
					uib_s("bind", "vm:create.password");
					uib_s("kind", "password");
					uib_b("private", 1);
					uib_b("hasValue", pwset);
					uib_f("maxLength", 64);
					if (!(lan && pwon)) uib_b("hidden", 1);
				UIB_Close();
			}
		UIB_Close();
	UIB_Close();

	UIB_Section("recent", _("Recent"), false);
		uib_b("collapsible", 1);
		uib_f("importance", 3);
		UIB_Open("list", "list");
			uib_s("source", "recent");
			uib_s("display", "rows");
			uib_s("rowHeight", "compact");
			uib_s("density", "compact");
			uib_s("state", "ready");
			uib_push("columns", UIB_Col("title", "map", _("Map"), "fill", 1, false, false));
			uib_push("columns", UIB_Col("modeName", "text", _("Mode"), "m", 2, false, false));
			uib_j("rowPrimary", UIB_A("recent.play", _("Play again"), "play"));
			uib_j("empty", sprintf("{\"title\":%s,\"icon\":\"timer\"}", uib_q(_("Nothing played yet"))));
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

void UIB_RecentRows()
{
	uib_rows_begin("recent");
	int n = tokenize_console(cvar_string("_uib_recent"));
	for (int i = 0; i < n && i < 8; ++i)
		uib_ids[i] = strzone(argv(i));
	for (int i = 0; i < n && i < 8; ++i)
	{
		tokenizebyseparator(uib_ids[i], ":");
		string map = argv(0), mode = argv(1);
		uib_row(uib_ids[i], sprintf("{\"map\":%s,\"mode\":%s,\"title\":%s,\"modeName\":%s}", uib_qv(map), uib_qv(mode), uib_q(map), uib_q(UIB_ModeName(mode))),
			strcat("pk3:maps/", map));
		strunzone(uib_ids[i]);
	}
	uib_rows_end();
}

void UIB_AddRecent(string map, string mode)
{
	string key = strcat(map, ":", mode);
	string out = key;
	int n = tokenize_console(cvar_string("_uib_recent"));
	for (int i = 0, k = 1; i < n && k < 6; ++i)
		if (argv(i) != key)
		{
			out = strcat(out, " ", argv(i));
			++k;
		}
	registercvar("_uib_recent", "", 1);
	cvar_set("_uib_recent", out);
	UIB_RecentRows();
}

void UIB_StartGame(string map, string mode)
{
	if (TouchData_MapDownloading(map))
	{
		// still coming in (docs/DATA_FETCH.md): fetch it first, never start another map instead
		TouchData_Want(map);
		uib_toast(sprintf(_("%s is still downloading. It is next; start it when it is here."), map), "info", "download", 5);
		return;
	}
	bool lan = UIB_CreateLan();
	Gametype gt = MapInfo_Type_FromString(mode, false, false);
	if (!gt)
		gt = MAPINFO_TYPE_DEATHMATCH;
	MapInfo_FilterGametype(gt, 0, MapInfo_RequiredFlags(), MapInfo_ForbiddenFlags(), 0);
	if (MapInfo_count <= 0)
	{
		uib_toast(_("No map you have supports this game mode"), "error", "error", 4);
		return;
	}
	string m = "";
	for (int i = 0; i < MapInfo_count; ++i)
		if (MapInfo_BSPName_ByID(i) == map)
			m = map;
	if (m == "")
		m = MapInfo_BSPName_ByID(floor(random() * MapInfo_count));
	int bots = bound(0, cvar("touch_lan_bots"), 32);
	// a local game has the slots its bots need; only a LAN game has more
	int slots = lan ? bound(2, max(cvar("touch_lan_maxplayers"), bots + 2), 64) : max(2, bots + 1);
	if (lan)
	{
		float port = cvar("touch_lan_host_port");
		cvar_set("_touch_lan_portcheck", ftos(port));
		if (!cvar("_touch_lan_port_free"))
		{
			cvar_set("touch_lan_host_port", cvar_string("_touch_lan_port_suggest"));
			uib_toast(sprintf(_("Port %d is in use; switched to %d. Tap Host again."), port, cvar("_touch_lan_port_suggest")), "warning", "warning", 5);
			UIB_RebuildAll();
			return;
		}
	}
	localcmd("\ndisconnect; wait; g_campaign 0\n");
	// a local game drops its player straight in; a hosted one lets people watch
	localcmd(lan ? "sv_spectate 1\n" : "sv_spectate 0\n");
	// ... and a game started here needs no "Welcome, Join or Spectate?" dialog
	// (the client QuakeC asks for it only while the menu says it has one)
	cvar_set("_menu_welcome_dialog_available", "0");
	localcmd(sprintf("maxplayers %d\n", slots));
	localcmd(sprintf("bot_number %d\n", bots));
	string w = UIB_VM("create.weapons");
	localcmd(sprintf("g_instagib %d; g_nix %d; g_weaponarena %s\n", w == "instagib", w == "nix", (w == "rockets") ? "devastator" : "0"));
	if (lan)
	{
		localcmd(sprintf("port %d\n", cvar("touch_lan_host_port")));
		localcmd("sv_public 0\nsv_status_privacy 1\n");
		localcmd(sprintf("hostname \"%s\"\n", strreplace("\"", "'", cvar_string("touch_lan_hostname"))));
		localcmd(sprintf("set sv_touch_assist %d\n", bound(0, cvar("touch_lan_assist"), 7)));
		if (!UIB_VMf("create.pwon"))
			cvar_set("touch_lan_host_password", "");
		// arms the engine: this map command opens the network port (touch_lan.c)
		localcmd("touch_lan_host_password_apply\n");
	}
	else
		// local: the engine opens no network socket for it (touch_lan_private);
		// sv_public -1 as well, so even with that switch off it answers nobody
		localcmd("sv_public -1\nset sv_touch_lan_password \"\"\n");
	cvar_set("touch_lan_map", m);
	cvar_set("touch_lan_gametype", mode);
	UIB_AddRecent(m, mode);
	MapInfo_SwitchGameType(gt);
	MapInfo_LoadMap(m, 1);
}

void UIB_BuildMapPreview()
{
	string map = UIB_VM("map.sel");
	if (map == "") map = "stormkeep";
	string mode = cvar_string("touch_lan_gametype");
	if (mode == "") mode = "dm";
	UIB_MapInfo(map);
	if (!UIB_Screen("sp.map", uib_map_title))
		return;
	uib_s("kind", "detail");
	UIB_Open("preview", "preview");
		uib_s("title", uib_map_title);
		uib_s("subtitle", UIB_ModeNames(uib_map_modes));
		uib_s("image", strcat("pk3:maps/", map));
		uib_s("aspect", "16:9");
		uib_s("body", uib_map_desc);
		if (uib_map_author != "") uib_push("attrs", UIB_Attr(_("Author"), uib_map_author, "credits"));
		uib_push("attrs", UIB_Attr(_("Modes"), UIB_ModeNames(uib_map_modes), "mode"));
		uib_j("primary", UIB_AS("map.play", sprintf(_("Play %s"), UIB_ModeName(mode)), "play", "primary"));
		uib_push("actions", UIB_A("map.choose", _("Use this map"), "check"));
	UIB_Close();
	UIB_Done();
}

// ---------------------------------------------------------------- servers

void UIB_ServerParams()
{
	uib_param("servers", "lan", UIB_VMf("mp.lan") ? "1" : "0");
	uib_param("servers", "empty", cvar_string("menu_slist_showempty"));
	uib_param("servers", "full", cvar_string("menu_slist_showfull"));
	uib_param("servers", "laggy", cvar_string("menu_slist_showlaggy"));
	uib_param("servers", "search", UIB_VM("mp.search"));
}

void UIB_BuildServers()
{
	UIB_ServerParams();
	bool lan = UIB_VMf("mp.lan");
	// what Join joins: the typed address if there is one, else the chosen row
	string sel = UIB_VM("mp.server");
	string typed = UIB_VM("mp.address");
	if (typed != "") sel = "";
	if (!UIB_Screen("mp", _("Multiplayer")))
		return;
	uib_s("kind", "page");
	UIB_ServerSummary();
	if (uib_mp_summary != "")
		uib_s("subtitle", cvar("_slist_refreshing") ? sprintf(_("%s · refreshing…"), uib_mp_summary) : uib_mp_summary);
	else if (cvar("_slist_refreshing"))
		uib_s("subtitle", _("Looking for servers…"));
	uib_j("header", "{\"collapsible\":true}");
	UIB_Open("bar", "toolbar");
		UIB_Open("search", "field");
			uib_s("label", _("Search"));
			uib_s("bind", "vm:mp.search");
			uib_s("value", UIB_VM("mp.search"));
			uib_s("kind", "search");
			uib_s("placeholder", _("Search servers"));
		UIB_Close();
		UIB_Open("filters", "row");
			uib_b("wrap", 1);
			UIB_Check("lan", _("LAN"), "vm:mp.lan", "lan");
			UIB_Check("empty", _("Empty"), "cvar:menu_slist_showempty", "");
			UIB_Check("full", _("Full"), "cvar:menu_slist_showfull", "");
			UIB_Check("laggy", _("Laggy"), "cvar:menu_slist_showlaggy", "");
			UIB_Button("refresh", _("Refresh"), UIB_A("refresh", _("Refresh"), "refresh"), "secondary");
		UIB_Close();
	UIB_Close();
	UIB_Open("servers", "list");
		string src = cvar_string("_uib_debug_servers");   // stress tests: a synthetic source
		uib_s("source", (src != "") ? src : "servers");
		uib_push("columns", UIB_Col("lock", "lock", "", "xs", 2, false, false));
		uib_push("columns", UIB_Col("name", "text", _("Server"), "fill", 1, true, false));
		uib_push("columns", UIB_Col("map", "map", _("Map"), "m", 2, true, false));
		uib_push("columns", UIB_Col("mode", "mode", _("Mode"), "s", 2, true, false));
		uib_push("columns", UIB_Col("players", "players", _("Players"), "s", 1, true, true));
		uib_push("columns", UIB_Col("ping", "ping", _("Ping"), "s", 1, true, true));
		string sort = UIB_VM("mp.sort");
		uib_s("sort", sort == "" ? "ping" : sort);
		uib_b("desc", UIB_VMf("mp.desc"));
		uib_s("filter", UIB_VM("mp.search"));
		// one tap chooses a server, the one Join button (or a double tap) joins it;
		// rows are text only: no per-row buttons, no page behind a row
		uib_s("selection", "single");
		if (sel != "") uib_j("selected", sprintf("[%s]", uib_qv(sel)));
		uib_s("density", "compact");
		uib_s("display", "table");
		uib_s("rowHeight", "compact");
		uib_s("state", "ready");
		uib_b("live", 1);
		uib_j("rowSelect", UIB_A("server.select", _("Choose"), "check"));
		uib_j("rowPrimary", UIB_AS("server.join", _("Join"), "play", "primary"));
		if (lan)
			uib_j("empty", sprintf("{\"title\":%s,\"text\":%s,\"icon\":\"lan\"}", uib_q(_("No games on this network")),
				uib_q(_("Games hosted on your Wi-Fi or VPN show up here."))));
		else
			uib_j("empty", sprintf("{\"title\":%s,\"text\":%s,\"icon\":\"server\",\"action\":%s}", uib_q(_("No servers yet")),
				uib_q(_("Check your connection, then Refresh.")), UIB_A("refresh", _("Refresh"), "refresh")));
	UIB_Close();
	UIB_Open("join", "actionbar");
		uib_s("align", "spread");
		UIB_Open("address", "field");
			uib_s("label", _("Address"));
			uib_s("bind", "vm:mp.address");
			uib_s("value", UIB_VM("mp.address"));
			uib_s("kind", "address");
			uib_s("placeholder", _("host:port or godnotic://join/…"));
			uib_j("submit", UIB_A("join", _("Join"), "play"));
		UIB_Close();
		UIB_Button("create", _("Create"), UIB_A("create", _("Create"), "add"), "secondary");
		// the one Join: off until a server is chosen or an address typed
		UIB_Open("go", "button");
			uib_s("label", _("Join"));
			uib_s("style", "primary");
			uib_j("action", UIB_AS("join", _("Join"), "play", "primary"));
			if (sel == "" && typed == "") uib_b("enabled", 0);
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

// one server's row (cells read with uib_wcell(0, ...)), "" if not known
bool UIB_FindServer(string address)
{
	int n = uib_window("servers.all", 0, 16, "", 0, address);
	for (int i = 0; i < n; ++i)
		if (uib_wkey(i) == address)
		{
			if (i)
				uib_window("servers.all", i, 1, "", 0, address);
			return true;
		}
	return false;
}

void UIB_BuildPassword()
{
	if (!UIB_Screen("mp.password", _("Password")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 1);
	// a button as well as Enter: the dialog's main action (renderers put it in the action bar)
	uib_j("primary", UIB_AS("password.join", _("Join"), "play", "primary"));
	UIB_Text("text", sprintf(_("%s needs a password."), UIB_VM("mp.joining")), "");
	UIB_Open("form", "form");
		uib_j("submit", UIB_AS("password.join", _("Join"), "play", "primary"));
		UIB_Open("pw", "field");
			uib_s("label", _("Password"));
			uib_s("bind", "vm:join.password");
			uib_s("kind", "password");
			uib_b("private", 1);
			uib_b("hasValue", 0);
			uib_j("submit", UIB_A("password.join", _("Join"), ""));
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

// ---------------------------------------------------------------- welcome

// What the welcome would save as the name: the typed text without the
// characters a name cannot carry, "" while there is nothing usable.
string UIB_WelcomeName()
{
	string n = strreplace("\"", "", strreplace("\\", "", strreplace("$", "", strreplace(";", "", UIB_VM("welcome.name")))));
	n = strreplace("\n", "", strreplace("\r", "", n));
	string plain = strtolower(strreplace(" ", "", strdecolorize(n)));
	if (plain == "" || plain == "player" || plain == "unnamedplayer")
		return "";
	return substring(n, 0, 32);
}

// First start: the name other players see, before anything else. It cannot be
// dismissed; Settings > Player changes it later (TouchStartup_nameNeeded).
void UIB_BuildWelcome()
{
	if (!UIB_Screen("welcome", _("Welcome to Godnotic")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 0);
	// off until there is a name to save
	string done = sprintf("{\"id\":\"welcome.done\",\"label\":%s,\"icon\":\"check\",\"style\":\"primary\",\"enabled\":%s}",
		uib_q(_("Continue")), (UIB_WelcomeName() == "") ? "false" : "true");
	uib_j("primary", done);
	UIB_Text("text", _("Pick the name other players see. You can change it later in Settings > Player."), "");
	UIB_Open("form", "form");
		uib_j("submit", done);
		UIB_Open("name", "field");
			uib_s("label", _("Name"));
			uib_s("bind", "vm:welcome.name");
			uib_s("kind", "name");
			uib_s("placeholder", _("Your name"));
			uib_f("maxLength", 32);
			// no submit of its own: renderers then send each change, and Continue turns on as you type
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

void UIB_WelcomeDone()
{
	string n = UIB_WelcomeName();
	if (n == "")
	{
		uib_toast(_("Type a name first"), "info", "info", 3);
		return;
	}
	cvar_set("_cl_name", n);
	localcmd(sprintf("\nname \"%s\"\nseta touch_profile_done 1\nsaveconfig\n", n));
	UIB_VMSet("welcome.name", "");
	if (uibn_modal == "welcome")
		strcpy(uibn_modal, "");
	UIB_RebuildAll();
}

void UIB_Join(string addr)
{
	if (substring(addr, 0, 16) == "godnotic://join/")
		addr = substring(addr, 16, -1);
	addr = strreplace("\"", "", strreplace(";", "", addr));
	if (addr == "")
		return;
	UIB_VMSet("mp.joining", addr);
	if (UIB_FindServer(addr) && uib_wcell(0, "lock") == "1")
	{
		cvar_set("_touch_lan_haspw_query", addr);
		if (!cvar("_touch_lan_haspw"))
		{
			UIB_Modal("mp.password");
			return;
		}
	}
	localcmd(sprintf("\nconnect \"%s\"\n", addr));
}

void UIB_Refresh()
{
	cvar_set("net_slist_lan_only", UIB_VMf("mp.lan") ? "1" : "0");
	refreshhostcache(true);
}

// ---------------------------------------------------------------- pause, loading, credits, dialogs

// touch_lan_address_labels is "label|address;label|address" (touch_lan.c); the
// addresses land in uib_ids[8..], their labels in uib_ids2[8..]. Returns how many.
int UIB_HostAddresses()
{
	if (cvar_string("touch_lan_address_labels") == "")
		return 0;
	int n = min(4, tokenizebyseparator(cvar_string("touch_lan_address_labels"), ";"));
	for (int i = 0; i < n; ++i)
		uib_ids[8 + i] = strzone(argv(i));
	for (int i = 0; i < n; ++i)
	{
		tokenizebyseparator(uib_ids[8 + i], "|");
		string lab = argv(0), addr = argv(1);
		strunzone(uib_ids[8 + i]);
		uib_ids[8 + i] = strzone(addr);
		uib_ids2[8 + i] = strzone(lab);
	}
	return n;
}

void UIB_HostAddressesFree(int n)
{
	for (int i = 0; i < n; ++i)
	{
		strunzone(uib_ids[8 + i]);
		strunzone(uib_ids2[8 + i]);
	}
}

bool UIB_HostLocked()
{
	return cvar("touch_lan_locked") || cvar("touch_lan_host_password_set");
}

void UIB_BuildPause()
{
	if (!UIB_Screen("pause", _("Paused")))
		return;
	uib_s("kind", "game");
	uib_s("presentation", "fullscreen");
	bool hosting = cvar("touch_lan_hosting");
	if (hosting)
	{
		bool pw = UIB_HostLocked();
		UIB_Open("host", "card");
			uib_s("title", _("Hosting on your network"));
			uib_s("subtitle", _("Others join with one of these addresses"));
			uib_s("icon", "host");
			uib_s("accent", "ember");
			uib_s("density", "compact");
			uib_push("badges", pw ? UIB_Badge("lock", _("Password set"), "warning") : UIB_Badge("unlock", _("Open"), "neutral"));
			int n = UIB_HostAddresses();
			for (int i = 0; i < n; ++i)
				uib_push("meta", UIB_Chip(((substring(uib_ids2[8 + i], 0, 3) == "VPN") ? "vpn" : "wifi"), strcat(uib_ids2[8 + i], " ", uib_ids[8 + i])));
			uib_j("primary", UIB_A("host.popup", _("Addresses"), "host"));
			if (n)
			{
				uib_push("actions", UIB_AText("host.copy", _("Copy address"), "copy", "copy", uib_ids[8]));
				uib_push("actions", UIB_AText("host.share", _("Share"), "share", "share", cvar_string("touch_lan_join_link")));
			}
			UIB_HostAddressesFree(n);
		UIB_Close();
	}
	UIB_Open("match", "card");
		if (gamestatus & GAME_ISSERVER)
		{
			// this device's game: its map and mode, with the map's picture
			string pm = cvar_string("touch_lan_map");
			string pmode = cvar_string("touch_lan_gametype");
			if (cvar("g_campaign") && campaign_entries > 0)
			{
				pm = campaign_mapname[uib_camp_index];
				pmode = campaign_gametype[uib_camp_index];
			}
			uib_s("title", UIB_MapTitle(pm));
			uib_s("subtitle", sprintf(_("%s · %s"), UIB_ModeName(pmode), hosting ? _("your LAN game") : _("your local game")));
			uib_s("image", strcat("pk3:maps/", pm));
		}
		else
		{
			uib_s("title", (cvar_string("_uib_server_name") != "") ? cvar_string("_uib_server_name") : _("Match"));
			uib_s("subtitle", _("Online game"));
		}
		uib_s("aspect", "21:9");
	UIB_Close();
	UIB_Open("actions", "grid");
		uib_f("minItem", 200);
		UIB_Button("resume", _("Resume"), UIB_AS("resume", _("Resume"), "resume", "primary"), "primary");
		UIB_Button("settings", _("Settings"), UIB_A("settings", _("Settings"), "settings"), "secondary");
		// the engine's action list (_touch_act_*): Join game while watching, Spectate while playing
		if (cvar("_touch_act_join"))
			UIB_Button("join", _("Join game"), UIB_A("join.open", _("Join game"), "play"), "secondary");
		if (cvar("_touch_act_team"))
			UIB_Button("team", _("Change team"), UIB_A("join.open", _("Change team"), "team"), "secondary");
		// a local game has its player in it (sv_spectate 0): nothing to watch from outside
		if (cvar("_touch_act_spectate") && (hosting || !(gamestatus & GAME_ISSERVER)))
			UIB_Button("spectate", _("Spectate"), ((cvar("_touch_act_spectate") == 3)
				? UIB_AC("spectate", _("Spectate"), "esp", "secondary", _("Spectate? You give up this round or what you carry."), _("Spectate"), false)
				: UIB_A("spectate", _("Spectate"), "esp")), "secondary");
		UIB_Open("disconnect", "button");
			// the host leaving ends a LAN game for everyone in it
			uib_s("label", hosting ? _("End the game") : _("Leave match"));
			uib_s("style", "destructive");
			if (hosting)
				uib_j("action", UIB_AC("disconnect", _("End the game"), "disconnect", "destructive", _("End the game for everyone?"), _("End"), true));
			else
				uib_j("action", UIB_AC("disconnect", _("Leave match"), "disconnect", "destructive", _("Leave the match?"), _("Leave"), true));
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

// The popup a LAN host gets when the game starts, and again from the pause
// menu's hosting card: the addresses to read out or copy, the lock, who is in,
// and whether it closes by itself when the first player joins.
void UIB_BuildLanPopup()
{
	if (!UIB_Screen("lan.host", _("Your LAN game")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 1);
	uib_s("subtitle", _("Others on your Wi-Fi or VPN join with one of these"));
	uib_j("primary", UIB_AS("host.close", _("Close"), "check", "primary"));
	bool pw = UIB_HostLocked();
	int n = UIB_HostAddresses();
	UIB_Section("addresses", _("Addresses"), false);
		if (!n)
			UIB_Text("none", _("No network is up. Connect to Wi-Fi or a VPN and the address shows here."), "warning");
		for (int i = 0; i < n; ++i)
		{
			UIB_Open(sprintf("a%d", i), "row");
				uib_s("align", "spread");
				uib_s("label", uib_ids2[8 + i]);
				uib_s("icon", (substring(uib_ids2[8 + i], 0, 3) == "VPN") ? "vpn" : "wifi");
				UIB_Open("text", "text");
					uib_s("body", uib_ids[8 + i]);
					uib_s("style", "mono");
					uib_b("selectable", 1);
				UIB_Close();
				UIB_Button("copy", _("Copy"), UIB_AText("host.copy", _("Copy"), "copy", "copy", uib_ids[8 + i]), "secondary");
			UIB_Close();
		}
		string link = cvar_string("touch_lan_join_link");
		if (link != "")
		{
			UIB_Open("link", "row");
				uib_s("align", "spread");
				uib_s("label", _("Link"));
				uib_s("icon", "link");
				UIB_Open("text", "text");
					uib_s("body", link);
					uib_s("style", "mono");
					uib_b("selectable", 1);
				UIB_Close();
				UIB_Button("copy", _("Copy"), UIB_AText("host.copylink", _("Copy"), "copy", "copy", link), "secondary");
				UIB_Button("share", _("Share"), UIB_AText("host.share", _("Share"), "share", "share", link), "secondary");
			UIB_Close();
		}
	UIB_Close();
	UIB_HostAddressesFree(n);
	UIB_Open("lock", "text");
		uib_s("body", pw ? _("Password set: players need it to join") : _("No password: anyone on your network can join"));
		uib_s("icon", pw ? "lock" : "unlock");
		uib_s("tone", pw ? "warning" : "neutral");
		uib_s("style", "note");
	UIB_Close();
	UIB_Open("players", "text");
		float others = max(0, cvar("touch_lan_playercount") - 1);
		uib_s("body", others ? sprintf(_("In the game: %s"), cvar_string("touch_lan_players")) : _("Nobody has joined yet"));
		uib_s("icon", "players");
		uib_s("style", "caption");
		uib_b("live", 1);
	UIB_Close();
	UIB_Switch("autoclose", _("Close when someone joins"), "cvar:touch_lan_popup_autoclose", _("This popup goes away by itself when the first player joins"));
	UIB_Done();
}

float uib_lanpopup_others, uib_lanpopup_wait;
bool uib_lanpopup_auto;   // the popup now up opened by itself (closing it then also closes the menu)
bool uib_lanpopup_seen;   // shown once for the game being hosted
bool UIB_LanPopupUp()
{
	return strstrofs(strcat(" ", uibn_modal, " "), " lan.host ", 0) >= 0;
}

void UIB_LanPopupClose(bool resume)
{
	if (UIB_TopScreen() == "lan.host")
	{
		UIB_DropLast(true);
		UIB_Nav();
	}
	if (resume && uibn_context == "game" && uibn_modal == "")
		m_hide();
	uib_lanpopup_auto = false;
}

// Every tick while in a game: open the popup once when a LAN game this device
// hosts is up, close it when the first player joins if the switch says so.
void UIB_LanPopupTick()
{
	if (uibn_context != "game" || !cvar("touch_lan_hosting"))
	{
		if (uibn_context == "menu")
			uib_lanpopup_seen = false;
		return;
	}
	// the addresses come with the host's first status (touch_lan.c, twice a
	// second); without a network there are none, and the popup says so
	if (!uib_lanpopup_seen && (cvar_string("touch_lan_address_labels") != "" || time > uib_lanpopup_wait))
	{
		uib_lanpopup_seen = true;
		uib_lanpopup_auto = !Menu_Active;
		uib_lanpopup_others = max(0, cvar("touch_lan_playercount") - 1);
		if (!UIB_LanPopupUp())
			UIB_Modal("lan.host");
		if (!Menu_Active)
			m_display();
		return;
	}
	if (UIB_LanPopupUp())
	{
		float others = max(0, cvar("touch_lan_playercount") - 1);
		if (others > uib_lanpopup_others && cvar("touch_lan_popup_autoclose") && UIB_TopScreen() == "lan.host")
		{
			uib_toast(_("A player joined your game"), "success", "players", 3);
			UIB_LanPopupClose(uib_lanpopup_auto);
		}
		uib_lanpopup_others = others;
	}
}


void UIB_BuildLoading()
{
	tokenize_console(uib_loadinfo());
	string state = argv(0);
	float progress = stof(argv(2)), speed = stof(argv(3));
	string file = argv(4);
	string stage;
	switch (state)
	{
		case "connecting": stage = _("Connecting"); break;
		case "downloading": stage = sprintf(_("Downloading %s"), file); break;
		default: stage = _("Loading the map"); break;
	}
	string detail = "";
	if (state == "downloading" && speed > 0)
		detail = sprintf(_("%d%% · %s/s"), floor(progress * 100), (speed >= 1048576) ? sprintf("%.1f MB", speed / 1048576) : sprintf("%d KB", floor(speed / 1024)));
	uib_busy(sprintf("{\"label\":%s,\"progress\":%s,\"indeterminate\":%s,\"cancel\":%s}", uib_q(stage), ftos(bound(0, progress, 1)),
		((state == "downloading") ? "false" : "true"), UIB_A("loading.cancel", _("Cancel"), "close")));
	if (!UIB_Screen("loading", _("Loading")))
		return;
	uib_s("kind", "loading");
	uib_s("presentation", "fullscreen");
	uib_s("background", "asset:loading");
	UIB_Open("status", "status");
		uib_s("icon", state == "downloading" ? "download" : "server");
		uib_s("title", (cvar_string("_uib_server_name") != "") ? sprintf(_("Joining %s"), cvar_string("_uib_server_name")) : _("Joining"));
		uib_s("body", stage);
		uib_b("live", 1);
		if (state == "downloading") uib_f("progress", bound(0, progress, 1));
		else uib_b("indeterminate", 1);
		uib_j("primary", UIB_AS("loading.cancel", _("Cancel"), "close", "secondary"));
	UIB_Close();
	UIB_Open("progress", "progress");
		uib_s("label", stage);
		if (state == "downloading") uib_f("value", bound(0, progress, 1));
		else uib_b("indeterminate", 1);
		if (detail != "") uib_s("detail", detail);
		uib_b("live", 1);
		uib_j("cancel", UIB_A("loading.cancel", _("Cancel"), "close"));
	UIB_Close();
	UIB_Done();
}

void UIB_BuildCredits()
{
	if (!UIB_Screen("credits", _("Credits")))
		return;
	uib_s("kind", "page");
	UIB_Section("godnotic", "Godnotic", false);
		UIB_Text("text", _("Godnotic is a touch-first build of Xonotic for phones, tablets, Linux and the web."), "");
	UIB_Close();
	UIB_Section("xonotic", "Xonotic", false);
		UIB_Text("text", _("Xonotic is made by the Xonotic team and its many contributors."), "");
	UIB_Close();
	UIB_Section("licence", _("Licence"), false);
		UIB_Text("text", _("The engine and game code are free software under the GNU General Public License, version 2 or later. Game data is under its authors' licences (see each pack's licence file)."), "caption");
		UIB_Button("source", _("Source code"), UIB_ALink("credits.source", _("Source code"), "link", "https://github.com/dixonSolutions/Godnotic"), "link");
	UIB_Close();
	UIB_Done();
}

void UIB_BuildJoinFail()
{
	if (!UIB_Screen("dialog.joinfail", _("Could not join")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 1);
	uib_j("primary", UIB_AS("joinfail.retry", _("Retry"), "refresh", "primary"));
	UIB_Open("text", "text");
		uib_s("body", cvar_string("_join_fail_text"));
		uib_s("style", "title");
		uib_s("icon", "warning");
	UIB_Close();
	if (cvar_string("_join_fail_detail") != "")
		UIB_Text("detail", cvar_string("_join_fail_detail"), "");
	UIB_Open("buttons", "row");
		uib_s("align", "end");
		UIB_Button("close", _("Close"), "{\"id\":\"dialog.cancel\",\"label\":\"\",\"kind\":\"back\"}", "secondary");
	UIB_Close();
	UIB_Done();
}

float uib_jf_serial = -1;
// A join failed (the serial counts up): the dialog, over whatever the menu shows.
void UIB_JoinFailTick()
{
	// this menu shows the reason itself; the classic dialog stays shut
	if (cvar("touch_joinfail_dialog"))
		cvar_set("touch_joinfail_dialog", "0");
	float serial = cvar("_join_fail_serial");
	if (uib_jf_serial < 0)
		uib_jf_serial = serial;
	if (serial == uib_jf_serial)
		return;
	uib_jf_serial = serial;
	if (strstrofs(strcat(" ", uibn_modal, " "), " dialog.joinfail ", 0) >= 0)
		UIB_Build("dialog.joinfail");
	else
		UIB_Modal("dialog.joinfail");
	if (!Menu_Active)
		m_display();
}

void UIB_BuildQuit()
{
	if (!UIB_Screen("dialog.quit", _("Quit Godnotic?")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 1);
	UIB_Text("text", _("Your settings are saved."), "");
	UIB_Open("buttons", "row");
		uib_s("align", "end");
		UIB_Button("cancel", _("Cancel"), "{\"id\":\"dialog.cancel\",\"label\":\"\",\"kind\":\"back\"}", "secondary");
		UIB_Button("ok", _("Quit"), UIB_AS("quit.confirm", _("Quit"), "quit", "destructive"), "destructive");
	UIB_Close();
	UIB_Done();
}

void UIB_Build(string id)
{
	switch (id)
	{
		case "home": UIB_BuildHome(); return;
		case "sp": UIB_BuildCreate(); return;
		case "sp.map": UIB_BuildMapPreview(); return;
		case "campaign": UIB_BuildCampaign(); return;
		case "lan.host": UIB_BuildLanPopup(); return;
		case "join": UIB_BuildJoin(); return;
		case "mp": UIB_BuildServers(); return;
		case "mp.password": UIB_BuildPassword(); return;
		case "welcome": UIB_BuildWelcome(); return;
		case "pause": UIB_BuildPause(); return;
		case "loading": UIB_BuildLoading(); return;
		case "credits": UIB_BuildCredits(); return;
		case "dialog.quit": UIB_BuildQuit(); return;
		case "dialog.joinfail": UIB_BuildJoinFail(); return;
	}
	if (substring(id, 0, 8) == "settings" || substring(id, 0, 4) == "mods")
		UIB_BuildSettings(id);
}

// ---------------------------------------------------------------- context: menu, game, loading

void UIB_Context()
{
	tokenize_console(uib_loadinfo());
	string st = argv(0);
	string ctx = "menu";
	if (st == "connecting" || st == "downloading" || st == "loading")
		ctx = "loading";
	else if (st == "game" || (gamestatus & (GAME_CONNECTED | GAME_ISSERVER)))
		ctx = "game";
	if (ctx == uibn_context && uibn_stack != "")
	{
		if (ctx == "loading")
			UIB_BuildLoading();
		return;
	}
	strcpy(uibn_context, ctx);
	if (ctx != "loading")
		uib_busy("");
	if (ctx == "game")
		uib_lanpopup_wait = time + 3;
	if (ctx == "menu")
	{
		cvar_set("_menu_welcome_dialog_available", "1");   // a server joined next gets its welcome dialog
		// back from a match: the campaign may have moved on
		UIB_CampaignLoad();
		UIB_CampaignRows();
	}
	if (ctx == "loading") UIB_SetStack("loading", "");
	else if (ctx == "game") UIB_SetStack("pause", "");
	else UIB_SetStack("home", TouchStartup_nameNeeded() ? "welcome" : "");
	if (ctx == "menu" && cvar("_touch_lan_ended"))
	{
		// the host of the LAN game we were in left or quit (touch_lan.c)
		cvar_set("_touch_lan_ended", "0");
		uib_toast(_("The host ended the game"), "warning", "info", 8);
	}
}

void UIB_Init()
{
	if (uib_inited || !UIB_EngineHasBridge())
		return;
	uib_inited = true;
	uib_reset();
	registercvar("_uib_server_name", "", 0);
	registercvar("_uib_debug_open", "", 0);
	registercvar("_uib_debug_toast", "", 0);
	registercvar("_uib_debug_servers", "", 0);
	registercvar("_uib_debug_maps", "", 0);
	UIB_RecentRows();
	UIB_CampaignLoad();
	UIB_CampaignRows();
	strcpy(uibn_context, "");
	UIB_Context();
}

float uib_next_tick, uib_mp_next_refresh; // real time: the menu VM's `time` stands still while a native front end hides the menu
void UIB_Tick()
{
	if (!uib_inited)
		return;
	UIB_Context();
	// test hooks (scripts/test-uibridge.sh, screenshot tours): open a modal, show a toast
	string dbg = cvar_string("_uib_debug_open");
	if (dbg != "")
	{
		cvar_set("_uib_debug_open", "");
		UIB_Modal(dbg);
	}
	dbg = cvar_string("_uib_debug_toast");
	if (dbg != "")
	{
		cvar_set("_uib_debug_toast", "");
		uib_toast(dbg, "success", "check", 4);
	}
	UIB_LanPopupTick();
	UIB_JoinFailTick();
	UIB_JoinTick();
	// time-varying screens
	string top = UIB_TopScreen();
	if ((top == "home" || top == "mp") && uib_gettime(1) > uib_next_tick)
	{
		uib_next_tick = uib_gettime(1) + 0.5;
		if (UIB_ServerSummary())
			UIB_Build(top);
		// while Multiplayer shows: a refresh every 10 s keeps the list live (the engine throttles it)
		if (top == "mp" && uib_gettime(1) > uib_mp_next_refresh)
		{
			uib_mp_next_refresh = uib_gettime(1) + 10;
			refreshhostcache(false);
		}
	}
	if ((top == "pause" || top == "lan.host" || top == "mods.detail" || top == "settings.data") && uib_gettime(1) > uib_next_tick)
	{
		uib_next_tick = uib_gettime(1) + 1;
		UIB_Build(top);
	}
}

// ---------------------------------------------------------------- events

void UIB_Action(string node, string a, string payload)
{
	if (substring(a, 0, 5) == "mode.")
	{
		cvar_set("touch_lan_gametype", substring(a, 5, -1));
		UIB_RebuildAll();
		return;
	}
	if (UIB_SettingsAction(node, a, payload))
		return;
	if (UIB_JoinAction(a))
		return;
	switch (a)
	{
		case "sp": UIB_VMSet("create.from", "home"); UIB_Push("sp"); return;
		case "campaign":
			UIB_CampaignLoad();
			UIB_CampaignRows();
			UIB_VMSet("campaign.sel", ftos(uib_camp_index));
			UIB_Push("campaign");
			return;
		case "campaign.play": UIB_StartCampaign(bound(0, UIB_VMf("campaign.sel"), uib_camp_index)); return;
		case "map.preview": UIB_VMSet("map.sel", cvar_string("touch_lan_map")); UIB_Push("sp.map"); return;
		case "host.popup": uib_lanpopup_auto = false; if (!UIB_LanPopupUp()) UIB_Modal("lan.host"); return;
		case "host.close": UIB_LanPopupClose(true); return;
		case "host.copy": case "host.copylink": case "host.share": return;   // the renderer copied or shared the text
		case "create": UIB_VMSet("create.from", "mp"); UIB_Push("sp"); return;
		case "mp": UIB_VMSet("mp.server", ""); UIB_Push("mp"); UIB_Refresh(); uib_mp_next_refresh = uib_gettime(1) + 10; return;
		case "credits": UIB_Push("credits"); return;
		case "quit": localcmd("\nquit\n"); return;   // the renderer already confirmed
		case "quit.confirm": localcmd("\nquit\n"); return;
		case "dialog.cancel": UIB_Back(); return;
		case "joinfail.retry":
			if (UIB_TopScreen() == "dialog.joinfail") UIB_Back();
			localcmd("\njoin_retry\n");
			return;
		case "play":
		{
			string map = cvar_string("touch_lan_map");
			UIB_StartGame(map, cvar_string("touch_lan_gametype"));
			return;
		}
		case "quick": UIB_StartGame("", cvar_string("touch_lan_gametype")); return;
		case "map.play": UIB_StartGame(UIB_VM("map.sel"), cvar_string("touch_lan_gametype")); return;
		case "map.choose":
			cvar_set("touch_lan_map", UIB_VM("map.sel"));
			if (UIB_TopScreen() == "sp.map") UIB_Back();
			UIB_RebuildAll();
			return;
		case "refresh": UIB_Refresh(); return;
		case "join": case "join.address": case "server.join":
		{
			// the one Join: the typed address, else the chosen server
			string target = (UIB_VM("mp.address") != "") ? UIB_VM("mp.address") : UIB_VM("mp.server");
			if (target == "")
				uib_toast(_("Choose a server or type an address"), "info", "info", 3);
			else
				UIB_Join(target);
			return;
		}
		case "welcome.done": UIB_WelcomeDone(); return;
		case "password.join":
		{
			string addr = UIB_VM("mp.joining");
			cvar_set("_cl_touch_lan_password_server", addr);
			UIB_Back();
			localcmd(sprintf("\nconnect \"%s\"\n", addr));
			return;
		}
		case "resume": m_hide(); return;
		case "spectate": localcmd("\ntouch_action spectate confirm\n"); m_hide(); return;
		case "disconnect": localcmd("\ndisconnect\n"); return;
		// disconnect alone does not stop a join the server has not answered yet
		case "loading.cancel": localcmd("\ndisconnect\n_touch_cancel_join\n"); return;
	}
}

void UIB_RowEvent(string node, string row, string json)
{
	if (UIB_SettingsRow(node, row, json))
		return;
	bool isaction = strstrofs(json, "\"action\"", 0) >= 0;
	switch (node)
	{
		case "sp/maps/list":
		{
			// one tap chooses, a double tap plays; an activate on a map that is not
			// the chosen one is taken as choosing it, so a renderer that knows only
			// "tap = activate" never starts a game with one tap
			string cur = cvar_string("touch_lan_map");
			if (isaction)
			{
				UIB_VMSet("map.sel", row);
				TouchData_Want(row); // a map still downloading goes to the front of the queue
				if (strstrofs(json, "map.preview", 0) >= 0) { UIB_Push("sp.map"); return; }
				cvar_set("touch_lan_map", row);
				UIB_RebuildAll();
				return;
			}
			if (strstrofs(json, "\"kind\":\"activate\"", 0) >= 0 && row == cur)
			{
				UIB_StartGame(row, cvar_string("touch_lan_gametype"));
				return;
			}
			UIB_VMSet("map.sel", row);
			cvar_set("touch_lan_map", row);
			UIB_RebuildAll();
			return;
		}
		case "campaign/levels/list":
		{
			int lvl = stof(row);
			if (lvl > uib_camp_index)
			{
				uib_toast(_("Win the level before this one first"), "warning", "lock", 3);
				return;
			}
			if (strstrofs(json, "\"kind\":\"activate\"", 0) >= 0 && lvl == UIB_VMf("campaign.sel"))
			{
				UIB_StartCampaign(lvl);
				return;
			}
			UIB_VMSet("campaign.sel", ftos(lvl));
			UIB_RebuildAll();
			return;
		}
		case "sp/recent/list":
			tokenizebyseparator(row, ":");
			string m = argv(0), g = argv(1);
			UIB_StartGame(m, g);
			return;
		case "mp/servers":
		{
			// one tap chooses, a double tap joins; an activate on a row that is not
			// the chosen one only chooses it (as in the map picker)
			bool chosen = (row == UIB_VM("mp.server") && UIB_VM("mp.address") == "");
			if (strstrofs(json, "\"kind\":\"activate\"", 0) >= 0 && chosen)
			{
				UIB_Join(row);
				return;
			}
			UIB_VMSet("mp.server", row);
			UIB_VMSet("mp.address", "");
			UIB_RebuildAll();
			return;
		}
	}
}

void UIB_ValueEvent(string node, string value, string json)
{
	if (UIB_SettingsValue(node, value))
		return;
	switch (node)
	{
		case "sp/match/form/lan": UIB_VMSet("create.lan", value); break;
		case "sp/match/form/pwon": UIB_VMSet("create.pwon", value); if (value == "0") cvar_set("touch_lan_host_password", ""); break;
		case "sp/match/form/password": cvar_set("touch_lan_host_password", value); UIB_VMSet("create.pwon", "1"); break;
		case "sp/match/form/port": cvar_set("_touch_lan_portcheck", value); break;
		case "sp/match/form/weapons": UIB_VMSet("create.weapons", value); break;
		case "mp/bar/search": UIB_VMSet("mp.search", value); break;
		case "mp/bar/filters/lan": UIB_VMSet("mp.lan", value); UIB_Refresh(); break;
		case "mp/join/address": UIB_VMSet("mp.address", value); break;   // a typed address is the target, not the row
		case "mp.password/form/pw": cvar_set("_cl_touch_lan_password", value); break;
		default:
			// cvar binds were applied by the engine; vm binds keep their value
			if (substring(json, 0, 1) != "")
			{
				string b = uib_get(node, "bind");
				if (substring(b, 0, 3) == "vm:")
					UIB_VMSet(substring(b, 3, -1), value);
			}
			break;
	}
	UIB_ServerParams();
	UIB_RebuildAll();
}

float UIB_Event(string ev, string node, string arg, string json)
{
	if (!UIB_EngineHasBridge())
		return 0;
	if (!uib_inited)
		UIB_Init();
	switch (ev)
	{
		case "attach": UIB_RebuildAll(); return 1;
		case "detach": return 1;
		case "caps": UIB_RebuildAll(); return 1;
		case "tick": UIB_Tick(); return 1;
		case "back": UIB_Back(); return 1;
		case "action": UIB_Action(node, arg, json); return 1;
		case "value": UIB_ValueEvent(node, arg, json); return 1;
		case "submit":
			UIB_ValueEvent(node, arg, json);
			if (node == "mp/join/address" && arg != "") UIB_Join(arg);
			if (node == "mp.password/form/pw") UIB_Action(node, "password.join", "");
			if (node == "welcome/form/name") UIB_Action(node, "welcome.done", "");
			return 1;
		case "row": UIB_RowEvent(node, arg, json); return 1;
		case "dismiss":
			if (node == "lan.host" && UIB_TopScreen() == "lan.host") UIB_LanPopupClose(uib_lanpopup_auto);
			else if (strstrofs(strcat(" ", uibn_modal, " "), strcat(" ", node, " "), 0) >= 0) UIB_Back();
			return 1;
		case "shown": if (node == "mp") UIB_Refresh(); return 1;
		case "hidden": return 1;
		case "changed": UIB_RebuildAll(); return 1;
		case "modpages": UIB_RebuildAll(); return 1;
	}
	return 0;
}
