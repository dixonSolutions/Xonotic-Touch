#include "dialog_touch_mods.qh"

#include "dialog.qh"
#include "../item/modalcontroller.qh"

// The Mods tab (see dialog_touch_mods.qh). A model function (TouchMods_update,
// once per frame) turns the engine's cvars and the current view into the text
// and the action of every widget; the widgets only show it and run the action
// when tapped. An action is a console command, or "@..." for a change of view.

const int TM_SLOTS = 5;     // finger-sized rows (tall: about 48 dp on a 10" tablet)
const int TM_BARS = 5;      // buttons along the bottom
const int TM_MAX = 32;      // mods per realm the engine keeps
const int TM_LINES = 96;

const int TM_V_LIST = 0;
const int TM_V_DETAIL = 1;
const int TM_V_INSTALL = 2;
const int TM_V_REVIEW = 3;
const int TM_V_REMOVE = 4;
const int TM_V_ENABLE = 5;  // turning a mod on: what it asks for, granted in one step
const int TM_V_TEXT = 6;    // the whole of a text too long for its row

const int TM_P_PERMS = 0;
const int TM_P_SETTINGS = 1;
const int TM_P_LOG = 2;
const int TM_P_INFO = 3;

// widget roles
const int TM_B_BIG = 0;
const int TM_B_A = 1;
const int TM_B_B = 2;
const int TM_B_PREV = 3;
const int TM_B_NEXT = 4;
const int TM_B_BAR = 5;
const int TM_L_TITLE = 0;
const int TM_L_DETAIL = 1;
const int TM_L_SUMMARY = 2;
const int TM_L_HEADER = 3;
const int TM_L_FULL = 4;    // the text page's text, over the rows

int tm_view;
int tm_realm;        // 0: client mods, 1: server plugins
int tm_page;
int tm_sub;          // detail sub-page
int tm_target;       // the realm Install puts a package in
bool tm_keepdata;
int tm_backview;     // where the Enable review returns to
bool tm_choose[16];  // the Enable review's selection, per requested permission
string tm_choosefor; // the mod that selection belongs to
bool tm_logend;      // the output page follows the newest line
string tm_sel;       // the selected mod's name
float tm_frame;
string tm_lastinstall;
string tm_lastpicked;
entity tm_input;

// what each widget shows and does
string tm_big[TM_SLOTS];
string tm_bigact[TM_SLOTS];
vector tm_bigcolor[TM_SLOTS];
string tm_title[TM_SLOTS];
string tm_detail[TM_SLOTS];
string tm_a[TM_SLOTS];
string tm_aact[TM_SLOTS];
string tm_b[TM_SLOTS];
string tm_bact[TM_SLOTS];
string tm_bar[TM_BARS];
string tm_baract[TM_BARS];
bool tm_barsel[TM_BARS];
string tm_header, tm_summary, tm_prevact, tm_nextact;
string tm_fullhead, tm_fulltext; // the text page
int tm_textback;                 // the view it returns to

#define TM_SET(var, val) MACRO_BEGIN string _tm_v = (val); if (_tm_v != var) strcpy(var, _tm_v); MACRO_END

// ---------------------------------------------------------------------------
// The engine's cvars, parsed

string tm_tmp[TM_LINES];
int tm_tmpn;

// splits a blob into tm_tmp (tokenize's buffer is shared: copy the lines out first)
void TouchMods_lines(string s)
{
	int n = tokenizebyseparator(s, "\n");
	int i;
	tm_tmpn = 0;
	for (i = 0; i < n && tm_tmpn < TM_LINES; ++i)
	{
		if (argv(i) == "")
			continue;
		strcpy(tm_tmp[tm_tmpn], argv(i));
		++tm_tmpn;
	}
}

// the list of the current realm
string tm_listraw;
int tm_listrealm = -1;
int tm_count;
string tm_name[TM_MAX];
string tm_state[TM_MAX];
string tm_ttl[TM_MAX];
string tm_err[TM_MAX];
string tm_desc[TM_MAX];

void TouchMods_readList()
{
	string s = cvar_string((tm_realm ? "_sv_lua_plugins" : "_cl_lua_mods"));
	if (s == tm_listraw && tm_realm == tm_listrealm)
		return;
	strcpy(tm_listraw, s);
	tm_listrealm = tm_realm;
	TouchMods_lines(s);
	tm_count = min(tm_tmpn, TM_MAX);
	for (int i = 0; i < tm_count; ++i)
	{
		tokenizebyseparator(tm_tmp[i], "\t");
		strcpy(tm_name[i], argv(0));
		strcpy(tm_state[i], argv(1));
		strcpy(tm_ttl[i], argv(2));
		strcpy(tm_err[i], argv(3));
		strcpy(tm_desc[i], argv(4));
	}
}

// the selected mod (_lua_detail)
string tm_detraw;
string tm_d_name, tm_d_state, tm_d_title, tm_d_version, tm_d_author, tm_d_homepage, tm_d_desc, tm_d_api, tm_d_manifest, tm_d_error, tm_d_memory;
bool tm_d_enabled;
const int TM_PERMS = 16;
int tm_np;
string tm_pn[TM_PERMS];
string tm_pr[TM_PERMS];
bool tm_pg[TM_PERMS];
const int TM_SETS = 8;
int tm_ns;
string tm_sc[TM_SETS];
string tm_st[TM_SETS];
string tm_sv[TM_SETS];
string tm_sa[TM_SETS];
string tm_sl[TM_SETS];
const int TM_LOG = 48;
int tm_nlog;
string tm_log[TM_LOG];
const int TM_INFO = 64;
int tm_ninfo;
string tm_info[TM_INFO];

void TouchMods_info(string s)
{
	if (tm_ninfo < TM_INFO)
	{
		strcpy(tm_info[tm_ninfo], s);
		++tm_ninfo;
	}
}

