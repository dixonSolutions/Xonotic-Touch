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
	string status;
	float percent;
	string message;

	TouchStartup_readProgress(&status, &percent, &message);
	me.progressPercent = bound(0, percent, 100);
	me.progressMessage = message;
	if (me.statusLabel)
		me.statusLabel.setText(me.statusLabel, message);

	if (status == "error")
	{
		me.progressPercent = 0;
		if (me.statusLabel)
			me.statusLabel.setText(me.statusLabel, strcat(_("Download failed: "), message));
		SUPER(XonoticTouchAssetFetchDialog).draw(me);
		return;
	}

	if (status == "done" || (TouchStartup_assetsReady() && me.progressPercent >= 100))
	{
		TouchAssetFetch_finish(me);
		return;
	}

	SUPER(XonoticTouchAssetFetchDialog).draw(me);

	draw_Fill('0.08 0.72', '0.84 0.06', '0.15 0.15 0.2', 0.85);
	draw_Fill('0.08 0.72', eX * (0.84 * me.progressPercent / 100) + eY * 0.06, '0.35 0.75 0.45', 0.95);
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
		me.TD(me, 1, 6, e = makeXonoticTextLabel(0, _("Downloading game assets for the first time. Keep Wi‑Fi connected — this may take several minutes.")));
		e.allowWrap = true;
		e.allowWrap_spacing = 0.5;
	me.TR(me);
	me.TR(me);
		me.TD(me, 1, 6, me.statusLabel = makeXonoticTextLabel(0.5, _("Preparing download...")));
		me.statusLabel.allowWrap = true;
}
