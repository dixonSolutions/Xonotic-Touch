#include "touch_data.qh"

#include <common/mapinfo.qh>

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
