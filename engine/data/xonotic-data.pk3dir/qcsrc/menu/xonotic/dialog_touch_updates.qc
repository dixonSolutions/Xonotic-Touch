#include "dialog_touch_updates.qh"

#include "textlabel.qh"
#include "button.qh"
#include "checkbox.qh"
#include "progressbar.qh"
#include "touch_update_util.qh"

const vector TOUCH_UPDATE_COLOR_PROGRESS = '0.35 0.75 0.35';
const vector TOUCH_UPDATE_COLOR_ERROR = '0.85 0.35 0.3';

entity makeXonoticTouchUpdatesDialog()
{
	entity me = NEW(XonoticTouchUpdatesDialog);
	me.configureDialog(me);
	return me;
}

void TouchUpdates_Check_Click(entity btn, entity dlg)
{
	dlg.requestFailed = !TouchUpdate_request("check");
	// Show the intent immediately. The platform layer will publish `checking`
	// within the second, but a button that looks inert until then reads as
	// broken on a touchscreen, where there is no cursor to show a press.
	if (!dlg.requestFailed)
		dlg.statusLabel.setText(dlg.statusLabel, _("Checking for updates..."));
}

void TouchUpdates_Install_Click(entity btn, entity dlg)
{
	dlg.requestFailed = !TouchUpdate_request("install");
	if (!dlg.requestFailed)
		dlg.statusLabel.setText(dlg.statusLabel, _("Starting download..."));
}

void TouchUpdates_Skip_Click(entity btn, entity dlg)
{
	dlg.requestFailed = !TouchUpdate_request("skip");
	if (!dlg.requestFailed)
		dlg.statusLabel.setText(dlg.statusLabel, _("This version will be skipped."));
}

void TouchUpdates_PayloadCheck_Click(entity btn, entity dlg)
{
	dlg.payloadRequestFailed = !TouchAndySU_request("check");
	if (!dlg.payloadRequestFailed)
		dlg.payloadStatusLabel.setText(dlg.payloadStatusLabel, _("Checking payload updates..."));
}

void TouchUpdates_PayloadApply_Click(entity btn, entity dlg)
{
	dlg.payloadRequestFailed = !TouchAndySU_request("apply");
	if (!dlg.payloadRequestFailed)
		dlg.payloadStatusLabel.setText(dlg.payloadStatusLabel, _("Applying staged payload..."));
}

void XonoticTouchUpdatesDialog_showNotify(entity me)
{
	// Re-read on open so the dialog never shows the previous visit's state, and
	// ask for a check straight away: a player opening this screen is asking the
	// question the check answers.
	TouchUpdate_read();
	me.nextPoll = 0;
	me.requestFailed = false;
	me.payloadRequestFailed = false;
	if (TouchAndySU_state() != "unavailable" && TouchAndySU_state() != "incompatible")
	{
		if (TouchUpdate_autoEnabled() && !TouchAndySU_busy())
			TouchAndySU_request("check");
	}
	else if (TouchUpdate_canSelfInstall() && !TouchUpdate_busy())
		TouchUpdate_request("check");
}

