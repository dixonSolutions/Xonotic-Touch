#include "touch_startup_util.qh"

bool TouchStartup_assetsReady()
{
	if (cvar("_touch_assets_ready"))
		return true;
	if (fexists(TOUCH_ASSETS_READY_MARKER))
		return true;
	return false;
}

bool TouchStartup_assetFetchActive()
{
	return cvar("_touch_asset_fetch_active");
}

bool TouchStartup_assetFetchDialog_shouldShow()
{
	if (!cvar("vid_touchscreen"))
		return false;
	if (TouchStartup_assetsReady())
		return false;
	return TouchStartup_assetFetchActive() || !fexists(TOUCH_ASSETS_READY_MARKER);
}

bool TouchStartup_userProfileNeeded()
{
	return cvar_string("_cl_name") == cvar_defstring("_cl_name");
}

bool TouchStartup_touchSetupNeeded()
{
	return cvar("vid_touchscreen") && !cvar("touch_setup_done");
}

void TouchStartup_readProgress(string *status, float *percent, string *message)
{
	float fh = fopen(TOUCH_ASSET_PROGRESS_FILE, FILE_READ);
	if (fh < 0)
	{
		*status = "running";
		*percent = 0;
		*message = _("Preparing download...");
		return;
	}

	string line = fgets(fh);
	string line2 = fgets(fh);
	string line3 = fgets(fh);
	fclose(fh);
	if (line == string_null || line == "")
	{
		*status = "running";
		*percent = 0;
		*message = _("Preparing download...");
		return;
	}

	*status = line;
	*percent = stof(line2);
	*message = (line3 != string_null) ? line3 : "";
}
