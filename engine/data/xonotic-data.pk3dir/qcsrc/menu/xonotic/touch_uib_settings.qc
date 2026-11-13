// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_uib_screens.qh"

// Godnotic UI bridge: Settings. Twelve sections, each bound to the cvars players change;
// "Advanced" opens the stock dialogs for the long tail. Mods is a top-level section:
// client and server mods, a mod's detail with the permissions the engine reports,
// install from a file or a link, "How to make a mod", and pages mods contribute.

string UIB_Opts2(string v1, string l1, string v2, string l2)
{
	return strcat("[", UIB_Opt(v1, l1), ",", UIB_Opt(v2, l2), "]");
}

string UIB_Opts3(string v1, string l1, string v2, string l2, string v3, string l3)
{
	return strcat("[", UIB_Opt(v1, l1), ",", UIB_Opt(v2, l2), ",", UIB_Opt(v3, l3), "]");
}

string UIB_Opts4(string v1, string l1, string v2, string l2, string v3, string l3, string v4, string l4)
{
	return strcat("[", UIB_Opt(v1, l1), ",", UIB_Opt(v2, l2), ",", strcat(UIB_Opt(v3, l3), ",", UIB_Opt(v4, l4), "]"));
}

void UIB_SettingsCard(string key, string title, string icon, string sub)
{
	UIB_Open(key, "card");
		uib_s("title", title);
		uib_s("subtitle", sub);
		uib_s("icon", icon);
		uib_s("density", "compact");
		uib_j("primary", UIB_A(strcat("settings.", key), title, "chevron"));
	UIB_Close();
}

void UIB_BuildSettingsRoot()
{
	if (!UIB_Screen("settings", _("Settings")))
		return;
	uib_s("kind", "page");
	UIB_Open("sections", "grid");
		uib_f("minItem", 240);
		UIB_SettingsCard("video", _("Video"), "video", _("Quality, field of view, frame rate"));
		UIB_SettingsCard("audio", _("Audio"), "audio", _("Volume, music, hit sounds"));
		UIB_SettingsCard("game", _("Game"), "game", _("Crosshair, weapons, messages"));
		UIB_SettingsCard("input", _("Input"), "input", _("Mouse, keyboard and keys"));
		UIB_SettingsCard("touch", _("Touch controls"), "touch", _("Presets, HUD, aim assist, auto-shoot, tactics"));
		UIB_SettingsCard("esp", _("ESP"), "esp", _("Offline matches only"));
		UIB_SettingsCard("player", _("Player"), "player", _("Name and colours"));
		UIB_SettingsCard("privacy", _("Privacy"), "privacy", _("Statistics and identity"));
		UIB_SettingsCard("updates", _("Updates"), "update", _("What is new"));
		UIB_SettingsCard("data", _("Data"), "data", _("Game data and downloads"));
		UIB_SettingsCard("menu", _("Menu"), "menu", _("Size and layout"));
		UIB_SettingsCard("mods", _("Mods"), "mods", _("Client and server mods, pages from mods"));
	UIB_Close();
	UIB_Button("advanced", _("Advanced settings"), UIB_A("settings.advanced", _("Advanced settings"), "more"), "link");
	UIB_Done();
}

bool UIB_Page(string id, string title)
{
	if (!UIB_Screen(id, title))
		return false;
	uib_s("kind", "page");
	uib_j("header", "{\"collapsible\":true}");
	return true;
}

