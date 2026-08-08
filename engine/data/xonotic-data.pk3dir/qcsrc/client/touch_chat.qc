#include "touch_chat.qh"
#include "touch_keyboard.qh"
#include "touch_pointer.qh"
#include "touch_widget.qh"
#include "touch_layout.qh"
#include "touch_init.qh"
#include "touch_defs.qh"
#include "touch_input.qh"
#include <lib/cvar.qh>

bool touch_chat_open;
bool touch_chat_team;
string touch_chat_text;

string touch_chat_line[TOUCH_CHAT_HISTORY];
int touch_chat_count;
int touch_chat_head;

bool touch_chat_pill_arming;
vector touch_chat_pill_down;
int touch_chat_hot_row;
int touch_chat_hot_cell;
int touch_chat_hot_button;

const int TOUCH_CHAT_BTN_NONE  = -1;
const int TOUCH_CHAT_BTN_CLOSE = 0;
const int TOUCH_CHAT_BTN_TEAM  = 1;
const int TOUCH_CHAT_BTN_COUNT = 2;

void Touch_Chat_Reset(void)
{
	// A reset can land mid-session (map change), and the sheet borrows the chat
	// panel while it is open, so it has to be handed back or the feed stays off.
	if (touch_chat_open)
		cvar_set("hud_panel_chat", "1");
	touch_chat_open = false;
	touch_chat_team = false;
	touch_chat_pill_arming = false;
	touch_chat_hot_row = -1;
	touch_chat_hot_cell = -1;
	touch_chat_hot_button = TOUCH_CHAT_BTN_NONE;
	strcpy(touch_chat_text, "");
}

// ---------------------------------------------------------------------------
// Backlog
// ---------------------------------------------------------------------------

// The engine routes a print to its chat box when the first byte is \1 (say and
// tell, with a sound) or \3 (message, silent) — see Con_MaskPrint in
// darkplaces/console.c. Using the same test means the sheet shows exactly what
// the feed showed, instead of a QC-side guess about what looks like chat.
void Touch_Chat_Capture(string msg)
{
	if (msg == "")
		return;
	float lead = str2chr(msg, 0);
	if (lead != 1 && lead != 3)
		return;

	string line = substring(msg, 1, strlen(msg) - 1);
	// Team chat carries a second marker byte before the text.
	if (strlen(line) > 0 && str2chr(line, 0) < 32)
		line = substring(line, 1, strlen(line) - 1);
	line = strreplace("\n", "", line);
	if (line == "")
		return;

	strcpy(touch_chat_line[touch_chat_head], line);
	touch_chat_head = (touch_chat_head + 1) % TOUCH_CHAT_HISTORY;
	if (touch_chat_count < TOUCH_CHAT_HISTORY)
		touch_chat_count += 1;
}

// 0 is the oldest line still held.
string Touch_Chat_Line(int i)
{
	if (i < 0 || i >= touch_chat_count)
		return "";
	int oldest = (touch_chat_head - touch_chat_count + TOUCH_CHAT_HISTORY) % TOUCH_CHAT_HISTORY;
	return touch_chat_line[(oldest + i) % TOUCH_CHAT_HISTORY];
}

// ---------------------------------------------------------------------------
// Pill
// ---------------------------------------------------------------------------

vector Touch_Chat_PillCenter(void)
{
	return Touch_WidgetCenter(autocvar_touch_chat_x, autocvar_touch_chat_y,
		autocvar_touch_chat_size);
}

vector Touch_Chat_PillHalf(void)
{
	return Touch_WidgetHalfExtents(autocvar_touch_chat_size, autocvar_touch_chat_aspect);
}

bool Touch_Chat_PillHit(vector p)
{
	if (!autocvar_touch_chat_visible)
		return false;
	return Touch_HitRect(p, autocvar_touch_chat_x, autocvar_touch_chat_y,
		autocvar_touch_chat_size, autocvar_touch_chat_aspect, 1.20);
}

void Touch_Chat_DrawPill(float alpha)
{
	if (!autocvar_touch_chat_visible)
		return;
	Touch_Widget_ChromePill(Touch_Chat_PillCenter(), Touch_Chat_PillHalf(), "CHAT",
		touch_chat_pill_arming, alpha, TOUCH_C_ACCENT);
}

int Touch_Chat_OnDown(vector p)
{
	if (!Touch_Chat_PillHit(p))
		return TOUCH_ROLE_NONE;
	touch_chat_pill_arming = true;
	touch_chat_pill_down = p;
	return TOUCH_ROLE_CHAT;
}