void TouchMods_readDetail()
{
	string s = cvar_string("_lua_detail");
	if (s == tm_detraw)
		return;
	strcpy(tm_detraw, s);
	TouchMods_lines(s);
	tm_np = tm_ns = tm_nlog = tm_ninfo = 0;
	strcpy(tm_d_name, "");
	strcpy(tm_d_error, "");
	strcpy(tm_d_manifest, "");
	for (int i = 0; i < tm_tmpn; ++i)
	{
		tokenizebyseparator(tm_tmp[i], "\t");
		string k = argv(0), v = argv(1);
		switch (k)
		{
			case "name": strcpy(tm_d_name, v); break;
			case "state": strcpy(tm_d_state, v); break;
			case "title": strcpy(tm_d_title, v); break;
			case "version": strcpy(tm_d_version, v); break;
			case "author": strcpy(tm_d_author, v); break;
			case "homepage": strcpy(tm_d_homepage, v); if (v != "") TouchMods_info(strcat("^7", _("Homepage:"), " ^5", v)); break;
			case "description": strcpy(tm_d_desc, v); break;
			case "api": strcpy(tm_d_api, v); TouchMods_info(strcat("^7", _("API level:"), " ^3", v)); break;
			case "manifest": strcpy(tm_d_manifest, v); if (v != "") TouchMods_info(strcat("^1", _("mod.txt:"), " ", v)); break;
			case "error": strcpy(tm_d_error, v); break;
			case "enabled": tm_d_enabled = (v == "1"); break;
			case "memory": strcpy(tm_d_memory, v); if (v != "") TouchMods_info(strcat("^7", _("Memory:"), " ", v)); break;
			case "perm":
				if (tm_np < TM_PERMS)
				{
					strcpy(tm_pn[tm_np], v);
					tm_pg[tm_np] = (argv(2) == "1");
					strcpy(tm_pr[tm_np], argv(3));
					++tm_np;
				}
				break;
			case "setting":
				if (tm_ns < TM_SETS)
				{
					strcpy(tm_sc[tm_ns], v);
					strcpy(tm_st[tm_ns], argv(2));
					strcpy(tm_sv[tm_ns], argv(3));
					strcpy(tm_sa[tm_ns], argv(4));
					strcpy(tm_sl[tm_ns], argv(5));
					++tm_ns;
				}
				break;
			case "log":
				if (tm_nlog < TM_LOG)
				{
					strcpy(tm_log[tm_nlog], v);
					++tm_nlog;
				}
				break;
			case "event": TouchMods_info(strcat("^7", _("Event:"), " ^2", v)); break;
			case "hook": TouchMods_info(strcat("^7", _("Hook:"), " ^2", v)); break;
			case "command": TouchMods_info(strcat("^7", _("Command:"), " ^5", v)); break;
		}
	}
}

// the pending install (_lua_install)
string tm_instraw;
string tm_i_state, tm_i_realm, tm_i_source, tm_i_message, tm_i_name, tm_i_title, tm_i_version, tm_i_author, tm_i_homepage, tm_i_desc, tm_i_api, tm_i_old, tm_i_files, tm_i_sha;
bool tm_i_update, tm_i_pinned;
int tm_nip;
string tm_ipn[TM_PERMS];
string tm_ipt[TM_PERMS];
string tm_ipr[TM_PERMS];

void TouchMods_readInstall()
{
	string s = cvar_string("_lua_install");
	if (s == tm_instraw)
		return;
	strcpy(tm_instraw, s);
	TouchMods_lines(s);
	tm_nip = 0;
	strcpy(tm_i_state, "idle");
	strcpy(tm_i_message, "");
	for (int i = 0; i < tm_tmpn; ++i)
	{
		tokenizebyseparator(tm_tmp[i], "\t");
		string k = argv(0), v = argv(1);
		switch (k)
		{
			case "state": strcpy(tm_i_state, v); break;
			case "realm": strcpy(tm_i_realm, v); break;
			case "source": strcpy(tm_i_source, v); break;
			case "message": strcpy(tm_i_message, v); break;
			case "name": strcpy(tm_i_name, v); break;
			case "title": strcpy(tm_i_title, v); break;
			case "version": strcpy(tm_i_version, v); break;
			case "author": strcpy(tm_i_author, v); break;
			case "homepage": strcpy(tm_i_homepage, v); break;
			case "description": strcpy(tm_i_desc, v); break;
			case "api": strcpy(tm_i_api, v); break;
			case "update": tm_i_update = (v == "1"); break;
			case "oldversion": strcpy(tm_i_old, v); break;
			case "files": strcpy(tm_i_files, v); break;
			case "sha256": strcpy(tm_i_sha, v); break;
			case "pinned": tm_i_pinned = (v == "1"); break;
			case "perm":
				if (tm_nip < TM_PERMS)
				{
					strcpy(tm_ipn[tm_nip], v);
					strcpy(tm_ipt[tm_nip], argv(2));
					strcpy(tm_ipr[tm_nip], argv(3));
					++tm_nip;
				}
				break;
		}
	}
}

// "" while the platform has no importer (not Android), else its last status line
string tm_importStatus;
float tm_importRead;
string TouchMods_importStatus()
{
	if (time - tm_importRead < 1 && tm_importRead)
		return tm_importStatus;
	tm_importRead = time;
	string line = "";
	float fh = fopen(TOUCH_MODS_IMPORT_STATUS_FILE, FILE_READ);
	if (fh >= 0)
	{
		line = fgets(fh);
		// an empty line is "ready"; fgets gives "" for it, not a null
		// string, and "" must still say "this platform has an importer"
		if (line == "")
			line = " ";
		fclose(fh);
	}
	strcpy(tm_importStatus, line);
	return tm_importStatus;
}

// ---------------------------------------------------------------------------
// The model

string TouchMods_prefix(int realm)
{
	return realm ? "sv_lua_mod_" : "lua_mod_";
}

string TouchMods_kind(int realm)
{
	return realm ? _("server mod") : _("mod");
}

string TouchMods_stateText(string st)
{
	switch (st)
	{
		case "running": return strcat("^2", _("running"));
		case "error": return strcat("^1", _("stopped by an error"));
		case "unfinished": return strcat("^1", _("did not finish loading"));
		case "enabled": return strcat("^3", _("on, not running yet"));
	}
	return strcat("^8", _("off"));
}

bool TouchMods_isOn(string st)
{
	return st != "off" && st != "unfinished" && st != "";
}

void TouchMods_clearSlots()
{
	for (int i = 0; i < TM_SLOTS; ++i)
	{
		TM_SET(tm_big[i], "");
		TM_SET(tm_bigact[i], "");
		tm_bigcolor[i] = '1 1 1';
		TM_SET(tm_title[i], "");
		TM_SET(tm_detail[i], "");
		TM_SET(tm_a[i], "");
		TM_SET(tm_aact[i], "");
		TM_SET(tm_b[i], "");
		TM_SET(tm_bact[i], "");
	}
	for (int j = 0; j < TM_BARS; ++j)
	{
		TM_SET(tm_bar[j], "");
		TM_SET(tm_baract[j], "");
		tm_barsel[j] = false;
	}
	TM_SET(tm_prevact, "");
	TM_SET(tm_nextact, "");
}

// pages of per items over n entries; sets prev/next and returns the first index
int TouchMods_paging(int n, int per)
{
	int pages = max(1, ceil(n / per));
	tm_page = bound(0, tm_page, pages - 1);
	if (tm_page > 0) TM_SET(tm_prevact, "@page -1");
	if (tm_page + 1 < pages) TM_SET(tm_nextact, "@page 1");
	return tm_page * per;
}

string TouchMods_pageText(int n, int per)
{
	int pages = max(1, ceil(n / per));
	if (pages < 2)
		return "";
	return sprintf(_("page %d of %d"), tm_page + 1, pages);
}

string TouchMods_lastMessage()
{
	string m = cvar_string("_lua_last");
	return (m == "") ? "" : strcat("  ^3", m);
}