void UIB_BuildVideo()
{
	if (!UIB_Page("settings.video", _("Video"))) return;
	UIB_Section("quality", _("Quality"), true);
		UIB_Choice("preset", _("Effects"), "vm:video.preset", "segmented", UIB_Opts4("low", _("Low"), "med", _("Medium"), "high", _("High"), "ultra", _("Ultra")), _("Particles, decals and lights"));
		UIB_Slider("scale", _("Resolution"), "cvar:r_viewscale", 0.5, 1, 0.05, "percent", _("Lower is faster and less sharp"));
		UIB_Switch("dynscale", _("Lower the resolution when the frame rate drops"), "cvar:r_viewscale_fpsscaling", "");
		UIB_Choice("maxfps", _("Frame rate limit"), "cvar:cl_maxfps", "segmented", UIB_Opts4("30", "30", "60", "60", "120", "120", "0", _("None")), "");
		UIB_Open("vsync", "switch");
			uib_s("label", _("Vertical sync"));
			uib_s("bind", "cvar:vid_vsync");
			UIB_Apply("video");
		UIB_Close();
	UIB_Close();
	UIB_Section("view", _("View"), true);
		UIB_Slider("fov", _("Field of view"), "cvar:fov", 60, 130, 1, "deg", "");
		UIB_Slider("contrast", _("Contrast"), "cvar:v_contrast", 0.5, 3, 0.05, "float2", "");
		UIB_Slider("brightness", _("Brightness"), "cvar:v_brightness", 0, 0.5, 0.01, "float2", "");
		UIB_Switch("showfps", _("Show the frame rate"), "cvar:showfps", "");
	UIB_Close();
	UIB_Done();
}

void UIB_BuildAudio()
{
	if (!UIB_Page("settings.audio", _("Audio"))) return;
	UIB_Section("volume", _("Volume"), true);
		UIB_Slider("master", _("Master"), "cvar:volume", 0, 1, 0.05, "percent", "");
		UIB_Slider("music", _("Music"), "cvar:bgmvolume", 0, 1, 0.05, "percent", "");
		UIB_Slider("ambient", _("Ambient"), "cvar:snd_staticvolume", 0, 1, 0.05, "percent", "");
	UIB_Close();
	UIB_Section("cues", _("Cues"), true);
		UIB_Choice("hitsound", _("Hit sound"), "cvar:cl_hitsound", "segmented", UIB_Opts4("0", _("Off"), "1", _("On"), "2", _("Lower"), "3", _("Higher")), _("A sound when you hit an enemy; Lower and Higher change with the damage"));
		UIB_Switch("mute", _("Mute when the game is in the background"), "cvar:snd_mutewhenidle", "");
	UIB_Close();
	UIB_Done();
}

void UIB_BuildGame()
{
	if (!UIB_Page("settings.game", _("Game"))) return;
	UIB_Section("crosshair", _("Crosshair"), true);
		UIB_Slider("size", _("Size"), "cvar:crosshair_size", 0.1, 1, 0.05, "float2", "");
		UIB_Slider("alpha", _("Opacity"), "cvar:crosshair_alpha", 0.1, 1, 0.05, "percent", "");
	UIB_Close();
	UIB_Section("weapons", _("Weapons"), true);
		UIB_Switch("autoswitch", _("Switch to better weapons you pick up"), "cvar:cl_autoswitch", "");
	UIB_Close();
	UIB_Section("content", _("Content"), true);
		UIB_Switch("gentle", _("Gentle mode"), "cvar:cl_gentle", _("No blood or gibs"));
	UIB_Close();
	UIB_Done();
}

string UIB_KeyCmd(int i)
{
	switch (i)
	{
		case 0: return "+forward"; case 1: return "+back"; case 2: return "+moveleft"; case 3: return "+moveright";
		case 4: return "+jump"; case 5: return "+fire"; case 6: return "+fire2";
	}
	return "";
}

string UIB_KeyName(int i)
{
	switch (i)
	{
		case 0: return _("Forward"); case 1: return _("Back"); case 2: return _("Left"); case 3: return _("Right");
		case 4: return _("Jump"); case 5: return _("Fire"); case 6: return _("Secondary fire");
	}
	return "";
}