void Touch_Chat_OnMove(vector p)
{
	if (!touch_chat_pill_arming)
		return;
	vector d = p - touch_chat_pill_down;
	d.z = 0;
	// Same slop rule as the other pills: a look swipe that begins on the pill is
	// a swipe, not a press.
	if (vlen(d) > autocvar_touch_pause_slop_px)
		touch_chat_pill_arming = false;
}

void Touch_Chat_OnUp(vector p)
{
	if (touch_chat_pill_arming)
	{
		vector d = p - touch_chat_pill_down;
		d.z = 0;
		if (vlen(d) <= autocvar_touch_pause_slop_px)
			Touch_Chat_Open(false);
	}
	touch_chat_pill_arming = false;
}

bool Touch_Chat_Busy(void)
{
	return touch_chat_pill_arming;
}

// ---------------------------------------------------------------------------
// Sheet
// ---------------------------------------------------------------------------

bool Touch_Chat_Active(void)
{
	return cvar("vid_touchscreen") && touch_chat_open;
}

void Touch_Chat_Open(bool team)
{
	// Everything held must be let go: the sheet covers the controls, so a key
	// still down would be stuck down for as long as the sheet is open.
	Touch_ReleaseAll();
	Touch_Kb_Reset();
	Touch_Pointer_Clear();
	// The transient feed is drawn by the *engine*, after CSQC, so it lands on top
	// of the sheet no matter how opaque the sheet is — the composer opened with
	// "no messages yet" in the middle of it and live chat lines printed across it.
	// Setting the panel off makes HUD_Chat park con_chat/con_chatrect, and turning
	// it back on restores them. The sheet's own backlog is the feed while it is up.
	cvar_set("hud_panel_chat", "0");
	touch_chat_open = true;
	touch_chat_team = team;
	touch_chat_hot_row = -1;
	touch_chat_hot_cell = -1;
	touch_chat_hot_button = TOUCH_CHAT_BTN_NONE;
	strcpy(touch_chat_text, "");
}

void Touch_Chat_Close(void)
{
	touch_chat_open = false;
	touch_chat_pill_arming = false;
	cvar_set("hud_panel_chat", "1");
	Touch_Pointer_Clear();
	strcpy(touch_chat_text, "");
}

// Space is a full-width key next to SEND, so a run of stray spaces is an easy
// mistake. Sending it would put an empty line in everyone's feed.
bool Touch_Chat_IsBlank(string s)
{
	int i;
	for (i = 0; i < strlen(s); i += 1)
		if (substring(s, i, 1) != " ")
			return false;
	return true;
}

void Touch_Chat_Send(void)
{
	if (Touch_Chat_IsBlank(touch_chat_text))
	{
		Touch_Chat_Close();
		return;
	}
	// This string reaches a console command line, so it is escaped here at the
	// boundary rather than trusted because the on-screen keyboard "can only
	// produce safe characters". A quote or a newline would otherwise end the
	// argument and let the rest run as commands.
	string safe = MakeConsoleSafe(touch_chat_text);
	localcmd(strcat((touch_chat_team ? "say_team \"" : "say \""), safe, "\"\n"));
	Touch_Chat_Close();
}

// The sheet spans from the top margin down to just above the keyboard. Its
// geometry is shared by the surface, the header and the backlog so that the three
// cannot disagree about where the panel's edges are.
float Touch_Chat_SheetMargin(void)
{
	return TOUCH_SPACE_3 * Touch_UIScale();
}

vector Touch_Chat_SheetHalf(void)
{
	vector ui = Touch_UISize();
	float m = Touch_Chat_SheetMargin();
	float bottom = Touch_Kb_Top() - m * 0.5;
	// Same side margin as the keyboard below it, so the sheet's edge and the outer
	// key column line up down the screen instead of missing each other by 10 units.
	return eX * (ui.x * 0.5 - m) + eY * (max(4, bottom - m) * 0.5);
}

vector Touch_Chat_SheetCenter(void)
{
	vector ui = Touch_UISize();
	return eX * (ui.x * 0.5) + eY * (Touch_Chat_SheetMargin() + Touch_Chat_SheetHalf().y);
}

// Everything in the sheet is laid out against this inner box, not the screen, so
// nothing sits on the sheet's own edge.
float Touch_Chat_Pad(void)
{
	return TOUCH_SPACE_3 * Touch_UIScale();
}

vector Touch_Chat_InnerHalf(void)
{
	return Touch_Chat_SheetHalf() - '1 1 0' * Touch_Chat_Pad();
}

vector Touch_Chat_FieldHalf(void)
{
	return eX * Touch_Chat_InnerHalf().x
		+ eY * (Touch_MMToUnits(TOUCH_MM_COMFORT) * 0.7);
}

