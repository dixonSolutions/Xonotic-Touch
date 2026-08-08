#include "progressbar.qh"

// One sweep of the indeterminate highlight, in seconds.
const float PROGRESSBAR_SWEEP_PERIOD = 1.6;
const float PROGRESSBAR_SWEEP_WIDTH = 0.3;

entity makeXonoticProgressBar()
{
	entity me = NEW(XonoticProgressBar);
	return me;
}

void XonoticProgressBar_setProgress(entity me, float theProgress)
{
	me.progress = bound(0, theProgress, 1);
}

void XonoticProgressBar_draw(entity me)
{
	vector barOrigin = eY * (0.5 * (1 - me.barHeight));
	vector barSize = eX + eY * me.barHeight;

	draw_Fill(barOrigin, barSize, me.colorTrack, me.alphaTrack);

	if (me.indeterminate)
	{
		// Sweep from just off the left edge to just off the right edge, so the
		// highlight enters and leaves instead of popping in place.
		float phase = (time / PROGRESSBAR_SWEEP_PERIOD);
		phase -= floor(phase);
		float head = phase * (1 + PROGRESSBAR_SWEEP_WIDTH) - PROGRESSBAR_SWEEP_WIDTH;
		float left = max(0, head);
		float right = min(1, head + PROGRESSBAR_SWEEP_WIDTH);
		if (right > left)
			draw_Fill(barOrigin + eX * left, eX * (right - left) + eY * me.barHeight,
				me.color, me.alphaBar);
	}
	else if (me.progress > 0)
	{
		draw_Fill(barOrigin, eX * me.progress + eY * me.barHeight,
			me.color, me.alphaBar);
	}

	SUPER(XonoticProgressBar).draw(me);
}
