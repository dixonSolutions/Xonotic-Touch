#include "dialog_touch_asset_fetch.qh"

#include "textlabel.qh"
#include "progressbar.qh"
#include "bigbutton.qh"
#include "image.qh"
#include "util.qh"
#include "mainwindow.qh"

// Hold the finished state on screen briefly so the last thing the user sees is
// success rather than the window disappearing mid-progress.
const float TOUCH_ASSET_FETCH_DONE_DWELL = 1.2;
const vector TOUCH_ASSET_FETCH_COLOR_ERROR = '1 0.35 0.3';
const vector TOUCH_ASSET_FETCH_COLOR_PROGRESS = '0 0.5 1';
// Fully cover the main menu — setup is a fullscreen surface, not a card over
// a half-broken game.
const float TOUCH_ASSET_FETCH_SCRIM_ALPHA = 1;
const string TOUCH_ASSET_FETCH_LOGO = "/gfx/xonotic_logo";
const string TOUCH_MUSIC_READY_MARKER = "touch/music-ready.txt";
// Default menu_cdtrack (xonotic-common.cfg) — game-open theme, not in-match BGM.
const string TOUCH_MENU_THEME_TRACK = "rising-of-the-phoenix";
const string TOUCH_MENU_THEME_FILE = "sound/cdtracks/rising-of-the-phoenix.ogg";

void TouchAssetFetch_finish(entity me)
{
	cvar_set("_touch_assets_ready", "1");
	cvar_set("_touch_asset_fetch_active", "0");

	// Packs that appear after the engine built its search path stay invisible,
	// so hand back to the launcher and come up again with everything mounted.
	if (TouchStartup_requestRelaunch())
		return;

	// No writable marker (unpackaged run): rebuild the search path in place and
	// continue the startup chain instead of stranding the user here.
	localcmd("fs_rescan\n");
	main.firstDraw = true;
	Dialog_Close(NULL, me);
}

/** Single primary CTA: Download in background → Close window → Try again. */
void TouchAssetFetch_Primary_Click(entity btn, entity me)
{
	if (cvar("_touch_asset_fetch_error"))
	{
		if (TouchStartup_requestRelaunch())
			return;
		me.relaunchFailed = true;
		return;
	}

	if (me.backgroundMode || cvar("_touch_asset_fetch_background"))
	{
		// Ignore the release half of the same tap that armed background mode.
		if (time < me.backgroundCloseArmedTime)
			return;
		// Re-assert the marker so start.sh never treats this quit as a full stop.
		TouchStartup_requestBackgroundFetch();
		localcmd("quit\n");
		return;
	}

	// Update UI first so a tap always gives feedback even if the marker write fails.
	me.backgroundMode = true;
	me.backgroundCloseArmedTime = time + 0.75;
	cvar_set("_touch_asset_fetch_background", "1");
	if (me.headlineLabel)
		me.headlineLabel.setText(me.headlineLabel, _("Downloading in background"));
	if (me.statusLabel)
		me.statusLabel.setText(me.statusLabel,
			_("Downloading in background — you can close this window."));
	if (me.hintLabel)
		me.hintLabel.setText(me.hintLabel,
			_("The tray keeps the download and notifies you when game data is ready."));
	if (me.primaryButton)
		me.primaryButton.setText(me.primaryButton, _("Close window"));

	if (!TouchStartup_requestBackgroundFetch())
		me.backgroundFailed = true;
	else
		me.backgroundFailed = false;
}

void TouchAssetFetch_syncPrimaryButton(entity me, bool isError, bool isDone)
{
	string want;

	if (!me.primaryButton)
		return;

	// Keep focusable for touch: never disable this control. Invisible disabled
	// siblings used to sit underneath and steal itemFromPoint hits.
	me.primaryButton.disabled = false;
	me.primaryButton.focusable = true;

	if (isDone)
		want = _("Please wait…");
	else if (isError)
		want = _("Try again");
	else if (me.backgroundMode || cvar("_touch_asset_fetch_background"))
		want = _("Close window");
	else
		want = _("Download in background");

	// Avoid setText every frame — it can disturb press/focus on some skins.
	if (me.primaryButton.text != want)
		me.primaryButton.setText(me.primaryButton, want);
}

void TouchAssetFetch_setErrorState(bool isError)
{
	if (cvar("_touch_asset_fetch_error") == isError)
		return;
	cvar_set("_touch_asset_fetch_error", isError ? "1" : "0");
}