// Bottom of the sheet, directly above the keyboard: the thing being typed sits
// between the keys producing it and the backlog it answers.
vector Touch_Chat_FieldCenter(void)
{
	vector c = Touch_Chat_SheetCenter();
	vector half = Touch_Chat_FieldHalf();
	return eX * c.x + eY * (c.y + Touch_Chat_InnerHalf().y - half.y);
}

vector Touch_Chat_ButtonHalf(void)
{
	float h = Touch_MMToUnits(TOUCH_MM_COMFORT) * 0.6;
	return eX * (h * 2.4) + eY * h;
}

vector Touch_Chat_ButtonCenter(int idx)
{
	vector c = Touch_Chat_SheetCenter();
	vector inner = Touch_Chat_InnerHalf();
	vector half = Touch_Chat_ButtonHalf();
	float gap = TOUCH_SPACE_2 * Touch_UIScale();
	// Right-aligned header buttons, CLOSE outermost: the exit belongs at the edge
	// where it is hard to hit by accident mid-typing.
	float right = c.x + inner.x - half.x;
	float x = right - idx * (half.x * 2 + gap);
	return eX * x + eY * (c.y - inner.y + half.y);
}

int Touch_Chat_HitButton(vector p)
{
	int i;
	for (i = 0; i < TOUCH_CHAT_BTN_COUNT; i += 1)
		if (Touch_HitRectPx(p, Touch_Chat_ButtonCenter(i), Touch_Chat_ButtonHalf(), 1.15))
			return i;
	return TOUCH_CHAT_BTN_NONE;
}

void Touch_Chat_PointerDown(vector p)
{
	touch_chat_hot_row = -1;
	touch_chat_hot_cell = -1;
	touch_chat_hot_button = Touch_Chat_HitButton(p);
	if (touch_chat_hot_button != TOUCH_CHAT_BTN_NONE)
		return;
	if (Touch_Kb_Hit(p))
	{
		touch_chat_hot_row = Touch_Kb_HitRow();
		touch_chat_hot_cell = Touch_Kb_HitCell();
	}
}

void Touch_Chat_PointerMove(vector p)
{
	if (touch_chat_hot_button != TOUCH_CHAT_BTN_NONE)
	{
		if (Touch_Chat_HitButton(p) != touch_chat_hot_button)
			touch_chat_hot_button = TOUCH_CHAT_BTN_NONE;
		return;
	}
	if (touch_chat_hot_row < 0)
		return;
	// Sliding off a key cancels it. On a key this small a slide is much more
	// likely to be a correction than a decision.
	Touch_Kb_Hit(p);
	if (Touch_Kb_HitRow() != touch_chat_hot_row || Touch_Kb_HitCell() != touch_chat_hot_cell)
	{
		touch_chat_hot_row = -1;
		touch_chat_hot_cell = -1;
	}
}

void Touch_Chat_PointerUp(void)
{
	if (touch_chat_hot_button == TOUCH_CHAT_BTN_CLOSE)
	{
		Touch_Chat_Close();
		return;
	}
	if (touch_chat_hot_button == TOUCH_CHAT_BTN_TEAM)
	{
		touch_chat_team = !touch_chat_team;
		touch_chat_hot_button = TOUCH_CHAT_BTN_NONE;
		return;
	}
	if (touch_chat_hot_row < 0)
		return;

	int row = touch_chat_hot_row;
	int cell = touch_chat_hot_cell;
	touch_chat_hot_row = -1;
	touch_chat_hot_cell = -1;

	if (Touch_Kb_CellAction(row, cell) == TOUCH_KB_SEND)
	{
		Touch_Chat_Send();
		return;
	}
	// The new text has to be built *before* strcpy runs. strcpy(x, f(x)) is a
	// use-after-free: the macro strunzones its target first and only then
	// evaluates the argument, so f() reads a freed string. Every keypress read
	// the buffer as empty and the field held only the last character typed.
	string next = Touch_Kb_Apply(touch_chat_text, row, cell);
	strcpy(touch_chat_text, next);
}

void Touch_Chat_Frame(void)
{
	if (!Touch_Chat_Active())
		return;
	// Keys are released on open, but a sheet that stays open across a respawn
	// must not let anything creep back on.
	Touch_ReleaseAll();

	Touch_Pointer_Frame();
	if (Touch_Pointer_Pressed())
		Touch_Chat_PointerDown(Touch_Pointer_Pos());
	else if (Touch_Pointer_Released())
		Touch_Chat_PointerUp();
	else if (Touch_Pointer_IsDown())
		Touch_Chat_PointerMove(Touch_Pointer_Pos());
}

