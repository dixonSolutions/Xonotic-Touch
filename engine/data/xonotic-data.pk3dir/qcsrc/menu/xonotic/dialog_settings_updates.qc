#include "dialog_settings_updates.qh"

#include "textlabel.qh"
#include "textbox.qh"
#include "button.qh"
#include "commandbutton.qh"
#include "checkbox.qh"
#include "progressbar.qh"
#include "touch_update_util.qh"

const vector TOUCH_UPDATE_COLOR_PROGRESS = '0.35 0.75 0.35';
const vector TOUCH_UPDATE_COLOR_PAUSED = '0.85 0.7 0.3';
const vector TOUCH_UPDATE_COLOR_ERROR = '0.85 0.35 0.3';
// What's new is fetched again after this long with the tab open.
const float TOUCH_UPDATE_CHANGELOG_REFRESH = 600;

entity makeXonoticUpdatesSettingsTab()
{
	entity me = NEW(XonoticUpdatesSettingsTab);
	me.configureDialog(me);
	return me;
}

void TouchUpdates_Request(entity tab, string token, string shown)
{
	tab.requestFailed = !TouchUpdate_request(token);
	// Show the intent at once. The platform layer answers within the second,
	// but a button that looks inert until then reads as broken on a
	// touchscreen, where there is no cursor to show the press.
	if (!tab.requestFailed)
		tab.statusLabel.setText(tab.statusLabel, shown);
}

void TouchUpdates_Check_Click(entity btn, entity tab)
{
	TouchUpdate_fetchChangelog(tab);
	tab.changelogFetched = time;
	if (TouchUpdate_canSelfInstall())
		TouchUpdates_Request(tab, "check", _("Checking for updates..."));
}

void TouchUpdates_Download_Click(entity btn, entity tab)
{
	TouchUpdates_Request(tab, "download", _("Starting download..."));
}

void TouchUpdates_Install_Click(entity btn, entity tab)
{
	TouchUpdates_Request(tab, "install", (TouchUpdate_stateName() == TOUCH_UPDATE_READY)
		? _("Installing...") : _("Starting download..."));
}

void TouchUpdates_Skip_Click(entity btn, entity tab)
{
	TouchUpdates_Request(tab, "skip", _("This version will be skipped."));
}

void TouchUpdates_PayloadCheck_Click(entity btn, entity tab)
{
	tab.requestFailed = !TouchAndySU_request("check");
}

void TouchUpdates_PayloadApply_Click(entity btn, entity tab)
{
	tab.requestFailed = !TouchAndySU_request("apply");
}

void TouchUpdates_Releases_Click(entity btn, entity tab)
{
	localcmd(strcat("openurl ", TOUCH_UPDATE_RELEASES_URL, "\n"));
}

void TouchUpdates_Latest_Click(entity btn, entity tab)
{
	localcmd(strcat("openurl ", TOUCH_UPDATE_RELEASES_URL, "/latest\n"));
}

void XonoticUpdatesSettingsTab_showNotify(entity me)
{
	// Re-read on open, so the tab never shows the previous visit's state, and
	// ask for a check: a player opening this screen is asking what it answers.
	TouchUpdate_read();
	me.nextPoll = 0;
	me.requestFailed = false;
	if (TouchUpdate_canSelfInstall() && !TouchUpdate_busy()
		&& TouchUpdate_stateName() != TOUCH_UPDATE_READY && TouchUpdate_stateName() != TOUCH_UPDATE_PAUSED)
		TouchUpdate_request("check");
	if (time - me.changelogFetched > 30 || TouchUpdate_changelog() == "")
	{
		me.changelogFetched = time;
		TouchUpdate_fetchChangelog(me);
	}
	SUPER(XonoticUpdatesSettingsTab).showNotify(me);
}

