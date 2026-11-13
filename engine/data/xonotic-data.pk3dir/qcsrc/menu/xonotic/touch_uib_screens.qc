// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_uib_screens.qh"
#include <common/mapinfo.qh>

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
	if (uibn_modal == "" && UIB_TopScreen() == "settings" && UIB_VMf("settings.open")
		&& (uib_caps("widthClass") == "compact" || uib_narrow))
	{
		// one column: back from a section to the list of sections
		UIB_VMSet("settings.open", "0");
		UIB_RebuildAll();
		return;
	}
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

void UIB_BuildHome()
{
	if (!UIB_Screen("home", _("Godnotic")))
		return;
	uib_s("kind", "home");
	uib_s("background", "asset:background");
	uib_push("actions", UIB_AC("quit", _("Quit"), "quit", "secondary", _("Quit Godnotic?"), _("Quit"), false));

	UIB_Open("banner", "image");
		uib_s("ref", "asset:banner");
		uib_s("aspect", "21:9");
		uib_s("fit", "contain");
		uib_s("alt", "Godnotic");
	UIB_Close();

	string map = cvar_string("touch_lan_map");
	if (map == "") map = "stormkeep";
	string mode = cvar_string("touch_lan_gametype");
	if (mode == "") mode = "dm";
	float bots = cvar("touch_lan_bots");
	string title = UIB_VM(strcat("map.title.", map));
	if (title == "") title = strcat(strtoupper(substring(map, 0, 1)), substring(map, 1, -1));
	UIB_Open("hero", "hero");
		uib_s("title", _("Play"));
		uib_s("subtitle", sprintf(_("Continue: %s · %s · %d bots"), title, UIB_ModeName(mode), bots));
		uib_s("image", strcat("pk3:maps/", map));
		uib_s("aspect", "16:9");
		uib_b("featured", 1);
		uib_push("meta", UIB_Chip("map", title));
		uib_push("meta", UIB_Chip("mode", UIB_ModeName(mode)));
		uib_push("meta", UIB_Chip("bot", sprintf(_("%d bots"), bots)));
		uib_j("primary", UIB_AS("play", _("Play"), "play", "primary"));
		uib_push("actions", UIB_A("sp", _("Choose…"), "map"));
	UIB_Close();

	UIB_Open("tiles", "grid");
		uib_f("minItem", 200);
		UIB_Open("sp", "card");
			uib_s("title", _("Singleplayer"));
			uib_s("subtitle", _("Bots, your rules"));
			uib_s("icon", "singleplayer");
			uib_s("image", "pk3:gfx/menu/luma/gametype_dm");
			uib_s("aspect", "16:9");
			uib_j("primary", UIB_A("sp", _("Open"), "chevron"));
		UIB_Close();
		UIB_Open("mp", "card");
			uib_s("title", _("Multiplayer"));
			uib_s("subtitle", _("Find a game or join by address"));
			uib_s("icon", "multiplayer");
			uib_s("image", "pk3:gfx/menu/luma/gametype_ctf");
			uib_s("aspect", "16:9");
			uib_j("primary", UIB_A("mp", _("Open"), "chevron"));
		UIB_Close();
		UIB_Open("settings", "card");
			uib_s("title", _("Settings"));
			uib_s("subtitle", _("Video, controls, Mods"));
			uib_s("icon", "settings");
			uib_j("primary", UIB_A("settings", _("Open"), "chevron"));
		UIB_Close();
		UIB_Open("credits", "card");
			uib_s("title", _("Credits"));
			uib_s("subtitle", _("The people behind Xonotic and Godnotic"));
			uib_s("icon", "credits");
			uib_j("primary", UIB_A("credits", _("Open"), "chevron"));
		UIB_Close();
	UIB_Close();
	UIB_Done();
}

// ---------------------------------------------------------------- create (singleplayer, LAN hosting)