void TouchMods_bar(int i, string text, string act, bool sel)
{
	TM_SET(tm_bar[i], text);
	TM_SET(tm_baract[i], act);
	tm_barsel[i] = sel;
}

void TouchMods_modelList()
{
	TouchMods_readList();
	int first = TouchMods_paging(tm_count, TM_SLOTS);
	string p = TouchMods_prefix(tm_realm);
	TM_SET(tm_header, (tm_realm ? _("Server mods (games you host)") : _("Client mods (this device)")));
	if (!tm_count)
		TM_SET(tm_summary, strcat((tm_realm
			? _("No server mods yet. Install one, or put its folder in the plugins folder; new ones start off.")
			: _("No mods yet. Install one, or put its folder in the mods folder; new ones start off.")), TouchMods_lastMessage()));
	else
		TM_SET(tm_summary, strcat(sprintf(_("%d installed. %s"), tm_count, TouchMods_pageText(tm_count, TM_SLOTS)), TouchMods_lastMessage()));
	for (int s = 0; s < TM_SLOTS; ++s)
	{
		int i = first + s;
		if (i >= tm_count)
			continue;
		bool on = TouchMods_isOn(tm_state[i]);
		TM_SET(tm_big[s], (on ? _("ON") : _("OFF")));
		TM_SET(tm_bigact[s], (on ? sprintf("%sdisable %s", p, tm_name[i]) : strcat("@turnon ", tm_name[i])));
		tm_bigcolor[s] = (tm_state[i] == "error" || tm_state[i] == "unfinished") ? '1 0.55 0.55' : (on ? '0.55 1 0.55' : '1 1 1');
		TM_SET(tm_title[s], strcat("^7", tm_ttl[i], "  ", TouchMods_stateText(tm_state[i])));
		TM_SET(tm_detail[s], ((tm_err[i] != "") ? strcat("^1", tm_err[i]) : strcat("^8", (tm_desc[i] != "") ? tm_desc[i] : tm_name[i])));
		TM_SET(tm_a[s], _("Details"));
		TM_SET(tm_aact[s], strcat("@detail ", tm_name[i]));
		if (on)
		{
			TM_SET(tm_b[s], _("Reload"));
			TM_SET(tm_bact[s], strcat(p, "reload ", tm_name[i]));
		}
	}
	// How to make a mod: in the first row the page leaves free.
	for (int hs = 0; hs < TM_SLOTS; ++hs)
		if (first + hs >= tm_count)
		{
			TM_SET(tm_big[hs], _("Guide"));
			TM_SET(tm_bigact[hs], "@howto");
			tm_bigcolor[hs] = '0.6 0.85 1';
			TM_SET(tm_title[hs], strcat("^7", _("How to make a mod")));
			TM_SET(tm_detail[hs], strcat("^8", _("Opens the guide (docs/MODDING.md on GitHub) in your browser: a first mod in five minutes.")));
			break;
		}
	TouchMods_bar(0, _("Client mods"), "@realm 0", !tm_realm);
	TouchMods_bar(1, _("Server mods"), "@realm 1", tm_realm);
	TouchMods_bar(2, _("Install..."), "@install", false);
	if (tm_count)
		TouchMods_bar(3, _("Reload all"), strcat(p, "reload"), false);
	string master = tm_realm ? "sv_lua_enable" : "cl_lua_enable";
	TouchMods_bar(4, (cvar(master) ? _("Running: ON") : _("Running: OFF")), strcat("toggle ", master), cvar(master) != 0);
}

