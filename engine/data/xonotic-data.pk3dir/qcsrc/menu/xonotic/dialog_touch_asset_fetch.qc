#include "dialog_touch_asset_fetch.qh"

#include "textlabel.qh"
#include "mainwindow.qh"

void TouchAssetFetch_finish(entity me)
{
	cvar_set("_touch_assets_ready", "1");
	cvar_set("_touch_asset_fetch_active", "0");
	main.firstDraw = true;
	Dialog_Close(NULL, me);
}

void XonoticTouchAssetFetchDialog_draw(entity me)
{
	TouchStartup_readProgress(me);
	me.progressPercent = bound(0, me.progressPercent, 100);
	if (me.statusLabel)
		me.statusLabel.setText(me.statusLabel, me.progressMessage);

	if (me.progressStatus == "error")
	{
		me.progressPercent = 0;
		if (me.statusLabel)
			me.statusLabel.setText(me.statusLabel, strcat(_("Download failed: "), me.progressMessage));
		SUPER(XonoticTouchAssetFetchDialog).draw(me);
		return;
	}

	if (me.progressStatus == "done" || (TouchStartup_assetsReady() && me.progressPercent >= 100))
	{
		TouchAssetFetch_finish(me);
		return;
	}

	SUPER(XonoticTouchAssetFetchDialog).draw(me);

	if (me.progressStatus == "discover")
	{
		draw_Fill('0.08 0.72 0', '0.84 0.06 0', '0.15 0.15 0.2', 0.85);
		float pulse = 0.35 + 0.25 * (1 + sin(4 * M_PI * time));
		draw_Fill('0.08 0.72 0', eX * (0.84 * pulse) + eY * 0.06, '0.45 0.65 0.85', 0.95);
		return;
	}

	draw_Fill('0.08 0.72 0', '0.84 0.06 0', '0.15 0.15 0.2', 0.85);
	draw_Fill('0.08 0.72 0', eX * (0.84 * me.progressPercent / 100) + eY * 0.06, '0.35 0.75 0.45', 0.95);
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

	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Checking for an existing Xonotic install on this device. If none is found, game assets will download here with progress shown below.")));
		e.allowWrap = true;
		e.allowWrap_spacing = 0.5;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 6, me.statusLabel = makeXonoticTextLabel(0.5, _("Searching for local game files...")));
		me.statusLabel.allowWrap = true;
}