void UIB_BuildInput()
{
	if (!UIB_Page("settings.input", _("Input"))) return;
	UIB_Section("mouse", _("Mouse"), true);
		UIB_Slider("sens", _("Sensitivity"), "cvar:sensitivity", 0.5, 20, 0.1, "float1", "");
		UIB_Switch("invert", _("Invert vertical look"), "vm:input.invert", "");
	UIB_Close();
	UIB_Section("keys", _("Keys"), true);
		for (int i = 0; i < 7; ++i)
		{
			UIB_Open(strcat("k", ftos(i)), "keybind");
				uib_s("label", UIB_KeyName(i));
				uib_s("command", UIB_KeyCmd(i));
				uib_s("bind", strcat("vm:key.", ftos(i)));
				uib_f("max", 2);
				string keys = findkeysforcommand(UIB_KeyCmd(i), 0);
				string v = "[";
				int n = tokenize(keys);
				for (int k = 0, c = 0; k < n && c < 2; ++k)
					if (stof(argv(k)) >= 0)
					{
						v = strcat(v, (c ? "," : ""), uib_qv(keynumtostring(stof(argv(k)))));
						++c;
					}
				uib_j("value", strcat(v, "]"));
			UIB_Close();
		}
	UIB_Close();
	UIB_Done();
}

void UIB_BuildTouch()
{
	if (!UIB_Page("settings.touch", _("Touch controls"))) return;
	UIB_Section("preset", _("Preset"), true);
		UIB_Choice("preset", _("Preset"), "cvar:touch_preset", "cards", strcat("[", UIB_Opt("balanced", _("Balanced")), ",", UIB_Opt("casual", _("Casual")), ",",
			strcat(UIB_Opt("competitive", _("Competitive")), ",", UIB_Opt("minimal", _("Minimal")), ",", UIB_Opt("left", _("Left-handed")), "]")), _("A starting point; every setting below can still be changed"));
		UIB_Button("reset", _("Apply the preset again"), UIB_A("touch.preset", _("Apply the preset again"), "refresh"), "tonal");
	UIB_Close();
	UIB_Section("hud", _("Controls and HUD"), true);
		UIB_Slider("scale", _("Control size"), "cvar:touch_scale", 0.6, 1.6, 0.05, "x", "");
		UIB_Slider("opacity", _("Opacity"), "cvar:touch_opacity", 0.2, 1, 0.05, "percent", "");
		UIB_Slider("hudscale", _("HUD size"), "cvar:touch_hud_scale", 0.6, 1.6, 0.05, "x", "");
		UIB_Choice("fire", _("Fire button"), "cvar:touch_fire_mode", "segmented", UIB_Opts2("0", _("Hold"), "1", _("Toggle")), "");
		UIB_Choice("hand", _("Handedness"), "cvar:touch_handedness", "segmented", UIB_Opts2("0", _("Right"), "1", _("Left")), "");
		UIB_Switch("dodge", _("Dodge button"), "cvar:touch_dodge_visible", "");
	UIB_Close();
	UIB_Section("aim", _("Aim"), true);
		UIB_Slider("sens", _("Look speed"), "cvar:touch_sens_base", 0.2, 4, 0.05, "x", "");
		UIB_Switch("invert", _("Invert vertical look"), "cvar:touch_invert_y", "");
		UIB_Switch("assist", _("Aim assist"), "cvar:cl_touch_aimassist", _("Aim slows over an enemy and is pulled toward one while you aim"));
		UIB_Slider("strength", _("Pull"), "cvar:cl_touch_aimassist_strength", 0, 1, 0.05, "percent", "");
		UIB_Switch("ring", _("Show the target ring"), "cvar:cl_touch_aim_ring", "");
	UIB_Close();
	UIB_Section("auto", _("Auto-shoot"), true);
		UIB_Switch("autoshoot", _("Fire automatically at enemies"), "cvar:cl_touch_autoshoot", "");
		UIB_Switch("autoswitch", _("Switch weapons for the situation"), "cvar:cl_touch_autoswitch", "");
		UIB_Switch("autododge", _("Dodge incoming fire"), "cvar:cl_touch_autododge", "");
	UIB_Close();
	UIB_Section("tactics", _("Tactics (local matches)"), true);
		UIB_Switch("position", _("Move to good positions"), "cvar:cl_touch_practice_position", "");
		UIB_Switch("objectives", _("Play the objective"), "cvar:cl_touch_practice_objectives", "");
		UIB_Switch("threat", _("Watch the biggest threat"), "cvar:cl_touch_practice_threat", "");
	UIB_Close();
	UIB_Done();
}