void UIB_BuildCreate()
{
	string mode = cvar_string("touch_lan_gametype");
	if (mode == "") mode = "dm";
	bool lan = UIB_VMf("create.lan");
	bool pwon = UIB_VMf("create.pwon");
	bool pwset = cvar_string("touch_lan_host_password") != "" || cvar("touch_lan_host_password_set");
	if (!UIB_Screen("sp", UIB_VM("create.from") == "mp" ? _("Create") : _("Singleplayer")))
		return;
	uib_s("kind", "page");
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
					uib_s("title", UIB_ModeName(id));
					uib_s("subtitle", UIB_ModeHelp(id));
					uib_s("image", strcat("pk3:gfx/menu/luma/gametype_", id));
					uib_s("aspect", "1:1");
					if (id == mode) uib_b("selected", 1);
					uib_j("primary", UIB_A(strcat("mode.", id), UIB_ModeName(id), ""));
				UIB_Close();
			}
		UIB_Close();
	UIB_Close();

	string map = cvar_string("touch_lan_map");
	UIB_Section("maps", _("Map"), true);
		uib_s("subtitle", _("Tap a map to see it"));
		UIB_Open("list", "list");
			string msrc = cvar_string("_uib_debug_maps");      // stress tests: a synthetic source
			uib_s("source", (msrc != "") ? msrc : strcat("maps:", mode));
			uib_s("display", "cards");
			uib_s("rowHeight", "card");
			uib_f("minItem", 220);
			uib_s("aspectItem", "16:9");
			uib_s("state", "ready");
			uib_s("selection", "single");
			if (map != "") uib_j("selected", sprintf("[%s]", uib_qv(map)));
			uib_push("columns", UIB_Col("title", "text", _("Map"), "fill", 1, true, false));
			uib_push("columns", UIB_Col("author", "text", _("Author"), "m", 3, false, false));
			uib_j("rowPrimary", UIB_A("map.preview", _("Preview"), "info"));
			uib_push("rowActions", UIB_A("map.choose", _("Use this map"), "check"));
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
			UIB_Stepper("slots", _("Player slots"), "cvar:touch_lan_maxplayers", 2, 32, 1, "");
			UIB_Open("lan", "switch");
				uib_s("label", _("LAN"));
				uib_s("style", "checkbox");
				uib_s("icon", "lan");
				uib_s("bind", "vm:create.lan");
				uib_b("value", lan);
				uib_s("help", _("Host on your network (Wi-Fi or VPN) so others can join"));
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
	bool lan = UIB_VMf("create.lan");
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
	int slots = bound(2, max(cvar("touch_lan_maxplayers"), bots + 1), 64);
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
		slots = max(slots, bots + 2);
	}
	localcmd("\ndisconnect; wait; g_campaign 0\n");
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
		localcmd("touch_lan_host_password_apply\n");
	}
	else
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
	// the map's own text from the maps source
	string title = map, author = "", desc = "", modes = "";
	if (uib_window("maps", 0, 8, "", 0, map) > 0)
		for (int i = 0; i < 8; ++i)
			if (uib_wkey(i) == map)
			{
				title = uib_wcell(i, "title");
				author = uib_wcell(i, "author");
				desc = uib_wcell(i, "description");
				modes = uib_wcell(i, "modes");
				break;
			}
	UIB_VMSet(strcat("map.title.", map), title);
	if (!UIB_Screen("sp.map", title))
		return;
	uib_s("kind", "detail");
	UIB_Open("preview", "preview");
		uib_s("title", title);
		uib_s("subtitle", UIB_ModeNames(modes));
		uib_s("image", strcat("pk3:maps/", map));
		uib_s("aspect", "16:9");
		uib_s("body", desc);
		if (author != "") uib_push("attrs", UIB_Attr(_("Author"), author, "credits"));
		uib_push("attrs", UIB_Attr(_("Modes"), UIB_ModeNames(modes), "mode"));
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
	if (!UIB_Screen("mp", _("Servers")))
		return;
	uib_s("kind", "page");
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
		uib_s("selection", "none");
		uib_s("density", "compact");
		uib_s("display", "table");
		uib_s("rowHeight", "compact");
		uib_s("state", "ready");
		uib_j("rowPrimary", UIB_A("server.open", _("Details"), "info"));
		uib_push("rowActions", UIB_A("server.join", _("Join"), "play"));
		uib_push("rowActions", UIB_A("server.fav", _("Favourite"), "star"));
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
			uib_j("submit", UIB_A("join.address", _("Join"), "play"));
		UIB_Close();
		UIB_Button("create", _("Create"), UIB_A("create", _("Create"), "add"), "secondary");
		UIB_Button("go", _("Join"), UIB_AS("join.address", _("Join"), "play", "primary"), "primary");
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