void TouchMods_modelDetail()
{
	TouchMods_readDetail();
	string p = TouchMods_prefix(tm_realm);
	if (tm_d_name != tm_sel)
	{
		// the engine has not answered the selection yet, or the mod is gone
		TM_SET(tm_header, tm_sel);
		TM_SET(tm_summary, strcat(_("This one is not installed any more."), TouchMods_lastMessage()));
		TouchMods_bar(0, _("< Back"), "@list", false);
		return;
	}
	bool on = TouchMods_isOn(tm_d_state);
	TM_SET(tm_header, sprintf("%s %s", (tm_realm ? _("Server mod") : _("Mod")), tm_d_title));
	TM_SET(tm_big[0], (on ? _("ON") : _("OFF")));
	TM_SET(tm_bigact[0], (on ? sprintf("%sdisable %s", p, tm_sel) : strcat("@turnon ", tm_sel)));
	tm_bigcolor[0] = (tm_d_state == "error" || tm_d_state == "unfinished") ? '1 0.55 0.55' : (on ? '0.55 1 0.55' : '1 1 1');
	string by = "";
	if (tm_d_version != "") by = strcat(by, "  v", tm_d_version);
	if (tm_d_author != "") by = strcat(by, "  ", sprintf(_("by %s"), tm_d_author));
	TM_SET(tm_title[0], strcat("^7", tm_d_title, "^8", by, "  ", TouchMods_stateText(tm_d_state)));
	if (tm_d_error != "")
		TM_SET(tm_detail[0], strcat("^1", tm_d_error));
	else if (tm_d_manifest != "")
		TM_SET(tm_detail[0], strcat("^1", tm_d_manifest));
	else
		TM_SET(tm_detail[0], strcat("^8", ((tm_d_desc != "") ? tm_d_desc : tm_sel)));
	TM_SET(tm_a[0], _("Reload"));
	TM_SET(tm_aact[0], strcat(p, "reload ", tm_sel));
	TM_SET(tm_b[0], _("Remove"));
	TM_SET(tm_bact[0], "@remove");
	int per = TM_SLOTS - 1, first, i, s;
	string what = "";
	switch (tm_sub)
	{
		case TM_P_PERMS:
			what = _("Permissions: each switch shows what it has. Tap one to allow it or take it back; allowing restarts a mod that stopped without it.");
			first = TouchMods_paging(tm_np, per);
			if (!tm_np)
				TM_SET(tm_title[1], strcat("^8", _("It asks for no permissions.")));
			for (s = 0; s < per; ++s)
			{
				i = first + s;
				if (i >= tm_np) break;
				TM_SET(tm_big[s + 1], (tm_pg[i] ? _("Allowed") : _("Not allowed")));
				tm_bigcolor[s + 1] = tm_pg[i] ? '0.55 1 0.55' : '1 0.85 0.5';
				TM_SET(tm_bigact[s + 1], sprintf("%spermission %s %s %s", p, tm_sel, (tm_pg[i] ? "revoke" : "grant"), tm_pn[i]));
				TM_SET(tm_title[s + 1], strcat("^7", tm_pn[i]));
				TM_SET(tm_detail[s + 1], strcat("^8", tm_pr[i]));
			}
			break;
		case TM_P_SETTINGS:
			what = _("Settings it declares (saved with your config).");
			first = TouchMods_paging(tm_ns, per);
			if (!tm_ns)
				TM_SET(tm_title[1], strcat("^8", _("It has no settings.")));
			for (s = 0; s < per; ++s)
			{
				i = first + s;
				if (i >= tm_ns) break;
				string cv = tm_sc[i], v = tm_sv[i], t = tm_st[i];
				TM_SET(tm_title[s + 1], strcat("^7", tm_sl[i]));
				TM_SET(tm_detail[s + 1], strcat("^8", cv, "  (", t, ((tm_sa[i] != "") ? strcat(": ", tm_sa[i]) : ""), ")"));
				if (t == "bool")
				{
					TM_SET(tm_big[s + 1], ((v == "1") ? _("ON") : _("OFF")));
					tm_bigcolor[s + 1] = (v == "1") ? '0.55 1 0.55' : '1 1 1';
					TM_SET(tm_bigact[s + 1], sprintf("set %s %s", cv, ((v == "1") ? "0" : "1")));
				}
				else if (t == "choice" || t == "number")
				{
					string prev = "", next = "";
					if (t == "choice")
					{
						int n = tokenizebyseparator(tm_sa[i], " ");
						int k;
						for (k = 0; k < n; ++k)
							if (argv(k) == v) break;
						if (k >= n) k = 0;
						prev = argv((k + n - 1) % n);
						next = argv((k + 1) % n);
					}
					else
					{
						int n2 = tokenizebyseparator(tm_sa[i], " ");
						float lo = stof(argv(0)), hi = stof(argv(1)), step = (n2 >= 3) ? stof(argv(2)) : 0;
						if (step <= 0) step = (hi - lo) / 10;
						prev = ftos(bound(lo, stof(v) - step, hi));
						next = ftos(bound(lo, stof(v) + step, hi));
					}
					TM_SET(tm_big[s + 1], v);
					TM_SET(tm_bigact[s + 1], sprintf("set %s \"%s\"", cv, next));
					TM_SET(tm_a[s + 1], "-");
					TM_SET(tm_aact[s + 1], sprintf("set %s \"%s\"", cv, prev));
					TM_SET(tm_b[s + 1], "+");
					TM_SET(tm_bact[s + 1], sprintf("set %s \"%s\"", cv, next));
				}
				else
				{
					TM_SET(tm_big[s + 1], v);
					TM_SET(tm_detail[s + 1], strcat("^8", cv, "  ", _("(text: change it in the console)")));
				}
			}
			break;
		case TM_P_LOG:
			what = _("Its recent output and errors (newest last).");
			{
				int lines = per * 2;
				int pages = max(1, ceil(tm_nlog / lines));
				if (tm_logend) tm_page = pages - 1;
				first = TouchMods_paging(tm_nlog, lines);
				if (!tm_nlog)
					TM_SET(tm_title[1], strcat("^8", _("No output yet.")));
				for (s = 0; s < lines; ++s)
				{
					i = first + s;
					if (i >= tm_nlog) break;
					if (s & 1) TM_SET(tm_detail[1 + floor(s / 2)], strcat("^7", tm_log[i]));
					else TM_SET(tm_title[1 + floor(s / 2)], strcat("^7", tm_log[i]));
				}
			}
			break;
		case TM_P_INFO:
			what = _("What it uses: API level, events, hooks, commands.");
			{
				int lines2 = per * 2;
				first = TouchMods_paging(tm_ninfo, lines2);
				for (s = 0; s < lines2; ++s)
				{
					i = first + s;
					if (i >= tm_ninfo) break;
					if (s & 1) TM_SET(tm_detail[1 + floor(s / 2)], tm_info[i]);
					else TM_SET(tm_title[1 + floor(s / 2)], tm_info[i]);
				}
			}
			break;
	}
	TM_SET(tm_summary, strcat(what, " ", TouchMods_pageText(((tm_sub == TM_P_PERMS) ? tm_np : ((tm_sub == TM_P_SETTINGS) ? tm_ns : ((tm_sub == TM_P_LOG) ? tm_nlog : tm_ninfo))), ((tm_sub >= TM_P_LOG) ? per * 2 : per)), TouchMods_lastMessage()));
	TouchMods_bar(0, _("< Back"), "@list", false);
	TouchMods_bar(1, _("Permissions"), "@sub 0", tm_sub == TM_P_PERMS);
	TouchMods_bar(2, _("Settings"), "@sub 1", tm_sub == TM_P_SETTINGS);
	TouchMods_bar(3, _("Output"), "@sub 2", tm_sub == TM_P_LOG);
	TouchMods_bar(4, _("Info"), "@sub 3", tm_sub == TM_P_INFO);
}

void TouchMods_modelInstall()
{
	TouchMods_readInstall();
	TM_SET(tm_header, (tm_target ? _("Install a server mod") : _("Install a client mod")));
	TM_SET(tm_summary, _("Nothing is written until you have seen what it asks for. It installs switched off, with no permissions."));
	// What it is: two choices, the chosen one lit.
	TM_SET(tm_big[0], (!tm_target ? _("Chosen") : _("Choose")));
	tm_bigcolor[0] = !tm_target ? '0.55 1 0.55' : '1 1 1';
	TM_SET(tm_bigact[0], "@target 0");
	TM_SET(tm_title[0], strcat("^7", _("Client mod (this device)")));
	TM_SET(tm_detail[0], strcat("^8", _("Runs in your own game here: HUD, sounds, controls. Only you see it.")));
	TM_SET(tm_big[1], (tm_target ? _("Chosen") : _("Choose")));
	tm_bigcolor[1] = tm_target ? '0.55 1 0.55' : '1 1 1';
	TM_SET(tm_bigact[1], "@target 1");
	TM_SET(tm_title[1], strcat("^7", _("Server mod (games you host)")));
	TM_SET(tm_detail[1], strcat("^8", _("Runs in the games this device hosts, for everyone in them; not in games you join.")));
	// Where it comes from: the field over slot 2's title.
	TM_SET(tm_detail[2], strcat("^8", _("A https:// link or a path to a .zip with init.lua and mod.txt. Add #sha256=<hash> to a link to pin the exact file.")));
	TM_SET(tm_a[2], _("Review"));
	TM_SET(tm_aact[2], "@review");
	TM_SET(tm_b[2], _("Clear"));
	TM_SET(tm_bact[2], "@clear");
	string imp = TouchMods_importStatus();
	if (imp != "")
	{
		TM_SET(tm_big[3], _("Pick file"));
		TM_SET(tm_bigact[3], "@pick");
		TM_SET(tm_title[3], strcat("^7", _("Or pick a .zip with the system file picker")));
		string shown = (substring(imp, 0, 7) == "picked ") ? _("Picked: checking it now...") : imp;
		TM_SET(tm_detail[3], strcat("^8", ((imp != " ") ? shown : _("It is checked and shown here before anything is installed."))));
	}
	else
	{
		TM_SET(tm_title[3], strcat("^7", _("Or a folder or .zip path on this computer")));
		TM_SET(tm_detail[3], strcat("^8", _("The launcher's Mods page can pick a file and send it here.")));
	}
	string st = tm_i_state;
	if (st == "downloading")
		TM_SET(tm_title[4], strcat("^3", tm_i_message));
	else if (st == "error")
		TM_SET(tm_title[4], strcat("^1", _("Refused:"), " ", tm_i_message));
	else if (st == "done")
		TM_SET(tm_title[4], strcat("^2", tm_i_message));
	if (st == "downloading")
	{
		TM_SET(tm_a[4], _("Stop"));
		TM_SET(tm_aact[4], "lua_install_cancel");
	}
	TouchMods_bar(0, _("< Back"), "@list", false);
}

