#include "touch_data.qh"

#include <common/mapinfo.qh>
#include "touch_uib_api.qh"

float touchdata_mounts = -1;

bool TouchData_Active()
{
	string s = cvar_string("touch_data_state");
	return s == "fetching" || s == "paused" || s == "waiting" || s == "error";
}

bool TouchData_MapDownloading(string map)
{
	if (map == "" || !TouchData_Active())
		return false;
	return !fexists(strcat("maps/", map, ".bsp")) && fexists(strcat("maps/", map, ".mapinfo"));
}

void TouchData_Want(string map)
{
	if (TouchData_MapDownloading(map))
		localcmd(sprintf("\ntouch_data_want \"%s\"\n", map));
}

float TouchData_MapProgress(string map)
{
	float total = cvar("touch_data_want_total_kb");
	if (map == "" || map != cvar_string("touch_data_want") || total <= 0)
		return -1;
	return bound(0, cvar("touch_data_want_done_kb") / total, 1);
}

string TouchData_StatusLine()
{
	string s = cvar_string("touch_data_state");
	float done = cvar("touch_data_done_kb") / 1024, total = cvar("touch_data_total_kb") / 1024;
	string want = cvar_string("touch_data_want");
	if (s == "fetching" && want != "")
		return sprintf(_("Getting %s: %d of %d MB"), want, cvar("touch_data_want_done_kb") / 1024, ceil(cvar("touch_data_want_total_kb") / 1024));
	if (s == "fetching")
		return sprintf(_("Getting the rest of the maps: %d of %d MB"), done, total);
	if (s == "paused")
		return sprintf(_("Download paused: %d of %d MB"), done, total);
	if (s == "waiting")
		return sprintf(_("Waiting for a connection: %d of %d MB of the maps"), done, total);
	if (s == "error")
	{
		string m = cvar_string("touch_data_message");
		return (m != "") ? m : _("The download of the other maps stopped");
	}
	return "";
}

// ---------------------------------------------------------------- the hold dialog
// A "map" or a join the engine holds until its map is here (touch_data_pending):
// a small dialog over the menu with the map's progress and Cancel
// (touch_data_cancel). Back or a tap outside only hides it; the hold goes on.

string touchdata_hold_hidden;  // the map whose dialog the player hid
string touchdata_hold_built;   // what the dialog showed when it was last built
bool touchdata_hold_open;

bool TouchData_HoldUp()
{
	return strstrofs(strcat(" ", uibn_modal, " "), " data.hold ", 0) >= 0;
}

// "12 of 40 MB", or "" while the wanted map's size is not known yet
string TouchData_HoldAmount(string map)
{
	float total = cvar("touch_data_want_total_kb");
	if (map != cvar_string("touch_data_want") || total <= 0)
		return "";
	return sprintf(_("%d of %d MB"), floor(cvar("touch_data_want_done_kb") / 1024), ceil(total / 1024));
}

void TouchData_BuildHold()
{
	string map = cvar_string("touch_data_pending_map");
	bool join = substring(cvar_string("touch_data_pending"), 0, 8) == "connect ";
	if (!UIB_Screen("data.hold", _("Map download")))
		return;
	uib_s("presentation", "dialog");
	uib_b("dismissable", 1);
	string then = join ? _("Then you join the game.") : _("Then the game starts.");
	if (join && cvar_string("_uib_server_name") != "")
		then = sprintf(_("Then you join %s."), cvar_string("_uib_server_name"));
	string amount = TouchData_HoldAmount(map);
	string s = cvar_string("touch_data_state");
	if (s == "waiting" || s == "paused" || s == "error")
		amount = TouchData_StatusLine();
	float progress = TouchData_MapProgress(map);
	UIB_Open("status", "status");
		uib_s("icon", "download");
		uib_s("title", sprintf(_("Getting the map %s"), map));
		uib_s("body", (amount != "") ? strcat(then, " ", amount) : then);
		uib_b("live", 1);
		if (progress >= 0) uib_f("progress", progress);
		else uib_b("indeterminate", 1);
		uib_j("primary", UIB_AS("data.cancel", _("Cancel"), "close", "secondary"));
	UIB_Close();
	UIB_Done();
}

void TouchData_HoldClose()
{
	if (!TouchData_HoldUp())
		return;
	int n = tokenize_console(uibn_modal);
	string out = "";
	for (int i = 0; i < n; ++i)
		if (argv(i) != "data.hold")
			out = strcat(out, ((out != "") ? " " : ""), argv(i));
	strcpy(uibn_modal, out);
	UIB_Nav();
}

// Every bridge tick (UIB_Tick): open, refresh or close the dialog.
void TouchData_UIBTick()
{
	string map = cvar_string("touch_data_pending_map");
	bool up = TouchData_HoldUp();
	if (map == "")
	{
		TouchData_HoldClose();
		strcpy(touchdata_hold_hidden, "");
		touchdata_hold_open = false;
		return;
	}
	if (touchdata_hold_open && !up)
		strcpy(touchdata_hold_hidden, map);  // hidden by the player, or by a new stack
	touchdata_hold_open = up;
	if (uibn_context != "menu")
		return;
	if (!up)
	{
		if (touchdata_hold_hidden == map)
			return;
		strcpy(touchdata_hold_built, "");
		UIB_Modal("data.hold");
		touchdata_hold_open = true;
		if (!Menu_Active)
			m_display();
		up = true;
	}
	// rebuilt when what it shows changes: whole percent, MB, state
	string shown = sprintf("%s %d %s %s", map, floor(TouchData_MapProgress(map) * 100), TouchData_HoldAmount(map), cvar_string("touch_data_state"));
	if (shown == touchdata_hold_built)
		return;
	strcpy(touchdata_hold_built, shown);
	UIB_Build("data.hold");
}

bool TouchData_Action(string a)
{
	if (a != "data.cancel")
		return false;
	localcmd("\ntouch_data_cancel\n");
	TouchData_HoldClose();
	strcpy(touchdata_hold_hidden, "");
	touchdata_hold_open = false;
	uib_toast(_("Cancelled. The map keeps downloading with the rest."), "info", "download", 3);
	return true;
}

void TouchData_MenuFrame()
{
	float n = cvar("fs_mountpack_count");
	if (n == touchdata_mounts)
		return;
	if (touchdata_mounts >= 0)
	{
		// the .bsp search and everything cached from it are stale
		MapInfo_Enumerate();
		MapInfo_FilterGametype(MapInfo_CurrentGametype(), MapInfo_CurrentFeatures(), MapInfo_RequiredFlags(), MapInfo_ForbiddenFlags(), 0);
	}
	touchdata_mounts = n;
}