void XonoticTouchUpdatesDialog_fill(entity me)
{
	entity e;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("This build")));
	me.TR(me);
		me.TD(me, 1, me.columns, me.versionLabel = makeXonoticTextLabel(0, TouchUpdate_versionLine()));
	me.TR(me);
		me.TD(me, 1, me.columns, me.statusLabel = makeXonoticTextLabel(0, TouchUpdate_shortStatus()));
	me.TR(me);
		me.TD(me, 1, me.columns, me.latestLabel = makeXonoticTextLabel(0, ""));

	me.TR(me);
		me.TD(me, 1, me.columns, me.progressBar = makeXonoticProgressBar());

	me.TR(me);
		me.TD(me, 1, me.columns, me.messageLabel = makeXonoticTextLabel(0, ""));
		me.messageLabel.allowWrap = true;

	me.TR(me);
	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("Updating")));
	me.TR(me);
		me.TD(me, 1, me.columns, me.autoBox = makeXonoticCheckBox_T(0, TOUCH_UPDATE_AUTO_CVAR,
			_("Update automatically"),
			_("Automatically check, download and apply compatible game updates.")));

	me.TR(me);
		me.TD(me, 1, 2, me.checkButton = makeXonoticButton(_("Check now"), '0 0 0'));
			me.checkButton.onClick = TouchUpdates_Check_Click;
			me.checkButton.onClickEntity = me;
		me.TD(me, 1, 2, me.installButton = makeXonoticButton(_("Install update"), '0 0 0'));
			me.installButton.onClick = TouchUpdates_Install_Click;
			me.installButton.onClickEntity = me;
		me.TD(me, 1, 2, me.skipButton = makeXonoticButton(_("Skip this version"), '0 0 0'));
			me.skipButton.onClick = TouchUpdates_Skip_Click;
			me.skipButton.onClickEntity = me;

	me.TR(me);
		me.TD(me, 1, me.columns, e = makeXonoticHeaderLabel(_("AndySU payload")));
	me.TR(me);
		me.TD(me, 1, me.columns, me.payloadStatusLabel = makeXonoticTextLabel(0, ""));
	me.TR(me);
		me.TD(me, 1, me.columns, me.payloadLatestLabel = makeXonoticTextLabel(0, ""));
	me.TR(me);
		me.TD(me, 1, me.columns, me.payloadProgressBar = makeXonoticProgressBar());
	me.TR(me);
		me.TD(me, 1, me.columns, me.payloadMessageLabel = makeXonoticTextLabel(0, ""));
		me.payloadMessageLabel.allowWrap = true;
	me.TR(me);
		me.TD(me, 1, 3, me.payloadCheckButton = makeXonoticButton(_("Check payload"), '0 0 0'));
			me.payloadCheckButton.onClick = TouchUpdates_PayloadCheck_Click;
			me.payloadCheckButton.onClickEntity = me;
		me.TD(me, 1, 3, me.payloadApplyButton = makeXonoticButton(_("Apply staged"), '0 0 0'));
			me.payloadApplyButton.onClick = TouchUpdates_PayloadApply_Click;
			me.payloadApplyButton.onClickEntity = me;

	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 1, me.columns, e = makeXonoticButton(_("OK"), '0 0 0'));
			e.onClick = Dialog_Close;
			e.onClickEntity = me;
}