void TouchMods_modelReview()
{
	TouchMods_readInstall();
	bool plugin = (tm_i_realm == "plugin");
	TM_SET(tm_header, (tm_i_update ? sprintf(_("Update this %s?"), plugin ? _("server mod") : _("mod")) : sprintf(_("Install this %s?"), plugin ? _("server mod") : _("mod"))));
	TM_SET(tm_big[0], (tm_i_update ? _("Update") : _("New")));
	tm_bigcolor[0] = '0.6 0.8 1';
	string by = "";
	if (tm_i_version != "") by = strcat(by, "  v", tm_i_version);
	if (tm_i_update) by = strcat(by, " ", sprintf(_("(installed: %s)"), ((tm_i_old != "") ? tm_i_old : "?")));
	if (tm_i_author != "") by = strcat(by, "  ", sprintf(_("by %s"), tm_i_author));
	TM_SET(tm_title[0], strcat("^7", tm_i_title, "^8", by));
	TM_SET(tm_detail[0], strcat("^8", tm_i_name, "  ", tm_i_files, "  sha256 ", substring(tm_i_sha, 0, 16), (tm_i_pinned ? strcat("  ^2", _("matches the pinned hash")) : ""), "  ^8", tm_i_source));
	int per = TM_SLOTS - 1;
	int first = TouchMods_paging(tm_nip, per);
	if (!tm_nip)
		TM_SET(tm_title[1], strcat("^7", _("It asks for no permissions.")));
	bool anynew = false;
	for (int k = 0; k < tm_nip; ++k)
		if (tm_ipt[k] == "new") anynew = true;
	for (int s = 0; s < per; ++s)
	{
		int i = first + s;
		if (i >= tm_nip) break;
		string t = tm_ipt[i];
		TM_SET(tm_big[s + 1], ((t == "new") ? _("NEW") : ((t == "kept") ? _("kept") : _("asks"))));
		tm_bigcolor[s + 1] = (t == "new") ? '1 0.8 0.4' : ((t == "kept") ? '0.55 1 0.55' : '1 1 1');
		TM_SET(tm_title[s + 1], strcat("^7", tm_ipn[i]));
		TM_SET(tm_detail[s + 1], strcat("^8", tm_ipr[i]));
	}
	TM_SET(tm_summary, strcat((anynew
		? _("This update asks for NEW permissions: they stay off until you grant them.")
		: (tm_i_update ? _("Permissions it had stay as they are.") : _("It asks for these; none is granted by installing, and it stays off."))),
		" ", TouchMods_pageText(tm_nip, per)));
	TouchMods_bar(0, _("Cancel"), "@cancelinstall", false);
	TouchMods_bar(4, (tm_i_update ? _("Update") : _("Install")), "@confirm", true);
}

void TouchMods_modelEnable()
{
	TouchMods_readDetail();
	string p = TouchMods_prefix(tm_realm);
	TM_SET(tm_header, sprintf(_("Turn on %s?"), ((tm_d_name == tm_sel) ? tm_d_title : tm_sel)));
	TouchMods_bar(0, _("Cancel"), "@back", false);
	if (tm_d_name != tm_sel)
	{
		TM_SET(tm_summary, _("Reading what it asks for..."));
		return;
	}
	int missing = 0, i;
	for (i = 0; i < tm_np; ++i)
		if (!tm_pg[i]) ++missing;
	if (tm_choosefor != tm_sel)
	{
		// a fresh review: what a mod with an api: line declares is preselected,
		// a legacy mod's extras are not
		strcpy(tm_choosefor, tm_sel);
		for (i = 0; i < 16; ++i)
			tm_choose[i] = (tm_d_api != "legacy");
	}
	if (!missing)
	{
		// nothing to grant: just turn it on
		localcmd(sprintf("\n%senable %s\n", p, tm_sel));
		tm_view = tm_backview;
		return;
	}
	TM_SET(tm_title[0], strcat("^7", _("It asks for these permissions. Each switch shows what it will get; tap one to change it.")));
	TM_SET(tm_detail[0], strcat("^8", _("You can take any of them back later in its Permissions page.")));
	int per = TM_SLOTS - 1;
	int first = TouchMods_paging(tm_np, per);
	string grant = "";
	for (i = 0; i < tm_np; ++i)
		if (!tm_pg[i] && tm_choose[i])
			grant = strcat(grant, ((grant != "") ? "," : ""), tm_pn[i]);
	for (int s = 0; s < per; ++s)
	{
		i = first + s;
		if (i >= tm_np) break;
		if (tm_pg[i])
		{
			TM_SET(tm_big[s + 1], _("Allowed"));
			tm_bigcolor[s + 1] = '0.55 1 0.55';
		}
		else
		{
			TM_SET(tm_big[s + 1], (tm_choose[i] ? _("Allowed") : _("Not allowed")));
			tm_bigcolor[s + 1] = (tm_choose[i] ? '0.55 1 0.55' : '1 0.85 0.5');
			TM_SET(tm_bigact[s + 1], sprintf("@choose %d", i));
		}
		TM_SET(tm_title[s + 1], strcat("^7", tm_pn[i]));
		TM_SET(tm_detail[s + 1], strcat("^8", tm_pr[i]));
	}
	TM_SET(tm_summary, strcat(sprintf(_("%d not allowed yet."), missing), " ", TouchMods_pageText(tm_np, per)));
	if (grant != "")
		TouchMods_bar(4, _("Allow, turn on"), sprintf("%spermission %s grant %s\n%senable %s", p, tm_sel, grant, p, tm_sel), true);
	else
		TouchMods_bar(4, _("Turn on"), sprintf("%senable %s", p, tm_sel), true);
	TouchMods_bar(3, _("Allow all"), "@chooseall", false);
}