void UIB_BuildESP()
{
	if (!UIB_Page("settings.esp", _("ESP"))) return;
	UIB_Open("note", "text");
		uib_s("body", _("ESP shows players through walls. It works only in matches on this device (Singleplayer and games you host); on other servers it stays off."));
		uib_s("style", "note");
	UIB_Close();
	UIB_Switch("enabled", _("ESP in local matches"), "cvar:cl_touch_esp_enabled", "");
	UIB_Switch("seethrough", _("Outline enemies behind walls"), "cvar:cl_touch_practice_seethrough", "");
	UIB_Done();
}

string UIB_ColourHex(int i)
{
	switch (i)
	{
		case 0: return "#ffffff"; case 1: return "#ff0000"; case 2: return "#33ff00"; case 3: return "#ffff00";
		case 4: return "#3366ff"; case 5: return "#00ffff"; case 6: return "#ff3366"; case 7: return "#ffffff";
		case 8: return "#ff6600"; case 9: return "#9933ff"; case 10: return "#ff00ff"; case 11: return "#00ff99";
		case 12: return "#ccff00"; case 13: return "#0099ff"; case 14: return "#ffcc00"; case 15: return "#666666";
	}
	return "#ffffff";
}

int UIB_ColourIndex(string hex)
{
	for (int i = 0; i < 16; ++i)
		if (UIB_ColourHex(i) == hex && i != 7)
			return i;
	return 0;
}

string UIB_Palette()
{
	string s = "[";
	for (int i = 0; i < 16; ++i)
		if (i != 7)
			s = strcat(s, (i ? "," : ""), "\"", UIB_ColourHex(i), "\"");
	return strcat(s, "]");
}

void UIB_BuildPlayer()
{
	if (!UIB_Page("settings.player", _("Player"))) return;
	UIB_Open("name", "field");
		uib_s("label", _("Name"));
		uib_s("bind", "cvar:_cl_name");
		uib_s("kind", "name");
		uib_f("maxLength", 32);
		UIB_Apply("reconnect");
	UIB_Close();
	float c = cvar("_cl_color");
	UIB_Open("shirt", "colour");
		uib_s("label", _("Shirt colour"));
		uib_s("bind", "vm:player.shirt");
		uib_s("value", UIB_ColourHex(floor(c / 16)));
		uib_j("palette", UIB_Palette());
		UIB_Apply("reconnect");
	UIB_Close();
	UIB_Open("pants", "colour");
		uib_s("label", _("Pants colour"));
		uib_s("bind", "vm:player.pants");
		uib_s("value", UIB_ColourHex(c - 16 * floor(c / 16)));
		uib_j("palette", UIB_Palette());
		UIB_Apply("reconnect");
	UIB_Close();
	UIB_Button("model", _("Player model"), UIB_A("settings.model", _("Player model"), "player"), "tonal");
	UIB_Done();
}

void UIB_BuildPrivacy()
{
	if (!UIB_Page("settings.privacy", _("Privacy"))) return;
	UIB_Text("note", _("Nothing is shared with XonStat unless you turn it on here."), "caption");
	UIB_Choice("tracking", _("Statistics"), "cvar:cl_allow_uidtracking", "segmented", UIB_Opts2("0", _("Off"), "1", _("On")), _("Lets XonStat link your matches to your player ID"));
	UIB_Choice("ranking", _("Rankings"), "cvar:cl_allow_uidranking", "segmented", UIB_Opts2("0", _("Off"), "1", _("On")), _("Lets your player ID appear in leaderboards"));
	UIB_Choice("name", _("Show my name in records"), "cvar:cl_allow_uid2name", "segmented", UIB_Opts2("0", _("Off"), "1", _("On")), "");
	UIB_Done();
}