void Touch_Chat_DrawHistory(float alpha)
{
	vector c = Touch_Chat_SheetCenter();
	vector inner = Touch_Chat_InnerHalf();
	float left = c.x - inner.x;
	// Between the header row and the field, both of which are already placed
	// against the same inner box.
	float top = c.y - inner.y + Touch_Chat_ButtonHalf().y * 2 + Touch_Chat_Pad();
	float bottom = Touch_Chat_FieldCenter().y - Touch_Chat_FieldHalf().y
		- TOUCH_SPACE_2 * Touch_UIScale();

	float fs = TOUCH_FONT_READOUT_SM * Touch_UIScale() * 1.15;
	float pitch = fs * 1.55;
	int fits = floor((bottom - top) / pitch);
	if (fits < 1)
		return;

	if (touch_chat_count <= 0)
	{
		Touch_Shape_Label(eX * c.x + eY * ((top + bottom) * 0.5), fs,
			"no messages yet", TOUCH_C_RIM, alpha * TOUCH_A_GHOST * 3);
		return;
	}

	// Newest at the bottom, next to the field being typed in, so the reply is
	// beside what it replies to.
	int shown = min(fits, touch_chat_count);
	int i;
	for (i = 0; i < shown; i += 1)
	{
		string line = Touch_Chat_Line(touch_chat_count - shown + i);
		float y = bottom - (shown - i) * pitch + pitch * 0.5;
		// Colour-coded: player names carry team and identity colours, and
		// stripping them would lose who said what.
		drawcolorcodedstring(eX * left + eY * (y - fs * 0.5), line, '1 1 0' * fs,
			alpha * TOUCH_A_INK, DRAWFLAG_NORMAL);
		Touch_DebugAddFills(1);
	}
}

void Touch_Chat_Draw(void)
{
	if (!Touch_Chat_Active())
		return;

	vector ui = Touch_UISize();
	float alpha = 1.0;

	// Modal: the scrim says the game is not listening to the controls.
	drawfill_builtin('0 0 0', ui, TOUCH_C_SURFACE, TOUCH_A_SCRIM, DRAWFLAG_NORMAL);
	Touch_DebugAddFills(1);

	// A dim alone is not enough. A scrim leaves whatever is behind it legible, and
	// while the player is dead the stock scoreboard is a screenful of white text
	// that read straight through the backlog. So the sheet is an actual surface
	// with an edge (Law of Common Region), the way a GNOME dialog is, rather than
	// text floating on a darkened game.
	float sheet_r = TOUCH_R_MD * Touch_UIScale();
	Touch_Shape_RRect(Touch_Chat_SheetCenter(), Touch_Chat_SheetHalf(), sheet_r,
		TOUCH_C_SURFACE, TOUCH_A_SHEET);
	Touch_Shape_RRectRing(Touch_Chat_SheetCenter(), Touch_Chat_SheetHalf(), sheet_r,
		TOUCH_W_STROKE * Touch_UIScale(), TOUCH_C_RIM, alpha * TOUCH_A_GHOST * 2);
	vector bhalf = Touch_Chat_ButtonHalf();
	vector sc = Touch_Chat_SheetCenter();
	vector inner = Touch_Chat_InnerHalf();
	Touch_Shape_LabelAligned(eX * (sc.x - inner.x) + eY * (sc.y - inner.y + bhalf.y),
		TOUCH_FONT_READOUT * Touch_UIScale(),
		(touch_chat_team ? "TEAM MESSAGE" : "MESSAGE"), -1, TOUCH_C_INK,
		alpha * TOUCH_A_INK);

	Touch_Widget_Key(Touch_Chat_ButtonCenter(TOUCH_CHAT_BTN_CLOSE), bhalf, "CLOSE",
		touch_chat_hot_button == TOUCH_CHAT_BTN_CLOSE, false, false, alpha,
		TOUCH_C_DANGER);
	Touch_Widget_Key(Touch_Chat_ButtonCenter(TOUCH_CHAT_BTN_TEAM), bhalf, "TEAM",
		touch_chat_hot_button == TOUCH_CHAT_BTN_TEAM, touch_chat_team, false, alpha,
		TOUCH_C_ACCENT);

	Touch_Chat_DrawHistory(alpha);

	Touch_Widget_Field(Touch_Chat_FieldCenter(), Touch_Chat_FieldHalf(),
		touch_chat_text, "tap to type…", alpha, TOUCH_C_ACCENT);

	Touch_Kb_Draw(alpha, touch_chat_hot_row, touch_chat_hot_cell);
}