void TouchMods_modelRemove()
{
	TouchMods_readDetail();
	TM_SET(tm_header, sprintf(_("Remove %s?"), ((tm_d_name == tm_sel) ? tm_d_title : tm_sel)));
	TM_SET(tm_summary, _("The game keeps running. Removing cannot be undone."));
	TM_SET(tm_title[0], strcat("^7", _("Its folder, its permissions and where it came from are deleted.")));
	TM_SET(tm_detail[0], strcat("^8", _("Install it again later and it starts off, with nothing granted.")));
	TM_SET(tm_big[1], (tm_keepdata ? _("Keep") : _("Delete")));
	tm_bigcolor[1] = tm_keepdata ? '0.55 1 0.55' : '1 0.8 0.4';
	TM_SET(tm_bigact[1], "@keep");
	TM_SET(tm_title[1], strcat("^7", _("Its saved data (the data/ folder)")));
	TM_SET(tm_detail[1], strcat("^8", (tm_keepdata ? _("is kept: a reinstall finds it again") : _("is deleted too"))));
	TouchMods_bar(0, _("Cancel"), "@detail_back", false);
	TouchMods_bar(4, _("Remove"), "@doremove", true);
}

void TouchMods_modelText()
{
	TM_SET(tm_header, tm_fullhead);
	TM_SET(tm_summary, "");
	TouchMods_bar(0, _("< Back"), "@textback", false);
}

void TouchMods_run(string act);

// a view change only: never confirm, remove or a console command
void TouchMods_goto(string step)
{
	if (step == "" || substring(step, 0, 7) == "confirm" || substring(step, 0, 8) == "doremove" || substring(step, 0, 13) == "cancelinstall" || substring(step, 0, 6) == "review" || substring(step, 0, 4) == "pick")
		return;
	TouchMods_run(strcat("@", step));
}

void TouchMods_update()
{
	if (tm_frame == time)
		return;
	tm_frame = time;
	// the engine's install state moves the view along
	TouchMods_readInstall();
	if (tm_i_state != tm_lastinstall)
	{
		strcpy(tm_lastinstall, tm_i_state);
		if (tm_i_state == "review")
		{
			tm_view = TM_V_REVIEW;
			tm_page = 0;
		}
		else if (tm_i_state == "downloading" || tm_i_state == "error")
			tm_view = TM_V_INSTALL;
		else if (tm_i_state == "done" && (tm_view == TM_V_REVIEW || tm_view == TM_V_INSTALL))
		{
			tm_realm = (tm_i_realm == "plugin") ? 1 : 0;
			tm_view = TM_V_LIST;
			tm_page = 0;
		}
	}
	if (tm_view == TM_V_REVIEW && tm_i_state != "review")
		tm_view = TM_V_LIST;
	// Android: a picked file waits in luastate/incoming/
	string imp = TouchMods_importStatus();
	if (substring(imp, 0, 7) == "picked " && imp != tm_lastpicked)
	{
		strcpy(tm_lastpicked, imp);
		// "picked <time> <path>": the time makes picking the same file again a new request
		string rest = substring(imp, 7, -1);
		int sp = strstrofs(rest, " ", 0);
		string path = (sp >= 0) ? substring(rest, sp + 1, -1) : rest;
		if (strstrofs(path, "\"", 0) < 0 && strstrofs(path, ";", 0) < 0 && strstrofs(path, "$", 0) < 0)
			localcmd(sprintf("\n%sinstall \"%s\"\n", TouchMods_prefix(tm_target), path));
	}
	// _lua_mods_goto "detail hud_clock,sub 2": opens a view from outside the
	// menu (the launcher, tests); views only, it never runs a command
	string go = cvar_string("_lua_mods_goto");
	if (go != "")
	{
		cvar_set("_lua_mods_goto", "");
		int ng = tokenizebyseparator(go, ",");
		string g0 = strzone(argv(0)), g1 = strzone((ng > 1) ? argv(1) : ""), g2 = strzone((ng > 2) ? argv(2) : "");
		TouchMods_goto(g0);
		TouchMods_goto(g1);
		TouchMods_goto(g2);
		strunzone(g0);
		strunzone(g1);
		strunzone(g2);
	}
	TouchMods_clearSlots();
	switch (tm_view)
	{
		case TM_V_LIST: TouchMods_modelList(); break;
		case TM_V_DETAIL: TouchMods_modelDetail(); break;
		case TM_V_INSTALL: TouchMods_modelInstall(); break;
		case TM_V_REVIEW: TouchMods_modelReview(); break;
		case TM_V_REMOVE: TouchMods_modelRemove(); break;
		case TM_V_ENABLE: TouchMods_modelEnable(); break;
		case TM_V_TEXT: TouchMods_modelText(); break;
	}
}

// ---------------------------------------------------------------------------
// Actions

void TouchMods_select(string name)
{
	strcpy(tm_sel, name);
	cvar_set("_lua_select", strcat((tm_realm ? "plugin " : "mod "), name));
}

bool TouchMods_safe(string s)
{
	return s != "" && strstrofs(s, "\"", 0) < 0 && strstrofs(s, ";", 0) < 0 && strstrofs(s, "$", 0) < 0 && strstrofs(s, "\n", 0) < 0 && strstrofs(s, "\\", 0) < 0;
}

void TouchMods_run(string act)
{
	if (act == "")
		return;
	if (substring(act, 0, 1) != "@")
	{
		localcmd(strcat("\n", act, "\n"));
		if (tm_view == TM_V_ENABLE)
			tm_view = tm_backview;
		return;
	}
	int n = tokenizebyseparator(act, " ");
	string verb = argv(0), arg = (n > 1) ? argv(1) : "";
	switch (verb)
	{
		case "@list": tm_view = TM_V_LIST; tm_page = 0; cvar_set("_lua_select", ""); break;
		case "@realm": tm_realm = stof(arg); tm_page = 0; tm_view = TM_V_LIST; break;
		case "@page": tm_page += stof(arg); tm_logend = false; break;
		case "@detail": TouchMods_select(arg); tm_view = TM_V_DETAIL; tm_sub = TM_P_PERMS; tm_page = 0; break;
		case "@detail_back": tm_view = TM_V_DETAIL; break;
		case "@sub": tm_sub = stof(arg); tm_page = 0; tm_logend = (tm_sub == TM_P_LOG); break;
		case "@install": tm_view = TM_V_INSTALL; tm_target = tm_realm; tm_page = 0; break;
		case "@target": tm_target = (arg != "") ? (stof(arg) != 0) : !tm_target; break;
		case "@textback": tm_view = tm_textback; break;
		case "@howto": localcmd("\nopenurl https://github.com/dixonSolutions/Godnotic/blob/main/docs/MODDING.md\n"); break;
		case "@clear": if (tm_input) tm_input.setText(tm_input, ""); break;
		case "@review":
		{
			string src = tm_input ? tm_input.text : "";
			if (TouchMods_safe(src))
				localcmd(sprintf("\n%sinstall \"%s\"\n", TouchMods_prefix(tm_target), src));
			else
				cvar_set("_lua_select", cvar_string("_lua_select")); // nothing to do
			break;
		}
		case "@pick":
		{
			float fh = fopen(TOUCH_MODS_IMPORT_REQUEST_FILE, FILE_WRITE);
			if (fh >= 0)
			{
				fputs(fh, "import\n");
				fclose(fh);
			}
			break;
		}
		case "@confirm": localcmd("\nlua_install_confirm\n"); break;
		case "@cancelinstall": localcmd("\nlua_install_cancel\n"); tm_view = TM_V_INSTALL; break;
		case "@remove": tm_view = TM_V_REMOVE; tm_keepdata = false; break;
		case "@keep": tm_keepdata = !tm_keepdata; break;
		case "@turnon":
			tm_backview = (tm_view == TM_V_ENABLE) ? TM_V_LIST : tm_view;
			TouchMods_select(arg);
			strcpy(tm_choosefor, "");
			tm_view = TM_V_ENABLE;
			tm_page = 0;
			break;
		case "@choose": { int c = stof(arg); if (c >= 0 && c < 16) tm_choose[c] = !tm_choose[c]; break; }
		case "@chooseall": for (int c2 = 0; c2 < 16; ++c2) tm_choose[c2] = true; break;
		case "@back": tm_view = tm_backview; tm_page = 0; break;
		case "@doremove":
			localcmd(sprintf("\n%suninstall %s%s\n", TouchMods_prefix(tm_realm), tm_sel, (tm_keepdata ? " keepdata" : "")));
			tm_view = TM_V_LIST;
			cvar_set("_lua_select", "");
			break;
	}
}