void UIB_BuildUpdates()
{
	if (!UIB_Page("settings.updates", _("Updates"))) return;
	UIB_Text("version", sprintf(_("Version %s"), cvar_string("g_xonoticversion")), "title");
	string url = cvar_string("_touch_update_changelog_url");
	if (substring(url, 0, 8) == "https://")
		UIB_Button("changes", _("What is new"), UIB_ALink("updates.changes", _("What is new"), "info", url), "link");
	UIB_Text("how", _("Updates arrive through your app store, Flatpak or the web page; the game data updates itself."), "caption");
	UIB_Done();
}

void UIB_BuildData()
{
	if (!UIB_Page("settings.data", _("Data"))) return;
	string err = cvar_string("_touch_asset_fetch_error");
	UIB_Open("status", "status");
		bool active = cvar("_touch_asset_fetch_active");
		bool ready = cvar("_touch_assets_ready");
		uib_s("icon", active ? "download" : (ready ? "check" : "warning"));
		uib_s("title", active ? _("Downloading game data") : (ready ? _("Game data is ready") : _("Game data is missing")));
		if (err != "") uib_s("body", err);
		if (active) uib_b("indeterminate", 1);
		if (!active) uib_j("primary", UIB_A("data.fetch", (ready ? _("Check for new data") : _("Download")), "download"));
	UIB_Close();
	UIB_Switch("background", _("Download in the background"), "cvar:_touch_asset_fetch_background", "");
	UIB_Done();
}

void UIB_BuildMenuSettings()
{
	if (!UIB_Page("settings.menu", _("Menu"))) return;
	UIB_Slider("size", _("Menu size"), "cvar:menu_touch_size", 0.7, 1.5, 0.05, "x", _("Applies the next time the menu opens"));
	UIB_Switch("uib", _("New menu"), "cvar:menu_touch_uib", _("The card menu described once and drawn here, on the web and natively"));
	UIB_Switch("quitdialog", _("Ask before quitting"), "cvar:menu_showquitdialog", "");
	UIB_Done();
}

// ---------------------------------------------------------------- mods

int uib_tmpn;
string uib_tmp[64];
void UIB_Lines(string s)
{
	for (int i = 0; i < uib_tmpn; ++i)
		strfree(uib_tmp[i]);
	uib_tmpn = 0;
	int n = tokenizebyseparator(s, "\n");
	for (int i = 0; i < n && uib_tmpn < 64; ++i)
		if (argv(i) != "")
			uib_tmp[uib_tmpn++] = strzone(argv(i));
}

void UIB_ModRows(string source, string cvarname)
{
	UIB_Lines(cvar_string(cvarname));
	uib_rows_begin(source);
	for (int i = 0; i < uib_tmpn; ++i)
	{
		tokenizebyseparator(uib_tmp[i], "\t");
		string name = argv(0), state = argv(1), title = argv(2), err = argv(3);
		if (title == "") title = name;
		bool on = state != "disabled" && state != "off" && state != "";
		uib_row(name, sprintf("{\"id\":%s,\"name\":%s,\"state\":%s,\"enabled\":%s,\"error\":%s}", uib_qv(name), uib_q(title), uib_qv(state),
			(on ? "true" : "false"), uib_q(err)), "");
	}
	uib_rows_end();
}