/** Solid fullscreen cover — no menu chrome and no game backdrop peeking through. */
void TouchAssetFetch_drawScrim(entity me)
{
	vector saveShift = draw_shift;
	vector saveScale = draw_scale;
	vector saveFontscale = draw_fontscale;
	float saveAlpha = draw_alpha;

	draw_reset_full();
	draw_Fill('0 0 0', eX * realconwidth + eY * realconheight, '0.06 0.07 0.1', TOUCH_ASSET_FETCH_SCRIM_ALPHA);

	draw_shift = saveShift;
	draw_scale = saveScale;
	draw_fontscale = saveFontscale;
	draw_alpha = saveAlpha;
}

/** Start the game-open theme once the track (or music-ready marker) is present.
 *
 * The shell fetches rising-of-the-phoenix.ogg early (~7 MB) into
 * xonotic-touch-theme.pk3dir so BGM is not blocked on the full music zip.
 */
void TouchAssetFetch_tryMusic(entity me)
{
	if (me.musicStarted)
		return;
	if (!fexists(TOUCH_MUSIC_READY_MARKER) && !fexists(TOUCH_MENU_THEME_FILE))
		return;
	// Prefer the canonical open-menu track; fall back to the user's menu_cdtrack.
	cvar_set("menu_cdtrack", TOUCH_MENU_THEME_TRACK);
	localcmd("fs_rescan; defer 0.4 \"cd loop rising-of-the-phoenix\"\n");
	me.musicStarted = true;
}

void TouchAssetFetch_updateWidgets(entity me, bool isError, bool isDiscover, bool isDone)
{
	string message = (me.progressMessage) ? me.progressMessage : "";

	if (isDone)
	{
		me.progressBar.indeterminate = false;
		me.progressBar.color = TOUCH_ASSET_FETCH_COLOR_PROGRESS;
		me.progressBar.setProgress(me.progressBar, 1);
		if (me.percentLabel)
			me.percentLabel.setText(me.percentLabel, "100%");
		me.statusLabel.setText(me.statusLabel, _("Game data ready — restarting Xonotic Touch"));
		me.hintLabel.setText(me.hintLabel, _("Setup is complete."));
		TouchAssetFetch_syncPrimaryButton(me, false, true);
		return;
	}

	if (isError)
	{
		me.progressBar.indeterminate = false;
		me.progressBar.color = TOUCH_ASSET_FETCH_COLOR_ERROR;
		me.progressBar.setProgress(me.progressBar, 1);
		if (me.percentLabel)
			me.percentLabel.setText(me.percentLabel, "");
		me.statusLabel.setText(me.statusLabel, (message != "") ? message : _("Download failed."));
		if (me.relaunchFailed)
			me.hintLabel.setText(me.hintLabel,
				_("Close and reopen Xonotic Touch to try the download again."));
		else
			me.hintLabel.setText(me.hintLabel,
				_("Check your connection and free space, then tap Try again."));
		TouchAssetFetch_syncPrimaryButton(me, true, false);
		return;
	}

	me.progressBar.color = TOUCH_ASSET_FETCH_COLOR_PROGRESS;

	// Determinate whenever the shell published a real percent (download bytes,
	// Flatpak copy steps, discovery checkpoints). Sweep only for "still thinking".
	bool havePercent = (me.progressPercent > 0);
	if (isDiscover && !havePercent && !me.backgroundMode)
	{
		me.progressBar.indeterminate = true;
		if (me.percentLabel)
			me.percentLabel.setText(me.percentLabel, "");
		me.statusLabel.setText(me.statusLabel,
			(message != "") ? message : _("Looking for game data on this device..."));
		if (me.backgroundFailed)
			me.hintLabel.setText(me.hintLabel,
				_("Could not switch to background download. Keep the app open, or close and reopen to resume."));
		else
			me.hintLabel.setText(me.hintLabel,
				_("Leave this open, or tap Download in background — the tray keeps the download and notifies you when it finishes."));
		TouchAssetFetch_syncPrimaryButton(me, false, false);
		return;
	}

	me.progressBar.indeterminate = false;
	me.progressBar.setProgress(me.progressBar, me.progressPercent / 100);
	if (me.percentLabel)
		me.percentLabel.setText(me.percentLabel, sprintf("%d%%", me.progressPercent));

	if (me.backgroundMode)
	{
		me.statusLabel.setText(me.statusLabel,
			_("Downloading in background — you can close this window."));
		me.hintLabel.setText(me.hintLabel,
			_("The tray keeps the download and notifies you when game data is ready."));
		TouchAssetFetch_syncPrimaryButton(me, false, false);
		return;
	}

	me.statusLabel.setText(me.statusLabel,
		(message != "") ? message : _("Downloading game data..."));
	if (me.backgroundFailed)
		me.hintLabel.setText(me.hintLabel,
			_("Could not switch to background download. Keep the app open, or close and reopen to resume."));
	else
		me.hintLabel.setText(me.hintLabel,
			_("Leave this open, or tap Download in background — the tray keeps the download and notifies you when it finishes."));
	TouchAssetFetch_syncPrimaryButton(me, false, false);
}