// ---------------------------------------------------------------------------
// Widgets

void TouchMods_setText(entity me, string t)
{
	if (t == me.modText)
		return;
	strcpy(me.modText, t);
	me.setText(me, me.modText);
}

string TouchMods_buttonText(entity me)
{
	int s = me.modSlot;
	switch (me.modRole)
	{
		case TM_B_BIG: return tm_big[s];
		case TM_B_A: return tm_a[s];
		case TM_B_B: return tm_b[s];
		case TM_B_PREV: return (tm_prevact != "") ? "<" : "";
		case TM_B_NEXT: return (tm_nextact != "") ? ">" : "";
		case TM_B_BAR: return tm_bar[s];
	}
	return "";
}

string TouchMods_buttonAction(entity me)
{
	int s = me.modSlot;
	switch (me.modRole)
	{
		case TM_B_BIG: return tm_bigact[s];
		case TM_B_A: return tm_aact[s];
		case TM_B_B: return tm_bact[s];
		case TM_B_PREV: return tm_prevact;
		case TM_B_NEXT: return tm_nextact;
		case TM_B_BAR: return tm_baract[s];
	}
	return "";
}

void GodnoticModsButton_draw(entity me)
{
	TouchMods_update();
	string t = TouchMods_buttonText(me);
	string act = TouchMods_buttonAction(me);
	TouchMods_setText(me, t);
	// a label-like button (the review's NEW/asks) shows without taking taps
	me.disabled = ((t == "") || (act == "" && me.modRole != TM_B_BIG));
	if (me.modRole == TM_B_BIG)
		me.color = tm_bigcolor[me.modSlot];
	else if (me.modRole == TM_B_BAR)
		me.color = tm_barsel[me.modSlot] ? '0.6 0.85 1' : '1 1 1';
	if (t == "")
	{
		me.focusable = false;
		return;
	}
	SUPER(GodnoticModsButton).draw(me);
}

// Draws t wrapped in the label's box, at most maxlines lines (fewer when
// the box is shorter), centred when centre is set; "\n" starts a new
// paragraph. True when some of it was left out: the last line then ends
// in "...".
bool TouchMods_drawWrapped(entity me, string t, float maxlines, float scale, bool centre)
{
	vector fs = me.realFontSize * scale;
	float w = 1 - me.keepspaceLeft - me.keepspaceRight;
	if (fs.y <= 0 || w <= 0)
		return false;
	float lh = fs.y * 1.12;
	int fit = max(1, min(maxlines, floor((1 + 0.12 * fs.y) / lh)));
	fit = min(fit, 24);
	string lines[24];
	int n = 0;
	bool cut = false;
	string paras[8];
	int np = min(8, tokenizebyseparator(t, "\n"));
	for (int k = 0; k < np; ++k)
		paras[k] = strzone(argv(k));
	for (int k = 0; k < np && !cut; ++k)
	{
		getWrappedLine_remaining = (paras[k] == "") ? " " : paras[k];
		while (getWrappedLine_remaining)
		{
			string l = getWrappedLine(w, fs, draw_TextWidth_WithColors);
			if (n >= fit)
			{
				cut = true;
				break;
			}
			bool more = (getWrappedLine_remaining != "") || (k + 1 < np);
			if (n == fit - 1 && more)
			{
				// the last line that fits: the rest cut with an ellipsis
				string rest = (getWrappedLine_remaining != "") ? strcat(l, " ", getWrappedLine_remaining) : strcat(l, " ...");
				l = draw_TextShortenToWidth(rest, w, true, fs);
				getWrappedLine_remaining = string_null;
				cut = true;
			}
			lines[n] = strzone(l);
			++n;
		}
	}
	for (int k = 0; k < np; ++k)
		strunzone(paras[k]);
	float y = centre ? 0.5 * (1 - (n * lh - 0.12 * fs.y)) : 0;
	for (int i = 0; i < n; ++i)
	{
		draw_Text(eX * me.keepspaceLeft + eY * (y + i * lh), lines[i], fs, me.colorL, me.alpha, true);
		strunzone(lines[i]);
	}
	return cut;
}

void GodnoticModsLabel_draw(entity me)
{
	TouchMods_update();
	string t = "";
	switch (me.modRole)
	{
		case TM_L_TITLE: t = tm_title[me.modSlot]; break;
		case TM_L_DETAIL: t = tm_detail[me.modSlot]; break;
		case TM_L_SUMMARY: t = tm_summary; break;
		case TM_L_HEADER: t = tm_header; break;
		case TM_L_FULL: t = (tm_view == TM_V_TEXT) ? tm_fulltext : ""; break;
	}
	// the Install view's input field sits where slot 2's title is
	if (me.modRole == TM_L_TITLE && me.modSlot == 2 && tm_view == TM_V_INSTALL)
		t = "";
	// the text page shows its title and its text, nothing else
	if (tm_view == TM_V_TEXT && me.modRole != TM_L_FULL && me.modRole != TM_L_HEADER)
		t = "";
	TouchMods_setText(me, t);
	me.modCut = false;
	if (t == "")
	{
		me.focusable = false;
		return;
	}
	if (me.modRole == TM_L_HEADER)
	{
		SUPER(GodnoticModsLabel).draw(me);
		return;
	}
	float a = draw_alpha;
	if (me.disabled)
		draw_alpha *= me.disabledAlpha;
	// A detail or the summary wraps onto two smaller lines; the text page
	// fills its box.
	if (me.modRole == TM_L_FULL)
		TouchMods_drawWrapped(me, t, 24, 1, false);
	else if (me.modRole == TM_L_TITLE)
		me.modCut = TouchMods_drawWrapped(me, t, 1, 1, true);
	else
		me.modCut = TouchMods_drawWrapped(me, t, 2, 0.82, true);
	// A cut text is a button: it opens the whole of it.
	me.focusable = me.modCut;
	if (me.modCut && me.focused)
		draw_Fill('0 0 0', '1 1 0', '1 1 1', 0.06);
	draw_alpha = a;
}