void UIB_ModList(string key, string title, string sub, string source)
{
	UIB_Section(key, title, true);
		uib_s("subtitle", sub);
		UIB_Open("list", "list");
			uib_s("source", source);
			uib_s("display", "rows");
			uib_s("rowHeight", "comfortable");
			uib_s("density", "compact");
			uib_s("state", "ready");
			uib_push("columns", UIB_Col("name", "text", _("Mod"), "fill", 1, true, false));
			uib_push("columns", UIB_Col("state", "text", _("State"), "s", 2, false, false));
			uib_push("columns", UIB_Col("enabled", "bool", _("On"), "xs", 1, false, true));
			uib_j("rowPrimary", UIB_A("mod.open", _("Details"), "info"));
			uib_j("empty", sprintf("{\"title\":%s,\"text\":%s,\"icon\":\"mods\"}", uib_q(_("No mods yet")), uib_q(_("Install one from a file or a link."))));
		UIB_Close();
	UIB_Close();
}

void UIB_BuildMods()
{
	UIB_ModRows("mods.client", "_cl_lua_mods");
	UIB_ModRows("mods.server", "_sv_lua_plugins");
	if (!UIB_Page("settings.mods", _("Mods"))) return;
	UIB_ModList("client", _("Client mods"), _("Run in your game only"), "mods.client");
	UIB_ModList("server", _("Server mods"), _("Run in games you host"), "mods.server");
	UIB_Open("install", "row");
		uib_b("wrap", 1);
		UIB_Open("file", "button");
			uib_s("label", _("Install from file"));
			uib_s("style", "tonal");
			uib_j("action", "{\"id\":\"mods.install.file\",\"label\":\"\",\"icon\":\"folder\",\"kind\":\"file\",\"accept\":[\"godnotic-mod\",\"zip\",\"pk3\"]}");
		UIB_Close();
		UIB_Button("link", _("Install from link"), UIB_A("mods.install.link", _("Install from link"), "link"), "tonal");
		UIB_Button("howto", _("How to make a mod"), UIB_ALink("mods.howto", _("How to make a mod"), "help", "https://github.com/dixonSolutions/Godnotic/blob/main/docs/MODDING.md"), "link");
	UIB_Close();
	string pages = uib_modpages();
	int n = tokenizebyseparator(pages, "\n");
	if (n > 0 && argv(0) != "")
	{
		UIB_Section("pages", _("Pages from mods"), true);
			for (int i = 0; i < n && i < 16; ++i)
				uib_ids2[i] = strzone(argv(i));
			for (int i = 0; i < n && i < 16; ++i)
			{
				if (uib_ids2[i] == "") { strunzone(uib_ids2[i]); continue; }
				tokenize_console(uib_ids2[i]);
				string mod = argv(0), page = argv(1);
				string title = substring(uib_ids2[i], strlen(mod) + strlen(page) + 2, -1);
				UIB_Open(strcat(mod, ".", page), "card");
					uib_s("title", title);
					uib_s("subtitle", sprintf(_("From the mod %s"), mod));
					uib_s("icon", "mods");
					uib_j("primary", UIB_A(strcat("modpage.", mod, ".", page), _("Open"), "chevron"));
				UIB_Close();
				strunzone(uib_ids2[i]);
			}
		UIB_Close();
	}
	UIB_Done();
}

