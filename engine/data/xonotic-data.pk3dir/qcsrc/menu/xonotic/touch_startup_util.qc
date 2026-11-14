#include "touch_startup_util.qh"
#include "touch_menu_layout.qh"
#include "touch_uib_api.qh"

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

/** A player who has not picked a name yet: the welcome asks for one.
 *
 * Nobody plays as the engine's default ("player"): that is the name every
 * new install shares. Once per install (touch_profile_done, set when the
 * welcome is saved); an install that already has a name of its own never
 * sees it. The language follows the system's (LanguageList_applySystemLanguage).
 */
bool TouchStartup_nameNeeded()
{
	if (cvar("touch_profile_done"))
		return false;
	string n = strtolower(strdecolorize(cvar_string("_cl_name")));
	n = strreplace(" ", "", n);
	return n == "" || n == "player" || n == "unnamedplayer"
		|| n == strtolower(strdecolorize(cvar_defstring("_cl_name")));
}

/** The stock menu's welcome (dialog_firstrun.qc), where the stock menu is what
 * the player sees: the web and Ubuntu Touch. The native interfaces (Android,
 * Linux) and the bridge renderer show the bridge's welcome dialog instead
 * (UIB_BuildWelcome in touch_uib_screens.qc).
 */
bool TouchStartup_userProfileNeeded()
{
	if (cvar("menu_touch_uib") || (UIB_EngineHasBridge() && uib_native()))
		return false;
	return TouchStartup_nameNeeded();
}

/** Statistics and identity on an install nobody asked, without a popup.
 *
 * Stock Xonotic asks every new player to accept its terms of service
 * (dialog_termsofservice.qc), and its defaults send match statistics with the
 * player ID to XonStat (cl_allow_uidtracking 1) on that acceptance. Godnotic
 * shows no terms popup (TouchStartup_skipTermsOfService), so nobody has
 * accepted anything: statistics start off, the player name is not shared and
 * the in-game "use your nickname?" question is answered no. Settings > Mobile >
 * Privacy turns them on and shows the terms.
 *
 * Once per install (touch_privacy_defaults_done). An install that accepted the
 * terms in an earlier version (_termsofservice_accepted > 0), or that already
 * chose to share its name (cl_allow_uid2name 1), keeps what it has.
 */
void TouchStartup_applyPrivacyDefaults()
{
	if (cvar("touch_privacy_defaults_done"))
		return;
	if (cvar("_termsofservice_accepted") <= 0 && cvar_string("cl_allow_uid2name") != "1")
	{
		cvar_set("cl_allow_uidtracking", "0");
		cvar_set("cl_allow_uidranking", "0");
		if (cvar_string("cl_allow_uid2name") == "-1")
			cvar_set("cl_allow_uid2name", "0");
	}
	localcmd("seta touch_privacy_defaults_done 1\nsaveconfig\n");
}

/** No terms of service popup, on first start or when the terms change.
 *
 * Godnotic is not Xonotic's release and does not make playing conditional on
 * accepting XonStat's terms. They stay readable from Credits and Settings >
 * Mobile > Privacy (GodnoticTerms_Open), next to the statistics switch they
 * are about. _termsofservice_accepted is left as it is: nobody accepted
 * anything here.
 */
bool TouchStartup_skipTermsOfService()
{
	return true;
}

/** XonStat requests that carry the player ID, from the menu (the Profile
 * statistics list): only when the player allows statistics. */
bool TouchStartup_statsAllowed()
{
	return cvar("cl_allow_uidtracking") == 1;
}

bool TouchStartup_touchSetupNeeded()
{
	return cvar("vid_touchscreen") && !cvar("touch_setup_done");
}

/** First touch launch: take the default controls without asking.
 *
 * There used to be a setup dialog here, and players did not want a popup
 * before they could play: everything it offered is in Settings -> Mobile. Its
 * "Save & continue" with nothing picked kept the preset the launcher had
 * already exec'd (Standard), saved the layout file and marked setup done; this
 * does the same, once. An install that already has touch_setup_done is left
 * alone.
 */
void TouchStartup_applyTouchDefaults()
{
	if (!TouchStartup_touchSetupNeeded())
		return;
	TouchMenu_WriteLayoutFile("touch.layout.cfg");
	// seta: the client VM that registers it has not run yet.
	localcmd("seta touch_setup_done 1\nsaveconfig\n");
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

	fputs(fh, "requested by the Godnotic setup wizard\n");
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

	fputs(fh, "requested by the Godnotic setup wizard\n");
	fclose(fh);
	return true;
}
