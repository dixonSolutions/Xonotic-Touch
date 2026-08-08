#include "touch_startup_util.qh"

/** The launcher checks `.assets-ready` before exec and passes the answer in.
 *
 * The menu cannot re-check that file itself (leading dot), so the wizard's own
 * completion is the only other thing that flips this cvar.
 */
bool TouchStartup_assetsReady()
{
	return cvar("_touch_assets_ready");
}

bool TouchStartup_assetFetchActive()
{
	return cvar("_touch_asset_fetch_active");
}

bool TouchStartup_assetFetchDialog_shouldShow()
{
	string status;
	float fh;

	if (!cvar("vid_touchscreen"))
		return false;

	// Live fetchd ticks win over a premature ready flag: minimum packs can be
	// marked installed while larger packs are still downloading.
	if (fexists(TOUCH_ASSET_PROGRESS_FILE))
	{
		fh = fopen(TOUCH_ASSET_PROGRESS_FILE, FILE_READ);
		if (fh >= 0)
		{
			status = fgets(fh);
			fclose(fh);
			if (status == TOUCH_FETCH_RUNNING || status == TOUCH_FETCH_DISCOVER
				|| status == TOUCH_FETCH_ERROR || status == TOUCH_FETCH_PAUSED)
				return true;
		}
	}

	if (TouchStartup_assetsReady())
		return false;
	// Only when the launcher started background resolution (or a progress file
	// already exists). Bare engine launches without the helper must not sit on
	// a forever "Checking..." screen with no worker behind it.
	if (TouchStartup_assetFetchActive())
		return true;
	return fexists(TOUCH_ASSET_PROGRESS_FILE);
}

bool TouchStartup_userProfileNeeded()
{
	// Explicit flag set by FirstRun "Save settings".
	if (cvar("touch_profile_done"))
		return false;

	string name = cvar_string("_cl_name");
	// Legacy migrate: a real name + stats choice means profile was finished
	// before touch_profile_done existed — do not force the wizard again.
	if (name != cvar_defstring("_cl_name")
		&& name != ""
		&& name != "player"
		&& name != "Player"
		&& cvar_string("cl_allow_uid2name") != "-1")
		return false;

	return true;
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
		// Launcher seeds this file before exec; missing means the worker has
		// not published a tick yet — keep the indeterminate checking state.
		strcpy(dlg.progressStatus, TOUCH_FETCH_DISCOVER);
		dlg.progressPercent = 0;
		strcpy(dlg.progressMessage, _("Checking your game data..."));
		return;
	}

	string status = fgets(fh);
	string percent = fgets(fh);
	string message = fgets(fh);
	fclose(fh);

	if (status == string_null || status == "")
	{
		strcpy(dlg.progressStatus, TOUCH_FETCH_DISCOVER);
		dlg.progressPercent = 0;
		strcpy(dlg.progressMessage, _("Checking your game data..."));
		return;
	}

	strcpy(dlg.progressStatus, status);
	dlg.progressPercent = bound(0, stof(percent), 100);
	strcpy(dlg.progressMessage, (message != string_null) ? message : "");
}

/** Ask packaging/start.sh to run the engine again.
 *
 * Returns false when the marker cannot be written; the caller then has to stay
 * in-process, because quitting without a marker would just close the app.
 */
bool TouchStartup_requestRelaunch()
{
	float fh = fopen(TOUCH_RELAUNCH_MARKER, FILE_WRITE);
	if (fh < 0)
		return false;

	fputs(fh, "requested by the Xonotic Touch setup wizard\n");
	fclose(fh);
	localcmd("quit\n");
	return true;
}

/** Mark that the download should keep running after the UI closes.
 *
 * Does not quit — the wizard shows a “downloading in background” state and
 * lets the user close when ready. packaging/start.sh reads the marker after
 * the engine exits.
 */
bool TouchStartup_requestBackgroundFetch()
{
	float fh = fopen(TOUCH_BACKGROUND_FETCH_MARKER, FILE_WRITE);
	if (fh < 0)
		return false;

	fputs(fh, "requested by the Xonotic Touch setup wizard\n");
	fclose(fh);
	return true;
}
