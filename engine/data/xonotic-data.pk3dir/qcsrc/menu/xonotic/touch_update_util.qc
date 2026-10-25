#include "touch_update_util.qh"
#include <lib/urllib.qh>

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

/** The auto-update value we have asked the platform layer for and not yet seen
 *  come back on line 8; -1 when nothing is in flight.
 *
 *  The platform layer serves requests on its own clock, so the next poll after
 *  a flip still reads the old preference. Mirroring that back would snap the
 *  checkbox to where it was, the dialog would read that as a second flip, and
 *  the request the player actually made would be overwritten by its opposite.
 */
float touchUpdate_requestedAuto = -1;

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
	// second copy that could disagree with it. A flip we have asked for and not
	// been answered on yet is the one case where the file is the stale copy.
	if (touchUpdate_requestedAuto >= 0)
	{
		if ((touchUpdate_auto ? 1 : 0) != touchUpdate_requestedAuto)
			return;
		touchUpdate_requestedAuto = -1;
	}
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

// What's new, as last fetched. Owned strings, like the status mirror above.
string touchUpdate_changelog;
string touchUpdate_changelogLatest;
bool touchUpdate_changelogPending;

.entity changelogBox;

void TouchUpdate_OnChangelog(entity fh, entity listener, int status)
{
	switch (status)
	{
		case URL_READY_CANREAD:
		{
			string text = "", latest = "";
			for (string s; (s = url_fgets(fh)); )
			{
				if (latest == "" && s != "")
					latest = s;
				// Version lines stand out; a literal ^ in a commit subject is
				// escaped so the box does not read it as a colour code.
				string shown = strreplace("^", "^^", s);
				if (s != "" && substring(s, 0, 2) != "- ")
					shown = strcat("^3", shown, "^7");
				text = (text == "") ? shown : strcat(text, "\n", shown);
			}
			url_fclose(fh);
			touchUpdate_changelogPending = false;
			strcpy(touchUpdate_changelog, text);
			strcpy(touchUpdate_changelogLatest, latest);
			if (listener && listener.changelogBox)
				listener.changelogBox.setText(listener.changelogBox,
					(text != "") ? text : _("No release notes yet."));
			break;
		}
		default:
		{
			// URL_READY_ERROR, or an HTTP status as its negative (-404 before
			// the first release that publishes the file).
			if (status >= 0)
				break;
			touchUpdate_changelogPending = false;
			if (listener && listener.changelogBox && TouchUpdate_changelog() == "")
				listener.changelogBox.setText(listener.changelogBox,
					strcat(_("Could not load what's new. It is on GitHub too:"), "\n", TOUCH_UPDATE_RELEASES_URL));
			break;
		}
	}
}

void TouchUpdate_fetchChangelog(entity listener)
{
	if (touchUpdate_changelogPending)
		return;
	touchUpdate_changelogPending = true;
	// _touch_update_changelog_url points it elsewhere: a mirror, or a local
	// file server when testing this screen.
	string url = cvar_string("_touch_update_changelog_url");
	if (url == "")
		url = TOUCH_UPDATE_CHANGELOG_URL;
	url_single_fopen(url, FILE_READ, TouchUpdate_OnChangelog, listener);
}

string TouchUpdate_changelog()
{
	return (touchUpdate_changelog != string_null) ? touchUpdate_changelog : "";
}

string TouchUpdate_changelogLatest()
{
	return (touchUpdate_changelogLatest != string_null) ? touchUpdate_changelogLatest : "";
}

string TouchUpdate_latestKnown()
{
	string latest = TouchUpdate_latest();
	string fromLog = TouchUpdate_changelogLatest();
	if (latest == "" || (fromLog != "" && vercmp(fromLog, latest) > 0))
		latest = fromLog;
	return latest;
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
		|| s == TOUCH_UPDATE_INSTALLING || s == TOUCH_UPDATE_NEEDS_PERMISSION
		|| s == TOUCH_UPDATE_READY || s == TOUCH_UPDATE_PAUSED)
		return true;
	if (touchUpdate_behind > 0)
		return true;
	// Flatpak and the OpenStore check for themselves, out of the menu's sight;
	// the changelog still says whether a newer build is out.
	string installed = TouchUpdate_installed();
	string fromLog = TouchUpdate_changelogLatest();
	return installed != "" && fromLog != "" && vercmp(fromLog, installed) > 0;
}

bool TouchUpdate_request(string token)
{
	float fh = fopen(TOUCH_UPDATE_REQUEST_FILE, FILE_WRITE);
	if (fh < 0)
		return false;

	fputs(fh, strcat(token, "\n"));
	fclose(fh);

	if (token == "auto-on")
		touchUpdate_requestedAuto = 1;
	else if (token == "auto-off")
		touchUpdate_requestedAuto = 0;

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
	if (s == TOUCH_UPDATE_READY)
		return sprintf(_("%s downloaded - installs next start"), TouchUpdate_latest());
	if (s == TOUCH_UPDATE_PAUSED)
		return sprintf(_("Download paused at %d%% - it resumes from there"), TouchUpdate_percent());
	if (s == TOUCH_UPDATE_NEEDS_PERMISSION)
		return _("Update ready - permission needed");
	if (s == TOUCH_UPDATE_ERROR)
		return _("Update check failed");
	if (s == TOUCH_UPDATE_MANAGED)
	{
		if (TouchUpdate_isBehind())
			return sprintf(_("%s is out - update through the system"), TouchUpdate_changelogLatest());
		return _("Updates managed by the system");
	}

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

float touchUpdate_bannerPoll;
bool touchUpdate_bannerFetched;

void TouchUpdate_drawBanner()
{
	if (time >= touchUpdate_bannerPoll)
	{
		touchUpdate_bannerPoll = time + 2;
		TouchUpdate_read();
		// Once a session: on Flatpak and the OpenStore the changelog is the
		// only place the menu learns of a newer build.
		if (!touchUpdate_bannerFetched)
		{
			touchUpdate_bannerFetched = true;
			TouchUpdate_fetchChangelog(NULL);
		}
	}
	if (!TouchUpdate_isBehind())
		return;
	string latest = TouchUpdate_latestKnown();
	if (latest == "")
		return;
	string line;
	if (TouchUpdate_stateName() == TOUCH_UPDATE_READY)
		line = sprintf(_("Xonotic Touch %s is downloaded and installs the next time you start"), latest);
	else if (TouchUpdate_stateName() == TOUCH_UPDATE_DOWNLOADING)
		line = sprintf(_("Downloading Xonotic Touch %s... %d%%"), latest, TouchUpdate_percent());
	else
		line = sprintf(_("Xonotic Touch %s is out: Settings > Updates"), latest);
	vector fs = ((1 / draw_scale.x) * eX + (1 / draw_scale.y) * eY) * 12;
	draw_CenterText(eX * 0.5 + eY * (1 - 1.6 * fs.y), line, fs, '1 0.85 0.4', 0.9, 0);
}