void XonoticTouchAssetFetchDialog_draw(entity me)
{
	TouchStartup_readProgress(me);

	bool isError = (me.progressStatus == TOUCH_FETCH_ERROR);
	bool isDiscover = (me.progressStatus == TOUCH_FETCH_DISCOVER);
	// Never treat a live download as finished — a premature .assets-ready /
	// _touch_assets_ready must not auto-quit the wizard (and the UI).
	bool isDone = (me.progressStatus == TOUCH_FETCH_DONE);
	if (!isDone && TouchStartup_assetsReady()
		&& me.progressStatus != TOUCH_FETCH_RUNNING
		&& me.progressStatus != TOUCH_FETCH_DISCOVER
		&& me.progressStatus != TOUCH_FETCH_PAUSED
		&& me.progressStatus != TOUCH_FETCH_ERROR)
		isDone = true;

	TouchAssetFetch_setErrorState(isError && !isDone);
	if (isDone && !me.completedTime)
		me.completedTime = time + TOUCH_ASSET_FETCH_DONE_DWELL;

	TouchAssetFetch_tryMusic(me);
	TouchAssetFetch_updateWidgets(me, isError, isDiscover, isDone);

	TouchAssetFetch_drawScrim(me);
	SUPER(XonoticTouchAssetFetchDialog).draw(me);

	if (me.completedTime && time >= me.completedTime)
		TouchAssetFetch_finish(me);
}

bool XonoticTouchAssetFetchDialog_shouldShow()
{
	return TouchStartup_assetFetchDialog_shouldShow();
}

void XonoticTouchAssetFetchDialog_fill(entity me)
{
	entity e;

	registercvar("_touch_asset_fetch_active", "0", 0);
	registercvar("_touch_assets_ready", "0", 0);
	registercvar("_touch_asset_fetch_error", "0", 0);
	registercvar("_touch_asset_fetch_background", "0", 0);
	cvar_set("_touch_asset_fetch_background", "0");

	// Rows are explicit rather than implied: TR always steps exactly one row, so
	// every multi-row cell below is followed by that many TRs.
	me.TR(me);
		// Brand first: wide hero logo (native 2:1 art).
		me.TD(me, 5, me.columns, me.logoImage = makeXonoticImage(TOUCH_ASSET_FETCH_LOGO, 2.0));
	me.TR(me);
	me.TR(me);
	me.TR(me);
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, me.columns, me.headlineLabel = makeXonoticHeaderLabel(_("Setting up your game")));
	me.TR(me);
	me.TR(me);
		me.TD(me, 2, me.columns, e = makeXonoticTextLabel(0.5,
			_("Xonotic Touch downloads maps, textures and music once. Partials resume if interrupted — nothing already installed is fetched again.")));
		e.allowWrap = true;
		e.allowWrap_spacing = 0.5;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, me.columns, me.percentLabel = makeXonoticHeaderLabel(""));
	me.TR(me);
		me.TD(me, 1, me.columns, me.progressBar = makeXonoticProgressBar());
	me.TR(me);
		me.TD(me, 2, me.columns, me.statusLabel = makeXonoticTextLabel(0.5, _("Checking your game data...")));
		me.statusLabel.allowWrap = true;
		me.statusLabel.allowWrap_spacing = 0.4;
	me.TR(me);
	me.TR(me);
		// Large touch target (same BigButton path as Home → Play now). Color 0
		// uses the solid button skin — tinted/gray skins were harder to hit-test
		// visually and we no longer stack invisible disabled siblings under it.
		me.TDempty(me, 1);
		me.TD(me, 3, 4, me.primaryButton = makeXonoticBigButton(_("Download in background"), '0 0 0'));
			me.primaryButton.onClick = TouchAssetFetch_Primary_Click;
			me.primaryButton.onClickEntity = me;
			me.primaryButton.focusable = true;
			me.primaryButton.disabled = false;
		me.TDempty(me, 1);
	me.TR(me);
	me.TR(me);
	me.TR(me);
	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 1, me.columns, me.hintLabel = makeXonoticTextLabel(0.5,
			_("Leave this open, or tap Download in background — the tray keeps the download and notifies you when it finishes.")));
		me.hintLabel.allowWrap = true;
}