void XonoticUpdatesSettingsTab_fill(entity me)
{
	entity e;

	// The Android bridge publishes before the menu starts. On a live build AndySU does the updating,
	// so the APK controls go. Other platforms keep the original layout.
	TouchUpdate_read();
	bool live = TouchUpdate_canSelfInstall() && TouchAndySU_state() != "unavailable" && TouchAndySU_state() != "incompatible";

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("This build")));
	me.TR(me);
		me.TD(me, 1, 2.5, me.versionLabel = makeXonoticTextLabel(0, TouchUpdate_versionLine()));
		me.TD(me, 1, 3.5, me.statusLabel = makeXonoticTextLabel(0, TouchUpdate_shortStatus()));
	me.TR(me);
		me.TD(me, 1, me.columns, me.progressBar = makeXonoticProgressBar());
	me.TR(me);
		me.TD(me, 1, me.columns, me.messageLabel = makeXonoticTextLabel(0, ""));
		me.messageLabel.allowWrap = true;
	me.TR(me);
		me.TD(me, 1, me.columns, me.autoBox = makeXonoticCheckBox_T(0, TOUCH_UPDATE_AUTO_CVAR,
			_("Update automatically"),
			live ? _("Download and apply updates in the background, while you play. A failed update rolls back on its own.")
				: _("Download new builds in the background on Wi-Fi. A downloaded update installs the next time you start Godnotic, or now with Install now. Android may still ask you to confirm.")));
	if (!live)
	{
	me.TR(me);
		me.TD(me, 1, 1.5, me.checkButton = makeXonoticButton_T(_("Check now"), '0 0 0',
			_("Ask GitHub for the newest release, and load what's new")));
			me.checkButton.onClick = TouchUpdates_Check_Click;
			me.checkButton.onClickEntity = me;
		me.TD(me, 1, 1.5, me.downloadButton = makeXonoticButton_T(_("Download"), '0 0 0',
			_("Download the new build now and install it the next time you start. A stopped download carries on from where it was")));
			me.downloadButton.onClick = TouchUpdates_Download_Click;
			me.downloadButton.onClickEntity = me;
		me.TD(me, 1, 1.5, me.installButton = makeXonoticButton_T(_("Install now"), '0 0 0',
			_("Install the new build now. Godnotic closes and comes back as the new version; settings, binds and maps are kept")));
			me.installButton.onClick = TouchUpdates_Install_Click;
			me.installButton.onClickEntity = me;
		me.TD(me, 1, 1.5, me.skipButton = makeXonoticButton_T(_("Skip version"), '0 0 0',
			_("Do not offer this version again; the next one will be")));
			me.skipButton.onClick = TouchUpdates_Skip_Click;
			me.skipButton.onClickEntity = me;
	}
	else
	{
		// One status line above (the version line's neighbour), and the two actions a live build has.
		me.payloadLabel = me.statusLabel;
		me.TR(me);
			me.TD(me, 1, 3, me.payloadCheck = makeXonoticButton(_("Check now"), '0 0 0'));
			me.payloadCheck.onClick = TouchUpdates_PayloadCheck_Click;
			me.payloadCheck.onClickEntity = me;
			me.TD(me, 1, 3, me.payloadApply = makeXonoticButton(_("Apply update"), '0 0 0'));
			me.payloadApply.onClick = TouchUpdates_PayloadApply_Click;
			me.payloadApply.onClickEntity = me;
	}
	me.TR(me);
		me.TD(me, 1, 3, e = makeXonoticButton_T(_("All releases on GitHub"), '0 0 0',
			_("Open the releases page: every build, its downloads and its notes")));
			e.onClick = TouchUpdates_Releases_Click;
			e.onClickEntity = me;
		me.TD(me, 1, 3, e = makeXonoticButton_T(_("Latest release on GitHub"), '0 0 0',
			_("Open the newest release: its APK, Flatpak and click downloads")));
			e.onClick = TouchUpdates_Latest_Click;
			e.onClickEntity = me;


	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("What's new")));
	me.TR(me);
		me.TD(me, me.rows - me.currentRow, me.columns, me.changelogBox = makeXonoticTextBox());
		me.changelogBox.allowColors = true;
		me.changelogBox.setText(me.changelogBox, _("Loading..."));
}