void TouchMods_openText(entity me)
{
	string head = tm_header, body = "";
	switch (me.modRole)
	{
		case TM_L_TITLE:
		case TM_L_DETAIL:
			head = tm_title[me.modSlot];
			body = strcat(tm_title[me.modSlot], "\n", tm_detail[me.modSlot]);
			break;
		case TM_L_SUMMARY:
			body = tm_summary;
			break;
	}
	strcpy(tm_fullhead, strdecolorize(head));
	strcpy(tm_fulltext, body);
	if (tm_view != TM_V_TEXT)
		tm_textback = tm_view;
	tm_view = TM_V_TEXT;
	m_play_click_sound(MENU_SOUND_OPEN);
}

bool GodnoticModsLabel_mousePress(entity me, vector pos)
{
	me.modPressed = me.modCut;
	return me.modCut;
}

float GodnoticModsLabel_mouseRelease(entity me, vector pos)
{
	if (me.modPressed && pos.x >= 0 && pos.y >= 0 && pos.x <= 1 && pos.y <= 1)
		TouchMods_openText(me);
	me.modPressed = false;
	return 1;
}

bool TouchMods_inputShown()
{
	return tm_view == TM_V_INSTALL;
}

void GodnoticModsInput_draw(entity me)
{
	TouchMods_update();
	if (!TouchMods_inputShown())
	{
		me.focusable = false;
		me.disabled = true;
		return;
	}
	me.disabled = false;
	SUPER(GodnoticModsInput).draw(me);
}

bool GodnoticModsInput_mousePress(entity me, vector pos)
{
	if (!TouchMods_inputShown())
		return false;
	return SUPER(GodnoticModsInput).mousePress(me, pos);
}

float GodnoticModsInput_keyDown(entity me, float key, float ascii, float shift)
{
	if (!TouchMods_inputShown())
		return false;
	if (key == K_ENTER || key == K_KP_ENTER)
	{
		TouchMods_run("@review");
		return true;
	}
	return SUPER(GodnoticModsInput).keyDown(me, key, ascii, shift);
}

void TouchMods_click(entity btn, entity me)
{
	TouchMods_run(TouchMods_buttonAction(btn));
}

entity TouchMods_button(int slot, int role)
{
	entity e = NEW(GodnoticModsButton);
	e.configureXonoticButton(e, "", '0 0 0', string_null);
	e.modSlot = slot;
	e.modRole = role;
	e.onClick = TouchMods_click;
	e.onClickEntity = e;
	return e;
}

entity TouchMods_label(int slot, int role)
{
	entity e = NEW(GodnoticModsLabel);
	e.configureLabel(e, "", e.fontSize, 0);
	e.modSlot = slot;
	e.modRole = role;
	return e;
}

void TouchMods_fill(entity me)
{
	entity e;
	int s;
	registercvar("_lua_mods_goto", "", 0);
	// row 0: what this is; row 1: the summary and the page arrows
	me.TR(me);
		me.TD(me, 1, me.columns, e = TouchMods_label(0, TM_L_HEADER));
			e.isBold = true;
			e.colorL = SKINCOLOR_HEADER;
			e.align = 0.5;
	me.TR(me);
		me.TD(me, 1, 4.2, TouchMods_label(0, TM_L_SUMMARY));
		me.TD(me, 1.05, 0.9, TouchMods_button(0, TM_B_PREV));
		me.TD(me, 1.05, 0.9, TouchMods_button(0, TM_B_NEXT));
	// five finger-sized rows: a big switch, two lines of text, two buttons
	for (s = 0; s < TM_SLOTS; ++s)
	{
		me.gotoRC(me, 2.1 + s * 2.05, 0);
			me.TD(me, 1.9, 1.1, TouchMods_button(s, TM_B_BIG));
			me.TD(me, 0.95, 3.0, TouchMods_label(s, TM_L_TITLE));
			me.TD(me, 1.9, 0.95, TouchMods_button(s, TM_B_A));
			me.TD(me, 1.9, 0.95, TouchMods_button(s, TM_B_B));
		me.gotoRC(me, 2.1 + s * 2.05 + 0.95, 1.1);
			me.TD(me, 0.95, 3.0, TouchMods_label(s, TM_L_DETAIL));
	}
	// the Install view's link field, over slot 2's title line
	me.gotoRC(me, 2.1 + 2 * 2.05, 1.1);
		me.TD(me, 0.95, 3.0, e = NEW(GodnoticModsInput));
			e.configureXonoticInputBox(e, 0, string_null, _("A https:// link to a .zip, or a file or folder path"));
			e.maxLength = 255;
			tm_input = e;
	// the text page's text, over the rows
	me.gotoRC(me, 2.1, 0);
		me.TD(me, TM_SLOTS * 2.05 - 0.15, me.columns, TouchMods_label(0, TM_L_FULL));
	// the bar
	me.gotoRC(me, 12.4, 0);
	for (s = 0; s < TM_BARS; ++s)
		me.TD(me, 1.6, 1.2, TouchMods_button(s, TM_B_BAR));
}

void GodnoticModsDialog_fill(entity me)
{
	TouchMods_fill(me);
}

void TouchMods_ConfigureDialogs(entity mainwindow)
{
	entity i = godnotic_mods_dialog = NEW(GodnoticModsDialog);
	i.configureDialog(i);
	mainwindow.addItemCentered(mainwindow, i, i.intendedWidth * eX + i.intendedHeight * eY, SKINALPHAS_MAINMENU_z);
}

void TouchMods_OpenManager(entity btn, entity me)
{
	if (godnotic_mods_dialog)
		DialogOpenButton_Click(btn, godnotic_mods_dialog);
}

entity makeGodnoticModsTab()
{
	entity me = NEW(GodnoticModsTab);
	me.configureDialog(me);
	return me;
}

void GodnoticModsTab_fill(entity me)
{
	entity e;
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Mods")));
	me.TR(me);
		me.TD(me, 3, me.columns, e = makeXonoticTextLabel(0, _("Client mods change your own game on this device (HUD, sounds, controls). Server mods run in the games this device hosts. Each one starts switched off and gets only the permissions you allow it.")));
			e.allowWrap = true;
	me.gotoRC(me, 4.5, 1.5);
		me.TD(me, 1.6, 3, e = makeGodnoticButton(_("Open the Mods manager"), '0.6 0.85 1'));
			e.onClick = TouchMods_OpenManager;
			e.onClickEntity = me;
}
