#include "touch_scores.qh"
#include "touch_widget.qh"
#include "touch_layout.qh"
#include "touch_init.qh"
#include "touch_defs.qh"
#include "touch_input.qh"
#include <lib/cvar.qh>
#include <client/hud/hud.qh>
#include <client/hud/panel/vote.qh>

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

// ---------------------------------------------------------------------------
// Vote pills
// ---------------------------------------------------------------------------
// A vote called mid-match (client/hud/panel/vote.qc) names its answers by key
// bind, "Yes (F1)", which a player holding a tablet cannot press. While a vote
// is pending and this player has not answered, YES and NO sit beside the vote
// panel and send vyes / vno. They go as soon as the server has the answer
// (vote_highlighted), which also says the tap reached it.

int touch_vote_arming; // 1 YES, -1 NO, 0 neither
vector touch_vote_down;

bool Touch_Vote_Shown(void)
{
	// uid2name is a vote-shaped question answered in its own menu dialog.
	if (!vote_active || vote_highlighted || uid2name_dialog || autocvar__hud_configure)
		return false;
	return true;
}

// The pills share the chrome pills' height, a little narrower: two of them sit
// side by side next to a panel about as wide as one CONSOLE pill.
vector Touch_Vote_PillHalf(void)
{
	return Touch_WidgetHalfExtents(autocvar_touch_pause_size, 2.2);
}

// Beside the panel, from its own cvars as HUD_Panel_LoadCvars reads them, so
// the pills follow wherever the HUD skin put it. Left of it by default, which
// on the stock skins is open aim area rather than a control; right of it when
// the panel sits against the left edge.
vector Touch_Vote_PillCenter(int which)
{
	vector ui = Touch_UISize();
	vector pos = stov(cvar_string("hud_panel_vote_pos"));
	vector size = stov(cvar_string("hud_panel_vote_size"));
	pos = eX * (pos.x * ui.x) + eY * (pos.y * ui.y);
	size = eX * (size.x * ui.x) + eY * (size.y * ui.y);
	vector half = Touch_Vote_PillHalf();
	float gap = TOUCH_SPACE_1 * Touch_UIScale();
	float row_w = half.x * 4 + gap;
	float x0 = pos.x - gap * 2 - row_w;
	if (x0 < gap)
		x0 = pos.x + size.x + gap * 2;
	x0 = bound(gap, x0, ui.x - gap - row_w);
	float cy = bound(half.y + gap, pos.y + size.y * 0.5, ui.y - half.y - gap);
	float cx = x0 + half.x;
	if (which < 0)
		cx += half.x * 2 + gap;
	return eX * cx + eY * cy;
}

int Touch_Vote_PillAt(vector p)
{
	if (!Touch_Vote_Shown())
		return 0;
	if (Touch_HitRectPx(p, Touch_Vote_PillCenter(1), Touch_Vote_PillHalf(), 1.1))
		return 1;
	if (Touch_HitRectPx(p, Touch_Vote_PillCenter(-1), Touch_Vote_PillHalf(), 1.1))
		return -1;
	return 0;
}

bool Touch_Vote_PillHit(vector p)
{
	return Touch_Vote_PillAt(p) != 0;
}

void Touch_Vote_Draw(float alpha)
{
	if (!Touch_Vote_Shown())
	{
		touch_vote_arming = 0;
		return;
	}
	vector half = Touch_Vote_PillHalf();
	Touch_Widget_ChromePill(Touch_Vote_PillCenter(1), half, "YES", touch_vote_arming == 1,
		alpha, TOUCH_C_SUCCESS);
	Touch_Widget_ChromePill(Touch_Vote_PillCenter(-1), half, "NO", touch_vote_arming == -1,
		alpha, TOUCH_C_DANGER);
}

int Touch_Vote_OnDown(vector p)
{
	touch_vote_arming = Touch_Vote_PillAt(p);
	if (!touch_vote_arming)
		return TOUCH_ROLE_NONE;
	touch_vote_down = p;
	return TOUCH_ROLE_VOTE;
}

// Same slop rule as the other pills: a swipe that begins on one is a swipe.
void Touch_Vote_OnMove(vector p)
{
	if (!touch_vote_arming)
		return;
	vector d = p - touch_vote_down;
	d.z = 0;
	if (vlen(d) > autocvar_touch_pause_slop_px)
		touch_vote_arming = 0;
}

void Touch_Vote_OnUp(vector p)
{
	if (touch_vote_arming && Touch_Vote_Shown())
		localcmd(touch_vote_arming > 0 ? "vyes\n" : "vno\n");
	touch_vote_arming = 0;
}