void UIB_BuildModDetail()
{
	string sel = UIB_VM("mods.sel");
	UIB_Lines(cvar_string("_lua_detail"));
	string title = sel, state = "", ver = "", author = "", desc = "", err = "";
	bool enabled = false;
	if (!UIB_Page("mods.detail", sel)) return;
	UIB_Open("perms", "section");
	uib_s("title", _("Permissions"));
	uib_b("sticky", 1);
	int np = 0;
	for (int i = 0; i < uib_tmpn; ++i)
	{
		tokenizebyseparator(uib_tmp[i], "\t");
		string k = argv(0), v = argv(1);
		switch (k)
		{
			case "title": title = v; break;
			case "state": state = v; break;
			case "version": ver = v; break;
			case "author": author = v; break;
			case "description": desc = v; break;
			case "error": err = v; break;
			case "enabled": enabled = v == "1"; break;
			case "perm":
				UIB_Open(strcat("p", ftos(np++)), "text");
					uib_s("body", sprintf("%s %s: %s", ((argv(2) == "1") ? "✓" : "✗"), v, argv(3)));
					uib_s("style", argv(2) == "1" ? "body" : "caption");
				UIB_Close();
				break;
		}
	}
	if (!np) UIB_Text("none", _("Asks for nothing"), "caption");
	UIB_Close();
	UIB_Open("about", "preview");
		uib_s("title", title);
		uib_s("subtitle", sprintf("%s %s", ver, author));
		uib_s("body", desc);
		uib_push("attrs", UIB_Attr(_("State"), state, "info"));
		if (err != "") uib_push("badges", UIB_Badge("error", err, "danger"));
		uib_j("primary", enabled ? UIB_AS("mod.disable", _("Turn off"), "close", "secondary") : UIB_AS("mod.enable", _("Turn on"), "check", "primary"));
		uib_push("actions", UIB_AC("mod.uninstall", _("Uninstall"), "delete", "destructive", _("Uninstall this mod?"), _("Uninstall"), true));
	UIB_Close();
	UIB_Done();
}

void UIB_BuildModLink()
{
	if (!UIB_Screen("mods.link", _("Install from link"))) return;
	uib_s("presentation", "sheet");
	uib_b("dismissable", 1);
	UIB_Open("form", "form");
		uib_j("submit", UIB_AS("mods.install.url", _("Install"), "download", "primary"));
		UIB_Open("url", "field");
			uib_s("label", _("Link (https)"));
			uib_s("bind", "vm:mods.url");
			uib_s("kind", "url");
			uib_s("placeholder", "https://…");
			uib_s("value", UIB_VM("mods.url"));
		UIB_Close();
	UIB_Close();
	UIB_Text("note", _("The package is checked whole before anything is written, and starts with nothing granted."), "caption");
	UIB_Done();
}

void UIB_BuildSettings(string id)
{
	switch (id)
	{
		case "settings": UIB_BuildSettingsRoot(); return;
		case "settings.video": UIB_BuildVideo(); return;
		case "settings.audio": UIB_BuildAudio(); return;
		case "settings.game": UIB_BuildGame(); return;
		case "settings.input": UIB_BuildInput(); return;
		case "settings.touch": UIB_BuildTouch(); return;
		case "settings.esp": UIB_BuildESP(); return;
		case "settings.player": UIB_BuildPlayer(); return;
		case "settings.privacy": UIB_BuildPrivacy(); return;
		case "settings.updates": UIB_BuildUpdates(); return;
		case "settings.data": UIB_BuildData(); return;
		case "settings.menu": UIB_BuildMenuSettings(); return;
		case "settings.mods": UIB_BuildMods(); return;
		case "mods.detail": UIB_BuildModDetail(); return;
		case "mods.link": UIB_BuildModLink(); return;
	}
	// mods.page.<mod>.<page> is built by the engine from the mod's description
}


