#include "touch_scores.qh"
#include "touch_widget.qh"
#include "touch_layout.qh"
#include "touch_init.qh"
#include "touch_defs.qh"
#include "touch_input.qh"
#include <lib/cvar.qh>

bool touch_scores_shown;
bool touch_scores_pill_arming;
vector touch_scores_pill_down;

void Touch_Scores_Reset(void)
{
	Touch_Scores_Hide();
	touch_scores_pill_arming = false;
}

bool Touch_Scores_Shown(void)
{
	return touch_scores_shown;
}

// `+showscores` sets the engine's sb_showscores, which the stock panel reads as
// one of its reasons to draw. Going through the same command a keyboard would use
// means every other reason the panel draws — intermission, the death scoreboard,
// mapvote — keeps working untouched.
void Touch_Scores_Show(void)
{
	if (touch_scores_shown)
		return;
	touch_scores_shown = true;
	localcmd("+showscores\n");
}

void Touch_Scores_Hide(void)
{
	if (!touch_scores_shown)
		return;
	touch_scores_shown = false;
	localcmd("-showscores\n");
}

void Touch_Scores_Toggle(void)
{
	if (touch_scores_shown)
		Touch_Scores_Hide();
	else
		Touch_Scores_Show();
}

// ---------------------------------------------------------------------------
// Pill
// ---------------------------------------------------------------------------

vector Touch_Scores_PillCenter(void)
{
	return Touch_WidgetCenter(autocvar_touch_scores_x, autocvar_touch_scores_y,
		autocvar_touch_scores_size);
}

vector Touch_Scores_PillHalf(void)
{
	return Touch_WidgetHalfExtents(autocvar_touch_scores_size,
		autocvar_touch_scores_aspect);
}

bool Touch_Scores_PillHit(vector p)
{
	if (!autocvar_touch_scores_visible)
		return false;
	return Touch_HitRect(p, autocvar_touch_scores_x, autocvar_touch_scores_y,
		autocvar_touch_scores_size, autocvar_touch_scores_aspect, 1.20);
}

void Touch_Scores_DrawPill(float alpha)
{
	if (!autocvar_touch_scores_visible)
		return;
	// Drawn accented while the board is up, for the same reason the hop bar is:
	// a toggle that does not say it is on is a toggle the player has to test.
	// The touch overlay draws after HUD_Draw, so this stays on top of the board
	// it opened and remains the way back out of it.
	Touch_Widget_ChromePill(Touch_Scores_PillCenter(), Touch_Scores_PillHalf(),
		"SCORE", touch_scores_pill_arming || touch_scores_shown, alpha,
		TOUCH_C_ACCENT);
}

int Touch_Scores_OnDown(vector p)
{
	if (!Touch_Scores_PillHit(p))
		return TOUCH_ROLE_NONE;
	touch_scores_pill_arming = true;
	touch_scores_pill_down = p;
	return TOUCH_ROLE_SCORES;
}

void Touch_Scores_OnMove(vector p)
{
	if (!touch_scores_pill_arming)
		return;
	vector d = p - touch_scores_pill_down;
	d.z = 0;
	// Same slop rule as the other pills: a swipe that begins on the pill is a
	// swipe, not a press.
	if (vlen(d) > autocvar_touch_pause_slop_px)
		touch_scores_pill_arming = false;
}

void Touch_Scores_OnUp(vector p)
{
	if (touch_scores_pill_arming)
	{
		vector d = p - touch_scores_pill_down;
		d.z = 0;
		if (vlen(d) <= autocvar_touch_pause_slop_px)
			Touch_Scores_Toggle();
	}
	touch_scores_pill_arming = false;
}
