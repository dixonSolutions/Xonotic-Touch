#include "touch_update_util.qh"

// Parsed mirror of update-status.txt. Globals rather than an entity: the menu
// is single-threaded and there is exactly one updater, so an entity would only
// add a lifetime to get wrong.
//
// All four strings are owned (strcpy, never a bare assignment of a temporary):
// fgets returns engine-temporary strings that are recycled within the frame.
string touchUpdate_state;
string touchUpdate_installed;
string touchUpdate_latest;
string touchUpdate_message;
float touchUpdate_behind;
float touchUpdate_percent;
bool touchUpdate_auto;
bool touchUpdate_haveStatus;
bool touchUpdate_cvarReady;

/** The menu runs with no CSQC behind it, so touch_init.qc's registration has
 *  not happened when the main menu first draws. Without this the checkbox reads
 *  an unset cvar as 0 and shows auto-update off on every cold start.
 */
void TouchUpdate_ensureCvar()
{
	if (touchUpdate_cvarReady)
		return;
	touchUpdate_cvarReady = true;
	// Flags 0, like the other launcher-handshake cvars: the platform layer
	// persists the real preference and line 8 syncs it back on every read, so a
	// second archived copy here could only ever disagree with it.
	registercvar(TOUCH_UPDATE_AUTO_CVAR, "1", 0);
}

void TouchUpdate_read()
{
	TouchUpdate_ensureCvar();

	float fh = fopen(TOUCH_UPDATE_STATUS_FILE, FILE_READ);
	if (fh < 0)
	{
		// No platform layer has published anything. Not an error: a bare engine
		// launch with no launcher behind it looks exactly like this, and the
		// dialog renders that as "unknown" rather than inventing a version.
		touchUpdate_haveStatus = false;
		strcpy(touchUpdate_state, TOUCH_UPDATE_IDLE);
		strcpy(touchUpdate_installed, "");
		strcpy(touchUpdate_latest, "");
		strcpy(touchUpdate_message, "");
		touchUpdate_behind = 0;
		touchUpdate_percent = 0;
		return;
	}

	string format = fgets(fh);
	string state = fgets(fh);
	string installed = fgets(fh);
	string latest = fgets(fh);
	string behind = fgets(fh);
	string percent = fgets(fh);
	string message = fgets(fh);
	string autoPref = fgets(fh);
	fclose(fh);

	// An unknown format means a newer platform layer than this menu understands.
	// Stand down rather than guess at the line order — showing "unknown" is
	// recoverable, showing the wrong version is not.
	if (stof(format) != TOUCH_UPDATE_FORMAT)
	{
		touchUpdate_haveStatus = false;
		strcpy(touchUpdate_state, TOUCH_UPDATE_IDLE);
		return;
	}

	touchUpdate_haveStatus = true;
	strcpy(touchUpdate_state, (state != string_null && state != "") ? state : TOUCH_UPDATE_IDLE);
	strcpy(touchUpdate_installed, (installed != string_null) ? installed : "");
	strcpy(touchUpdate_latest, (latest != string_null) ? latest : "");
	strcpy(touchUpdate_message, (message != string_null) ? message : "");
	touchUpdate_behind = max(0, stof(behind));
	touchUpdate_percent = bound(0, stof(percent), 100);
	touchUpdate_auto = (autoPref != "off");

	// Keep the checkbox cvar in step with the platform's own preference, so the
	// toggle reads correctly on a fresh launch without the menu storing a
	// second copy that could disagree with it.
	if (cvar(TOUCH_UPDATE_AUTO_CVAR) != (touchUpdate_auto ? 1 : 0))
		cvar_set(TOUCH_UPDATE_AUTO_CVAR, touchUpdate_auto ? "1" : "0");
}

string TouchUpdate_stateName()
{
	return (touchUpdate_state != string_null) ? touchUpdate_state : TOUCH_UPDATE_IDLE;
}

string TouchUpdate_installed()
{
	return (touchUpdate_installed != string_null) ? touchUpdate_installed : "";
}

string TouchUpdate_latest()
{
	return (touchUpdate_latest != string_null) ? touchUpdate_latest : "";
}

float TouchUpdate_behind()
{
	return touchUpdate_behind;
}

float TouchUpdate_percent()
{
	return touchUpdate_percent;
}

string TouchUpdate_message()
{
	return (touchUpdate_message != string_null) ? touchUpdate_message : "";
}

bool TouchUpdate_autoEnabled()
{
	return touchUpdate_auto;
}

bool TouchUpdate_busy()
{
	string s = TouchUpdate_stateName();
	return s == TOUCH_UPDATE_CHECKING || s == TOUCH_UPDATE_DOWNLOADING
		|| s == TOUCH_UPDATE_INSTALLING;
}

bool TouchUpdate_canSelfInstall()
{
	// Anything the platform layer marks managed is updated by Flatpak or the
	// OpenStore, where an in-app installer would be both wrong and impossible.
	return TouchUpdate_stateName() != TOUCH_UPDATE_MANAGED;
}

bool TouchUpdate_isBehind()
{
	string s = TouchUpdate_stateName();
	if (s == TOUCH_UPDATE_AVAILABLE || s == TOUCH_UPDATE_DOWNLOADING
		|| s == TOUCH_UPDATE_INSTALLING || s == TOUCH_UPDATE_NEEDS_PERMISSION)
		return true;
	return touchUpdate_behind > 0;
}

bool TouchUpdate_request(string token)
{
	float fh = fopen(TOUCH_UPDATE_REQUEST_FILE, FILE_WRITE);
	if (fh < 0)
		return false;

	fputs(fh, strcat(token, "\n"));
	fclose(fh);
	return true;
}

string TouchUpdate_versionLine()
{
	string v = TouchUpdate_installed();
	if (v == "")
		return _("Xonotic Touch (development build)");
	return strcat(_("Xonotic Touch"), " ", v);
}

string TouchUpdate_shortStatus()
{
	string s = TouchUpdate_stateName();

	if (s == TOUCH_UPDATE_CHECKING)
		return _("Checking for updates...");
	if (s == TOUCH_UPDATE_DOWNLOADING)
		return sprintf(_("Downloading update... %d%%"), TouchUpdate_percent());
	if (s == TOUCH_UPDATE_INSTALLING)
		return _("Installing update...");
	if (s == TOUCH_UPDATE_NEEDS_PERMISSION)
		return _("Update ready - permission needed");
	if (s == TOUCH_UPDATE_ERROR)
		return _("Update check failed");
	if (s == TOUCH_UPDATE_MANAGED)
		return _("Updates managed by the system");

	if (TouchUpdate_behind() == 1)
		return _("1 version behind");
	if (TouchUpdate_behind() > 1)
		return sprintf(_("%d versions behind"), TouchUpdate_behind());

	if (s == TOUCH_UPDATE_AVAILABLE)
		return _("Update available");
	if (s == TOUCH_UPDATE_UPTODATE)
		return _("Up to date");
	return "";
}