bool UIB_SettingsAction(string node, string a, string payload)
{
	if (substring(a, 0, 9) == "settings." && a != "settings.advanced" && a != "settings.model")
	{
		UIB_Push(a);
		return true;
	}
	if (substring(a, 0, 8) == "modpage.")
	{
		UIB_Push(strcat("mods.page.", substring(a, 8, -1)));
		return true;
	}
	string sel = UIB_VM("mods.sel");
	string p = (UIB_VM("mods.realm") == "server") ? "sv_lua_plugin_" : "lua_mod_";
	switch (a)
	{
		case "settings": UIB_Push("settings"); return true;
		case "settings.advanced": m_goto("", true); m_goto("settings", true); return true;
		case "settings.model": m_goto("profile", true); return true;
		case "touch.preset":
			localcmd(strcat("exec touch/profiles/", cvar_string("touch_preset"), ".cfg\n"));
			if (cvar_string("touch_preset") != "competitive")
				localcmd("exec touch/profiles/helpers.cfg\n");
			uib_toast(_("Preset applied"), "success", "check", 2);
			return true;
		case "data.fetch": localcmd("\ntouch_assets_fetch\n"); return true;
		case "mods.install.link": UIB_Modal("mods.link"); return true;
		case "mods.install.url":
		{
			string url = UIB_VM("mods.url");
			if (substring(url, 0, 8) != "https://")
			{
				uib_toast(_("Only https links can be installed"), "error", "error", 4);
				return true;
			}
			localcmd(sprintf("\n%sinstall \"%s\"\n", p, strreplace("\"", "", url)));
			UIB_Back();
			uib_toast(_("Installing…"), "info", "download", 3);
			return true;
		}
		case "mods.install.file":
		{
			// the renderer returns the chosen file as payload.path
			int k = strstrofs(payload, "\"path\":\"", 0);
			if (k < 0) return true;
			string path = substring(payload, k + 8, -1);
			path = substring(path, 0, strstrofs(path, "\"", 0));
			localcmd(sprintf("\n%sinstall \"%s\"\n", p, strreplace("\"", "", path)));
			return true;
		}
		case "mod.enable": localcmd(sprintf("\n%senable %s\n", p, sel)); UIB_RebuildAll(); return true;
		case "mod.disable": localcmd(sprintf("\n%sdisable %s\n", p, sel)); UIB_RebuildAll(); return true;
		case "mod.uninstall": localcmd(sprintf("\n%suninstall %s\n", p, sel)); UIB_Back(); return true;
	}
	return false;
}

bool UIB_SettingsValue(string node, string value)
{
	switch (node)
	{
		case "settings.player/shirt":
		case "settings.player/pants":
		{
			float c = cvar("_cl_color");
			float shirt = floor(c / 16), pants = c - 16 * shirt;
			if (node == "settings.player/shirt") shirt = UIB_ColourIndex(value);
			else pants = UIB_ColourIndex(value);
			cvar_set("_cl_color", ftos(shirt * 16 + pants));
			UIB_RebuildAll();
			return true;
		}
		case "settings.video/quality/preset":
			UIB_VMSet("video.preset", value);
			localcmd(sprintf("\nexec effects-%s.cfg\n", value));
			UIB_RebuildAll();
			return true;
		case "settings.input/mouse/invert":
			UIB_VMSet("input.invert", value);
			cvar_set("m_pitch", ftos((value == "1" ? -1 : 1) * fabs(cvar("m_pitch"))));
			UIB_RebuildAll();
			return true;
		case "mods.link/form/url": UIB_VMSet("mods.url", value); return true;
	}
	if (substring(node, 0, 20) == "settings.input/keys/")
	{
		// value: key names separated by spaces (the engine joined the array)
		string cmd = uib_get(node, "command");
		string old = findkeysforcommand(cmd, 0);
		int n = tokenize(old);
		for (int i = 0; i < n; ++i)
			if (stof(argv(i)) >= 0)
				localcmd(sprintf("\nunbind %s\n", keynumtostring(stof(argv(i)))));
		n = tokenize_console(value);
		for (int i = 0; i < n && i < 2; ++i)
			localcmd(sprintf("\nbind %s \"%s\"\n", argv(i), cmd));
		return true;
	}
	return false;
}

bool UIB_SettingsRow(string node, string row, string json)
{
	if (node == "settings.mods/client/list" || node == "settings.mods/server/list")
	{
		bool server = node == "settings.mods/server/list";
		UIB_VMSet("mods.sel", row);
		UIB_VMSet("mods.realm", server ? "server" : "client");
		cvar_set("_lua_select", strcat((server ? "plugin " : "mod "), row));
		UIB_Push("mods.detail");
		return true;
	}
	return false;
}