void UIB_BuildServer()
{
	string addr = UIB_VM("mp.server");
	string name = addr, map = "", mode = "", ver = "";
	float players = 0, mx = 0, bots = 0, ping = 0;
	bool lock = false, fav = false;
	if (UIB_FindServer(addr))
	{
		name = uib_wcell(0, "name");
		map = uib_wcell(0, "map");
		mode = uib_wcell(0, "mode");
		ver = uib_wcell(0, "version");
		players = stof(uib_wcell(0, "players"));
		mx = stof(uib_wcell(0, "max"));
		bots = stof(uib_wcell(0, "bots"));
		ping = stof(uib_wcell(0, "ping"));
		lock = uib_wcell(0, "lock") == "1";
		fav = uib_wcell(0, "fav") == "1";
	}
	if (!UIB_Screen("mp.server", name))
		return;
	uib_s("kind", "detail");
	uib_j("header", "{\"collapsible\":true}");
	UIB_Open("preview", "preview");
		uib_s("title", name);
		uib_s("subtitle", sprintf(_("%s on %s"), UIB_ModeName(mode), map));
		if (map != "") uib_s("image", strcat("pk3:maps/", map));
		uib_s("aspect", "16:9");
		uib_push("attrs", UIB_Attr(_("Players"), sprintf(_("%d/%d (%d bots)"), players, mx, bots), "players"));
		uib_push("attrs", UIB_Attr(_("Ping"), sprintf(_("%d ms"), ping), "ping"));
		uib_push("attrs", UIB_Attr(_("Address"), addr, "server"));
		if (ver != "") uib_push("attrs", UIB_Attr(_("Version"), ver, "info"));
		if (lock) uib_push("badges", UIB_Badge("lock", _("Password"), "warning"));
		if (fav) uib_push("badges", UIB_Badge("star_filled", _("Favourite"), "accent"));
		uib_j("primary", UIB_AS("server.join", _("Join"), "play", "primary"));
		uib_push("actions", UIB_A("server.fav", (fav ? _("Remove favourite") : _("Favourite")), (fav ? "star_filled" : "star")));
		uib_push("actions", UIB_AText("server.copy", _("Copy address"), "copy", "copy", addr));
	UIB_Close();
	UIB_Section("players", _("Players"), true);
		UIB_Open("list", "list");
			uib_s("source", strcat("players:", addr));
			uib_s("density", "compact");
			uib_s("display", "table");
			uib_s("rowHeight", "compact");
			uib_s("state", "ready");
			uib_push("columns", UIB_Col("name", "text", _("Player"), "fill", 1, false, false));
			uib_push("columns", UIB_Col("score", "number", _("Score"), "s", 1, true, true));
			uib_push("columns", UIB_Col("ping", "ping", _("Ping"), "s", 2, false, true));
			uib_j("empty", sprintf("{\"title\":%s,\"icon\":\"players\"}", uib_q(_("Nobody is playing"))));
		UIB_Close();
	UIB_Close();
	UIB_Open("bar", "actionbar");
		uib_s("align", "end");
		UIB_Button("fav", (fav ? _("Remove favourite") : _("Favourite")), UIB_A("server.fav", _("Favourite"), "star"), "secondary");
		UIB_Button("join", _("Join"), UIB_AS("server.join", _("Join"), "play", "primary"), "primary");
	UIB_Close();
	UIB_Done();
}

