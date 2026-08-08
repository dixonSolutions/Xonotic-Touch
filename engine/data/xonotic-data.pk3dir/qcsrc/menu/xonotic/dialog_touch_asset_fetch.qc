#include "dialog_touch_asset_fetch.qh"

#include "textlabel.qh"
#include "progressbar.qh"
#include "touchbutton.qh"
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

void TouchAssetFetch_Retry_Click(entity btn, entity me)
{
	// The downloader lives in the launcher shell, so a retry means going back
	// out to it. If that is impossible, say so instead of quitting the app.
	if (TouchStartup_requestRelaunch())
		return;

	me.relaunchFailed = true;
}

float TouchAssetFetch_RetryEnabled(entity btn)
{
	return cvar("_touch_asset_fetch_error");
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
		return;
	}

	me.progressBar.color = TOUCH_ASSET_FETCH_COLOR_PROGRESS;
	me.hintLabel.setText(me.hintLabel, _("Keep Xonotic Touch open until setup finishes."));

	// Determinate whenever the shell published a real percent (download bytes,
	// Flatpak copy steps, discovery checkpoints). Sweep only for "still thinking".
	bool havePercent = (me.progressPercent > 0);
	if (isDiscover && !havePercent)
	{
		me.progressBar.indeterminate = true;
		if (me.percentLabel)
			me.percentLabel.setText(me.percentLabel, "");
		me.statusLabel.setText(me.statusLabel,
			(message != "") ? message : _("Looking for game data on this device..."));
		return;
	}

	me.progressBar.indeterminate = false;
	me.progressBar.setProgress(me.progressBar, me.progressPercent / 100);
	if (me.percentLabel)
		me.percentLabel.setText(me.percentLabel, sprintf("%d%%", me.progressPercent));
	me.statusLabel.setText(me.statusLabel,
		(message != "") ? message : _("Downloading game data..."));
}

void XonoticTouchAssetFetchDialog_draw(entity me)
{
	TouchStartup_readProgress(me);

	bool isError = (me.progressStatus == TOUCH_FETCH_ERROR);
	bool isDiscover = (me.progressStatus == TOUCH_FETCH_DISCOVER);
	bool isDone = (me.progressStatus == TOUCH_FETCH_DONE) || TouchStartup_assetsReady();

	TouchAssetFetch_setErrorState(isError && !isDone);
	if (isDone && !me.completedTime)
		me.completedTime = time + TOUCH_ASSET_FETCH_DONE_DWELL;

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

	// Rows are explicit rather than implied: TR always steps exactly one row, so
	// every multi-row cell below is followed by that many TRs.
	me.TR(me);
		me.TD(me, 1, me.columns, me.headlineLabel = makeXonoticHeaderLabel(_("Setting up your game")));
	me.TR(me);
	me.TR(me);
		me.TD(me, 2, me.columns, e = makeXonoticTextLabel(0.5,
			_("Xonotic Touch downloads the maps, textures and music once. Everything stays in this app, and a Wi-Fi connection is recommended.")));
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
	me.TR(me);
		me.TDempty(me, 1);
		me.TD(me, 2, 4, e = makeXonoticTouchButton(_("Try again"), '0 0 0'));
			e.onClick = TouchAssetFetch_Retry_Click;
			e.onClickEntity = me;
			// Recovery only: an always-visible retry on a healthy download is
			// one more thing to reason about, so it fades out entirely.
			e.disabledAlpha = 0;
			setDependentWeird(e, TouchAssetFetch_RetryEnabled);
		me.TDempty(me, 1);
	me.gotoRC(me, me.rows - 1, 0);
		me.TD(me, 1, me.columns, me.hintLabel = makeXonoticTextLabel(0.5,
			_("Keep Xonotic Touch open until setup finishes.")));
		me.hintLabel.allowWrap = true;
}
