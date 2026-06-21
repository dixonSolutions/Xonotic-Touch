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

void TouchStartup_readProgress(entity dlg)
{
	float fh = fopen(TOUCH_ASSET_PROGRESS_FILE, FILE_READ);
	if (fh < 0)
	{
		dlg.progressStatus = "running";
		dlg.progressPercent = 0;
		dlg.progressMessage = _("Preparing download...");
		return;
	}

	string line = fgets(fh);
	string line2 = fgets(fh);
	string line3 = fgets(fh);
	fclose(fh);
	if (line == string_null || line == "")
	{
		dlg.progressStatus = "running";
		dlg.progressPercent = 0;
		dlg.progressMessage = _("Preparing download...");
		return;
	}

	dlg.progressStatus = line;
	dlg.progressPercent = stof(line2);
	dlg.progressMessage = (line3 != string_null) ? line3 : "";
}