void XonoticTouchUpdatesDialog_draw(entity me)
{
	if (time >= me.nextPoll)
	{
		me.nextPoll = time + 1;
		TouchUpdate_read();
	}

	// A flipped checkbox is only a menu-side cvar until someone tells the
	// platform layer, which is what actually stops the boot-time prompt.
	float wantAuto = cvar(TOUCH_UPDATE_AUTO_CVAR) ? 1 : 0;
	if (me.publishedAuto < 0)
	{
		me.publishedAuto = wantAuto;
	}
	else if (wantAuto != me.publishedAuto)
	{
		me.publishedAuto = wantAuto;
		me.requestFailed = !TouchUpdate_request(wantAuto ? "auto-on" : "auto-off");
	}

	string state = TouchUpdate_stateName();
	string payloadState = TouchAndySU_state();
	bool payloadAvailable = payloadState != "unavailable" && payloadState != "incompatible";
	bool payloadBusy = payloadState == "checking" || payloadState == "downloading"
		|| payloadState == "applying";
	bool managed = !TouchUpdate_canSelfInstall() || payloadAvailable;
	bool busy = TouchUpdate_busy();

	me.versionLabel.setText(me.versionLabel, TouchUpdate_versionLine());
	me.statusLabel.setText(me.statusLabel, TouchUpdate_shortStatus());
	string activeVersion = TouchAndySU_installed();
	if (payloadAvailable && activeVersion != "")
		me.payloadStatusLabel.setText(me.payloadStatusLabel,
			strcat(_("Installed payload:"), " ", activeVersion, " - ", payloadState));
	else if (payloadState == "unavailable")
		me.payloadStatusLabel.setText(me.payloadStatusLabel, _("AndySU payload updates are not enabled in this install."));
	else
		me.payloadStatusLabel.setText(me.payloadStatusLabel, strcat(_("Payload update status:"), " ", payloadState));
	string payloadLatest = TouchAndySU_latest();
	if (payloadLatest != "" && payloadLatest != activeVersion)
		me.payloadLatestLabel.setText(me.payloadLatestLabel, strcat(_("Latest payload:"), " ", payloadLatest));
	else
		me.payloadLatestLabel.setText(me.payloadLatestLabel, "");
	me.payloadMessageLabel.setText(me.payloadMessageLabel, TouchAndySU_message());
	if (me.payloadRequestFailed)
		me.payloadMessageLabel.setText(me.payloadMessageLabel,
			_("Could not write the AndySU request. Restart Xonotic Touch and try again."));

	string latest = TouchUpdate_latest();
	if (!managed && latest != "" && latest != TouchUpdate_installed())
		me.latestLabel.setText(me.latestLabel, strcat(_("Latest release:"), " ", latest));
	else
		me.latestLabel.setText(me.latestLabel, "");

	if (me.requestFailed)
	{
		me.messageLabel.setText(me.messageLabel,
			_("Could not reach the updater. Restart Xonotic Touch and try again."));
	}
	else if (managed && !payloadAvailable)
	{
		me.messageLabel.setText(me.messageLabel,
			_("This copy is installed by Flatpak or the OpenStore. Update it the same way you installed it."));
	}
	else if (managed)
	{
		me.messageLabel.setText(me.messageLabel,
			_("AndySU manages this install. Use the payload controls below; host APK updates are separate."));
	}
	else if (state == TOUCH_UPDATE_NEEDS_PERMISSION)
	{
		// Worth spelling out: the installer flashing open and shut is exactly
		// what this looks like from the player's side, and it reads as a bug.
		me.messageLabel.setText(me.messageLabel,
			_("Android needs 'install unknown apps' granted to Xonotic Touch before it will finish."));
	}
	else
	{
		me.messageLabel.setText(me.messageLabel, TouchUpdate_message());
	}

	if (state == TOUCH_UPDATE_DOWNLOADING)
	{
		me.progressBar.indeterminate = false;
		me.progressBar.color = TOUCH_UPDATE_COLOR_PROGRESS;
		me.progressBar.setProgress(me.progressBar, TouchUpdate_percent() / 100);
	}
	else if (state == TOUCH_UPDATE_CHECKING || state == TOUCH_UPDATE_INSTALLING)
	{
		me.progressBar.indeterminate = true;
		me.progressBar.color = TOUCH_UPDATE_COLOR_PROGRESS;
	}
	else if (state == TOUCH_UPDATE_ERROR)
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
	}

	if (payloadBusy)
	{
		me.payloadProgressBar.indeterminate = true;
		me.payloadProgressBar.color = TOUCH_UPDATE_COLOR_PROGRESS;
	}
	else if (payloadState == "error")
	{
		me.payloadProgressBar.indeterminate = false;
		me.payloadProgressBar.color = TOUCH_UPDATE_COLOR_ERROR;
		me.payloadProgressBar.setProgress(me.payloadProgressBar, 1);
	}
	else
	{
		me.payloadProgressBar.indeterminate = false;
		me.payloadProgressBar.color = TOUCH_UPDATE_COLOR_PROGRESS;
		me.payloadProgressBar.setProgress(me.payloadProgressBar, TouchAndySU_percent() / 100);
	}

	// Disable rather than hide. A control that vanishes mid-gesture hands the
	// press to whatever moves into its place, which on a touchscreen means the
	// player installs an update they were trying not to.
	bool actionable = TouchUpdate_isBehind();
	me.checkButton.disabled = (managed || busy);
	me.installButton.disabled = (managed || busy || !actionable);
	me.skipButton.disabled = (managed || busy || !actionable);
	me.autoBox.disabled = (managed && !payloadAvailable);
	me.payloadCheckButton.disabled = (!payloadAvailable || payloadBusy || TouchUpdate_autoEnabled() == false);
	me.payloadApplyButton.disabled = (!payloadAvailable || payloadBusy || payloadState != "staged");

	SUPER(XonoticTouchUpdatesDialog).draw(me);
}
