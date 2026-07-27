#include "touchconsolepreview.qh"

entity makeXonoticTouchConsolePreview()
{
	entity me = NEW(XonoticTouchConsolePreview);
	me.configureXonoticTouchConsolePreview(me);
	return me;
}

void XonoticTouchConsolePreview_configureXonoticTouchConsolePreview(entity me)
{
}

void TouchConsolePreview_ClearHandshake(void)
{
	cvar_set("_touch_kb_preview", "0");
	cvar_set("_touch_kb_preview_w", "0");
	cvar_set("_touch_kb_preview_h", "0");
}

// Reserve a cell and publish its console-space rect. The engine paints the real
// TouchUI sheet into that rect after the menu (same LayoutSheet as in-game).
void XonoticTouchConsolePreview_draw(entity me)
{
	vector g0, gs;

	if (!cvar("touch_kb_preview_show"))
	{
		TouchConsolePreview_ClearHandshake();
		draw_CenterText('0.5 0.5 0', _("Preview hidden"), '0.04 0.04 0', '0.55 0.62 0.72', 1, false);
		SUPER(XonoticTouchConsolePreview).draw(me);
		return;
	}

	// Subtle hole so the reserved area is visible before/under the engine pass.
	draw_Fill('0 0 0', '1 1 0', '0.02 0.04 0.08', 0.55);

	g0 = boxToGlobal('0 0 0', draw_shift, draw_scale);
	gs = boxToGlobalSize('1 1 0', draw_scale);
	cvar_set("_touch_kb_preview_x", ftos(g0.x));
	cvar_set("_touch_kb_preview_y", ftos(g0.y));
	cvar_set("_touch_kb_preview_w", ftos(gs.x));
	cvar_set("_touch_kb_preview_h", ftos(gs.y));
	cvar_set("_touch_kb_preview", "1");

	SUPER(XonoticTouchConsolePreview).draw(me);
}

void XonoticTouchConsolePreview_hideNotify(entity me)
{
	TouchConsolePreview_ClearHandshake();
	SUPER(XonoticTouchConsolePreview).hideNotify(me);
}