void XonoticUpdatesSettingsTab_draw(entity me)
{
	if (time >= me.nextPoll)
	{
		me.nextPoll = time + 1;
		TouchUpdate_read();
	}
	if (time - me.changelogFetched > TOUCH_UPDATE_CHANGELOG_REFRESH)
	{
		me.changelogFetched = time;
		TouchUpdate_fetchChangelog(me);
	}

	// A flipped checkbox is only a menu-side cvar until the platform layer is
	// told, which is what actually changes what it does in the background.
	float wantAuto = cvar(TOUCH_UPDATE_AUTO_CVAR) ? 1 : 0;
	if (me.publishedAuto < 0)
		me.publishedAuto = wantAuto;
	else if (wantAuto != me.publishedAuto)
	{
		me.publishedAuto = wantAuto;
		me.requestFailed = !TouchUpdate_request(wantAuto ? "auto-on" : "auto-off");
		if (me.payloadLabel) // one setting covers full app updates and live updates
			TouchAndySU_request(wantAuto ? "auto-on" : "auto-off");
	}

	string state = TouchUpdate_stateName();
	bool managed = !TouchUpdate_canSelfInstall();
	bool busy = TouchUpdate_busy();
	bool behind = TouchUpdate_isBehind();
	bool ready = (state == TOUCH_UPDATE_READY);

	if (me.payloadLabel)
	{
		string payloadState = TouchAndySU_state();
		bool payloadReady = false;
		if (payloadState == "staged")
			payloadReady = true;
		if (payloadState == "shell-update")
			payloadReady = true;
		bool payloadBlocked = TouchAndySU_busy();
		if (payloadState == "unavailable")
			payloadBlocked = true;
		if (payloadState == "incompatible")
			payloadBlocked = true;
		me.payloadLabel.setText(me.payloadLabel, TouchAndySU_statusText());
		me.payloadCheck.disabled = payloadBlocked;
		me.payloadApply.disabled = !payloadReady;
	}

	me.versionLabel.setText(me.versionLabel, TouchUpdate_versionLine());
	if (!me.payloadLabel)
	{
		string status = TouchUpdate_shortStatus();
		string latest = TouchUpdate_latestKnown();
		if (status == "" && latest != "")
			status = strcat(_("Latest release:"), " ", latest);
		me.statusLabel.setText(me.statusLabel, status);
	}

	if (me.requestFailed)
		me.messageLabel.setText(me.messageLabel,
			_("Could not reach the updater. Restart Godnotic and try again."));
	else if (managed)
		me.messageLabel.setText(me.messageLabel, behind
			? _("Update the way you installed it: flatpak update, or the OpenStore on Ubuntu Touch.")
			: _("Installed by Flatpak or the OpenStore, which keep it up to date."));
	else if (state == TOUCH_UPDATE_NEEDS_PERMISSION)
		// Worth spelling out: the installer flashing open and shut is what this
		// looks like from the player's side, and it reads as a bug.
		me.messageLabel.setText(me.messageLabel,
			_("Permission needed: allow Godnotic to install unknown apps in Android's settings, then press Install now."));
	else if (ready)
		me.messageLabel.setText(me.messageLabel,
			_("The update is downloaded. It installs the next time you start Godnotic, or now with Install now."));
	else
		me.messageLabel.setText(me.messageLabel, TouchUpdate_message());

	// An empty track on an up-to-date copy reads as stuck: the bar shows only while it has news.
	me.progressBar.alphaTrack = 0.15;
	me.progressBar.alphaBar = 0.9;
	if (me.payloadLabel)
	{
		me.progressBar.indeterminate = false;
		me.progressBar.color = TOUCH_UPDATE_COLOR_PROGRESS;
		me.progressBar.setProgress(me.progressBar, TouchAndySU_percent() / 100);
		if (TouchAndySU_state() != "downloading")
		{
			me.progressBar.alphaTrack = 0;
			me.progressBar.alphaBar = 0;
		}
	}
	else if (state == TOUCH_UPDATE_DOWNLOADING || state == TOUCH_UPDATE_PAUSED || ready)
	{
		me.progressBar.indeterminate = false;
		me.progressBar.color = (state == TOUCH_UPDATE_PAUSED) ? TOUCH_UPDATE_COLOR_PAUSED : TOUCH_UPDATE_COLOR_PROGRESS;
		me.progressBar.setProgress(me.progressBar, ready ? 1 : TouchUpdate_percent() / 100);
	}
	else if (state == TOUCH_UPDATE_CHECKING || state == TOUCH_UPDATE_INSTALLING)
	{
		me.progressBar.indeterminate = true;
		me.progressBar.color = TOUCH_UPDATE_COLOR_PROGRESS;
	}
	else if (state == TOUCH_UPDATE_ERROR || state == TOUCH_UPDATE_NEEDS_PERMISSION)
	{
		me.progressBar.indeterminate = false;
		me.progressBar.color = TOUCH_UPDATE_COLOR_ERROR;
		me.progressBar.setProgress(me.progressBar, 1);
	}
	else
	{
		me.progressBar.indeterminate = false;
		me.progressBar.color = TOUCH_UPDATE_COLOR_PROGRESS;
		me.progressBar.setProgress(me.progressBar, 0);
		me.progressBar.alphaTrack = 0;
		me.progressBar.alphaBar = 0;
	}

	// Disable rather than hide. A control that vanishes mid-gesture hands the
	// press to whatever moves into its place, which on a touchscreen means
	// installing an update the player was trying not to.
	if (me.checkButton)
	{
		me.checkButton.disabled = busy;
		me.downloadButton.disabled = (managed || busy || !behind || ready);
		me.installButton.disabled = (managed || busy || !behind);
		me.skipButton.disabled = (managed || busy || !behind);
	}
	else
	{
		// AppUpdater stands down on a live build: its version and status lines would be stale.
		me.versionLabel.setText(me.versionLabel, strcat(_("Godnotic"), " ", TouchAndySU_installed()));
		me.messageLabel.setText(me.messageLabel, "");
	}
	me.autoBox.disabled = managed;

	SUPER(XonoticUpdatesSettingsTab).draw(me);
}