void UIB_BuildPassword()
{
	if (!UIB_Screen("mp.password", _("Password")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 1);
	UIB_Text("text", sprintf(_("%s needs a password."), UIB_VM("mp.server")), "");
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

void UIB_Join(string addr)
{
	if (substring(addr, 0, 16) == "godnotic://join/")
		addr = substring(addr, 16, -1);
	addr = strreplace("\"", "", strreplace(";", "", addr));
	if (addr == "")
		return;
	UIB_VMSet("mp.server", addr);
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

void UIB_ToggleFav(string addr)
{
	string s = cvar_string("net_slist_favorites"), out = "";
	bool found = false;
	int n = tokenize_console(s);
	for (int i = 0; i < n; ++i)
		if (argv(i) == addr) found = true;
		else out = strcat(out, (out == "" ? "" : " "), argv(i));
	if (!found)
		out = strcat(out, (out == "" ? "" : " "), addr);
	cvar_set("net_slist_favorites", out);
	resorthostcache();
	uib_toast((found ? _("Removed from favourites") : _("Added to favourites")), "success", (found ? "star" : "star_filled"), 3);
}

void UIB_Refresh()
{
	cvar_set("net_slist_lan_only", UIB_VMf("mp.lan") ? "1" : "0");
	refreshhostcache(true);
}

// ---------------------------------------------------------------- pause, loading, credits, dialogs

string UIB_AddrLabel(string a)
{
	tokenizebyseparator(a, ".");
	float o1 = stof(argv(0)), o2 = stof(argv(1));
	if (o1 == 100 && o2 >= 64 && o2 <= 127)
		return _("VPN");
	return _("Wi-Fi");
}

void UIB_BuildPause()
{
	if (!UIB_Screen("pause", _("Paused")))
		return;
	uib_s("kind", "game");
	uib_s("presentation", "fullscreen");
	if (cvar("touch_lan_hosting"))
	{
		bool pw = cvar("touch_lan_host_password_set") || cvar_string("sv_touch_lan_password") != "";
		string addrs = cvar_string("touch_lan_addresses");
		UIB_Open("host", "card");
			uib_s("title", _("Hosting on your network"));
			uib_s("subtitle", _("Others join with one of these addresses"));
			uib_s("icon", "host");
			uib_s("accent", "ember");
			uib_s("density", "compact");
			uib_push("badges", pw ? UIB_Badge("lock", _("Password set"), "warning") : UIB_Badge("unlock", _("Open"), "neutral"));
			int n = min(4, tokenizebyseparator(addrs, " "));
			for (int i = 0; i < n; ++i)
				uib_ids[8 + i] = strzone(argv(i));
			string first = n ? uib_ids[8] : "";
			for (int i = 0; i < n; ++i)
			{
				string lab = UIB_AddrLabel(uib_ids[8 + i]);   // tokenizes: the addresses were copied first
				uib_push("meta", UIB_Chip(((lab == _("VPN")) ? "vpn" : "wifi"), strcat(lab, " ", uib_ids[8 + i])));
			}
			if (first != "")
			{
				uib_j("primary", UIB_AText("host.share", _("Share"), "share", "share", strcat("godnotic://join/", first)));
				uib_push("actions", UIB_AText("host.copy", _("Copy address"), "copy", "copy", first));
			}
			for (int i = 0; i < n; ++i)
				strunzone(uib_ids[8 + i]);
		UIB_Close();
	}
	UIB_Open("match", "card");
		uib_s("title", _("Match"));
		uib_s("subtitle", (gamestatus & GAME_ISSERVER) ? _("Your game") : cvar_string("_uib_server_name"));
		uib_s("aspect", "21:9");
	UIB_Close();
	UIB_Open("actions", "grid");
		uib_f("minItem", 200);
		UIB_Button("resume", _("Resume"), UIB_AS("resume", _("Resume"), "resume", "primary"), "primary");
		UIB_Button("settings", _("Settings"), UIB_A("settings", _("Settings"), "settings"), "secondary");
		UIB_Button("spectate", _("Spectate"), UIB_A("spectate", _("Spectate"), "esp"), "secondary");
		UIB_Open("disconnect", "button");
			uib_s("label", _("Leave match"));
			uib_s("style", "destructive");
			uib_j("action", UIB_AC("disconnect", _("Leave match"), "disconnect", "destructive", _("Leave the match?"), _("Leave"), true));
		UIB_Close();
	UIB_Close();
	UIB_Done();
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
		UIB_Button("list", _("Everyone who made Xonotic"), UIB_A("credits.full", _("Everyone who made Xonotic"), "credits"), "link");
	UIB_Close();
	UIB_Section("licence", _("Licence"), false);
		UIB_Text("text", _("The engine and game code are free software under the GNU General Public License, version 2 or later. Game data is under its authors' licences (see each pack's licence file)."), "caption");
		UIB_Button("source", _("Source code"), UIB_ALink("credits.source", _("Source code"), "link", "https://github.com/dixonSolutions/Godnotic"), "link");
	UIB_Close();
	UIB_Done();
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
		case "mp": UIB_BuildServers(); return;
		case "mp.server": UIB_BuildServer(); return;
		case "mp.password": UIB_BuildPassword(); return;
		case "pause": UIB_BuildPause(); return;
		case "loading": UIB_BuildLoading(); return;
		case "credits": UIB_BuildCredits(); return;
		case "dialog.quit": UIB_BuildQuit(); return;
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
	if (ctx == "loading") UIB_SetStack("loading", "");
	else if (ctx == "game") UIB_SetStack("pause", "");
	else UIB_SetStack("home", "");
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
	strcpy(uibn_context, "");
	UIB_Context();
}

float uib_next_tick;
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
	// time-varying screens
	string top = UIB_TopScreen();
	if ((top == "pause" || top == "mods.detail" || top == "settings.data") && time > uib_next_tick)
	{
		uib_next_tick = time + 1;
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
	switch (a)
	{
		case "sp": UIB_VMSet("create.from", "home"); UIB_Push("sp"); return;
		case "create": UIB_VMSet("create.from", "mp"); UIB_Push("sp"); return;
		case "mp": UIB_Push("mp"); UIB_Refresh(); return;
		case "credits": UIB_Push("credits"); return;
		case "quit": localcmd("\nquit\n"); return;   // the renderer already confirmed
		case "quit.confirm": localcmd("\nquit\n"); return;
		case "dialog.cancel": UIB_Back(); return;
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
		case "server.join": UIB_Join(UIB_VM("mp.server")); return;
		case "server.fav": UIB_ToggleFav(UIB_VM("mp.server")); UIB_RebuildAll(); return;
		case "join.address": UIB_Join(UIB_VM("mp.address")); return;
		case "password.join":
		{
			string addr = UIB_VM("mp.server");
			cvar_set("_cl_touch_lan_password_server", addr);
			UIB_Back();
			localcmd(sprintf("\nconnect \"%s\"\n", addr));
			return;
		}
		case "resume": m_hide(); return;
		case "spectate": localcmd("\nspec\n"); m_hide(); return;
		case "disconnect": localcmd("\ndisconnect\n"); return;
		case "loading.cancel": localcmd("\ndisconnect\n"); return;
		case "credits.full": m_goto("Credits", true); return;
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
			UIB_VMSet("map.sel", row);
			if (strstrofs(json, "map.choose", 0) >= 0) { cvar_set("touch_lan_map", row); UIB_RebuildAll(); }
			else UIB_Push("sp.map");
			return;
		case "sp/recent/list":
			tokenizebyseparator(row, ":");
			string m = argv(0), g = argv(1);
			UIB_StartGame(m, g);
			return;
		case "mp/servers":
			UIB_VMSet("mp.server", row);
			if (isaction && strstrofs(json, "server.join", 0) >= 0) UIB_Join(row);
			else if (isaction && strstrofs(json, "server.fav", 0) >= 0) { UIB_ToggleFav(row); UIB_RebuildAll(); }
			else UIB_Push("mp.server");
			return;
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
		case "mp/join/address": UIB_VMSet("mp.address", value); break;
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
			if (node == "mp/join/address") UIB_Join(arg);
			if (node == "mp.password/form/pw") UIB_Action(node, "password.join", "");
			return 1;
		case "row": UIB_RowEvent(node, arg, json); return 1;
		case "dismiss":
			if (strstrofs(strcat(" ", uibn_modal, " "), strcat(" ", node, " "), 0) >= 0) UIB_Back();
			return 1;
		case "shown": if (node == "mp") UIB_Refresh(); return 1;
		case "hidden": return 1;
		case "changed": UIB_RebuildAll(); return 1;
		case "modpages": UIB_RebuildAll(); return 1;
	}
	return 0;
}
