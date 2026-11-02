#include "touch_keyboard.qh"
#include "touch_widget.qh"
#include "touch_init.qh"
#include "touch_layout.qh"

bool touch_kb_shift;
bool touch_kb_symbols;
int touch_kb_hit_row;
int touch_kb_hit_cell;
int touch_kb_shown_override = -1; // -1 = follow vid_keyboard_detected
bool touch_kb_shown_override_for;

bool touch_kb_pop_open;
string touch_kb_pop_chars;        // the key's own character, then its variants
int touch_kb_pop_sel;             // -1 = away from the row
vector touch_kb_pop_key_c;
vector touch_kb_pop_key_half;
vector touch_kb_pop_cell_half;
float touch_kb_pop_left;          // left edge of the row
float touch_kb_pop_y;             // centre line of the row
bool touch_kb_pop_rtl;            // own character on the right, near the screen edge

void Touch_Kb_Reset(void)
{
	touch_kb_shift = false;
	touch_kb_symbols = false;
	touch_kb_hit_row = -1;
	touch_kb_hit_cell = -1;
	touch_kb_pop_open = false;
}

bool Touch_Kb_Shifted(void)
{
	return touch_kb_shift;
}

// ---------------------------------------------------------------------------
// Key data
// ---------------------------------------------------------------------------
// QWERTY, because the player already knows it (Jakob's law) even on glass. The
// symbol layer carries what a game chat actually needs — digits for scores and
// times, and the handful of punctuation marks people use.

string Touch_Kb_RowChars(int row)
{
	if (touch_kb_symbols)
	{
		switch (row)
		{
			case 0: return "1234567890";
			case 1: return "-/:;()$&@";
			case 2: return ".,?!'\"+=";
			default: return "";
		}
	}
	switch (row)
	{
		case 0: return "qwertyuiop";
		case 1: return "asdfghjkl";
		case 2: return "zxcvbnm";
		default: return "";
	}
}

// Every row is laid out on the same 10-unit grid so the columns line up down the
// keyboard. Rows narrower than 10 units are centred by their leading offset.
float Touch_Kb_RowGrid(void)
{
	return 10;
}

// Row 2 is [shift] + letters + DEL. Shift exists only in the letter layer — there
// is no upper case to reach in the symbol layer — but DEL is on row 2 in both,
// because it is the only backspace on the keyboard.
int Touch_Kb_Row2Lead(void)
{
	return touch_kb_symbols ? 0 : 1;
}

float Touch_Kb_CellWeight(int row, int cell)
{
	if (row == 3)
	{
		switch (cell)
		{
			case 0: return 1.5; // layer toggle
			case 1: return 6.0; // space
			default: return 2.5; // send
		}
	}
	if (row == 2)
	{
		// Shift and backspace bracket the letters, matching every phone keyboard.
		if (cell == Touch_Kb_RowCells(row) - 1)
			return 1.5;
		if (cell == 0 && Touch_Kb_Row2Lead())
			return 1.5;
	}
	return 1.0;
}

int Touch_Kb_RowCells(int row)
{
	// Bottom row is layer | space | SEND. It used to carry a second backspace
	// beside the one on the letter row, in the position every phone keyboard uses
	// for return — two ways to do one thing, in the place people aim for another.
	if (row == 3)
		return 3;
	int chars = strlen(Touch_Kb_RowChars(row));
	if (row == 2)
		return chars + Touch_Kb_Row2Lead() + 1; // [shift] + chars + backspace
	return chars;
}

float Touch_Kb_RowWeight(int row)
{
	float total = 0;
	int i;
	for (i = 0; i < Touch_Kb_RowCells(row); i += 1)
		total += Touch_Kb_CellWeight(row, i);
	return total;
}

float Touch_Kb_CellOffset(int row, int cell)
{
	// Centre any row that does not fill the grid, so the columns stay aligned
	// and the row still reads as part of one block.
	float lead = (Touch_Kb_RowGrid() - Touch_Kb_RowWeight(row)) * 0.5;
	float off = lead;
	int i;
	for (i = 0; i < cell; i += 1)
		off += Touch_Kb_CellWeight(row, i);
	return off;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

bool Touch_Kb_Hidden(void)
{
	bool def = cvar("vid_keyboard_detected") > 0;
	if (touch_kb_shown_override >= 0 && touch_kb_shown_override_for != def)
		touch_kb_shown_override = -1;
	if (touch_kb_shown_override >= 0)
		return !touch_kb_shown_override;
	return def;
}

void Touch_Kb_ToggleHidden(void)
{
	bool hidden = Touch_Kb_Hidden();
	touch_kb_shown_override_for = cvar("vid_keyboard_detected") > 0;
	touch_kb_shown_override = hidden ? 1 : 0;
	touch_kb_pop_open = false;
}

float Touch_Kb_Height(void)
{
	if (Touch_Kb_Hidden())
		return 0;
	vector ui = Touch_UISize();
	return bound(0.20, autocvar_touch_kb_height, 0.70) * ui.y;
}

float Touch_Kb_Top(void)
{
	vector ui = Touch_UISize();
	return ui.y - Touch_Kb_Height();
}

float Touch_Kb_Gap(void)
{
	return max(2, TOUCH_SPACE_1 * Touch_UIScale() * 0.6);
}

float Touch_Kb_SideMargin(void)
{
	// Matches the chat sheet's margin: the keyboard is part of the same panel as
	// far as the eye is concerned, so their outer edges have to share a line.
	return TOUCH_SPACE_3 * Touch_UIScale();
}

float Touch_Kb_RowHeight(void)
{
	return (Touch_Kb_Height() - Touch_Kb_Gap() * (TOUCH_KB_ROWS + 1)) / TOUCH_KB_ROWS;
}

float Touch_Kb_UnitWidth(void)
{
	vector ui = Touch_UISize();
	float usable = ui.x - Touch_Kb_SideMargin() * 2;
	return usable / Touch_Kb_RowGrid();
}

vector Touch_Kb_CellHalf(int row, int cell)
{
	float w = Touch_Kb_UnitWidth() * Touch_Kb_CellWeight(row, cell) - Touch_Kb_Gap();
	float h = Touch_Kb_RowHeight();
	return eX * (max(2, w) * 0.5) + eY * (max(2, h) * 0.5);
}

vector Touch_Kb_CellCenter(int row, int cell)
{
	float gap = Touch_Kb_Gap();
	float x = Touch_Kb_SideMargin()
		+ Touch_Kb_CellOffset(row, cell) * Touch_Kb_UnitWidth()
		+ (Touch_Kb_UnitWidth() * Touch_Kb_CellWeight(row, cell)) * 0.5;
	float y = Touch_Kb_Top() + gap + row * (Touch_Kb_RowHeight() + gap)
		+ Touch_Kb_RowHeight() * 0.5;
	return eX * x + eY * y;
}

// ---------------------------------------------------------------------------
// Key identity
// ---------------------------------------------------------------------------

int Touch_Kb_CellAction(int row, int cell)
{
	if (row == 3)
	{
		switch (cell)
		{
			case 0: return TOUCH_KB_LAYER;
			case 1: return TOUCH_KB_CHAR; // space
			default: return TOUCH_KB_SEND;
		}
	}
	if (row == 2)
	{
		if (cell == Touch_Kb_RowCells(row) - 1)
			return TOUCH_KB_BACKSPACE;
		if (cell == 0 && Touch_Kb_Row2Lead())
			return TOUCH_KB_SHIFT;
	}
	return TOUCH_KB_CHAR;
}

string Touch_Kb_CellText(int row, int cell)
{
	if (row == 3)
		return (cell == 1) ? " " : "";
	// Row 2's leading cell is shift in the letter layer, so it holds no character
	// and the character index is offset by one.
	int idx = cell - (row == 2 ? Touch_Kb_Row2Lead() : 0);
	string chars = Touch_Kb_RowChars(row);
	if (idx < 0 || idx >= strlen(chars))
		return "";
	string c = substring(chars, idx, 1);
	return touch_kb_shift ? strtoupper(c) : c;
}

string Touch_Kb_CellLabel(int row, int cell)
{
	int act = Touch_Kb_CellAction(row, cell);
	if (act == TOUCH_KB_SHIFT)
		return touch_kb_shift ? "SHIFT" : "shift";
	if (act == TOUCH_KB_BACKSPACE)
		return "DEL";
	if (act == TOUCH_KB_LAYER)
		return touch_kb_symbols ? "abc" : "?123";
	if (act == TOUCH_KB_SEND)
		return "SEND";
	if (row == 3 && cell == 1)
		return "space";
	return Touch_Kb_CellText(row, cell);
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

int Touch_Kb_HitRow(void)
{
	return touch_kb_hit_row;
}

int Touch_Kb_HitCell(void)
{
	return touch_kb_hit_cell;
}

bool Touch_Kb_Hit(vector p)
{
	touch_kb_hit_row = -1;
	touch_kb_hit_cell = -1;
	if (Touch_Kb_Hidden() || p.y < Touch_Kb_Top())
		return false;

	int row, cell;
	for (row = 0; row < TOUCH_KB_ROWS; row += 1)
	{
		for (cell = 0; cell < Touch_Kb_RowCells(row); cell += 1)
		{
			// Keys are grabbed slightly beyond their drawn edge so the gaps
			// between them are not dead: at this size a near-miss is far more
			// likely than an intentional press of the space between two keys.
			if (!Touch_HitRectPx(p, Touch_Kb_CellCenter(row, cell),
				Touch_Kb_CellHalf(row, cell), 1.08))
				continue;
			touch_kb_hit_row = row;
			touch_kb_hit_cell = cell;
			return true;
		}
	}
	// Inside the keyboard but off every key: still ours, so the sheet behind it
	// does not also react.
	return true;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

string Touch_Kb_Apply(string text, int row, int cell)
{
	int act = Touch_Kb_CellAction(row, cell);

	if (act == TOUCH_KB_SHIFT)
	{
		touch_kb_shift = !touch_kb_shift;
		return text;
	}
	if (act == TOUCH_KB_LAYER)
	{
		touch_kb_symbols = !touch_kb_symbols;
		touch_kb_shift = false;
		return text;
	}
	if (act == TOUCH_KB_BACKSPACE)
	{
		if (strlen(text) <= 0)
			return text;
		return substring(text, 0, strlen(text) - 1);
	}
	if (act != TOUCH_KB_CHAR)
		return text;

	string c = Touch_Kb_CellText(row, cell);
	if (c == "")
		return text;
	if (strlen(text) >= autocvar_touch_kb_maxlen)
		return text;
	// One-shot shift, as on every phone keyboard: holding it for a whole word is
	// not what anyone means by pressing it once.
	touch_kb_shift = false;
	return strcat(text, c);
}

string Touch_Kb_Type(string text, float key, float ch)
{
	if (key == K_BACKSPACE)
	{
		if (strlen(text) <= 0)
			return text;
		return substring(text, 0, strlen(text) - 1);
	}
	if (ch < 32 || ch == 127)
		return text;
	if (strlen(text) >= autocvar_touch_kb_maxlen)
		return text;
	return strcat(text, chr2str(ch));
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Touch_Kb_Draw(float alpha, int hot_row, int hot_cell)
{
	if (Touch_Kb_Hidden())
		return;
	vector ui = Touch_UISize();
	float top = Touch_Kb_Top();

	// One backing region behind the whole keyboard (Law of Common Region): the
	// keys read as a single instrument rather than 40 loose buttons.
	drawfill_builtin(eY * top, eX * ui.x + eY * (ui.y - top), TOUCH_C_SURFACE,
		alpha * TOUCH_A_SURFACE_ACTIVE, DRAWFLAG_NORMAL);
	Touch_DebugAddFills(1);

	int row, cell;
	for (row = 0; row < TOUCH_KB_ROWS; row += 1)
	{
		for (cell = 0; cell < Touch_Kb_RowCells(row); cell += 1)
		{
			vector c = Touch_Kb_CellCenter(row, cell);
			vector half = Touch_Kb_CellHalf(row, cell);
			int act = Touch_Kb_CellAction(row, cell);
			bool hot = (row == hot_row && cell == hot_cell);
			bool on = (act == TOUCH_KB_SHIFT && touch_kb_shift)
				|| (act == TOUCH_KB_LAYER && touch_kb_symbols);
			// SEND is the one key with a consequence, so it is the one key that
			// carries the accent at rest (Von Restorff).
			vector accent = (act == TOUCH_KB_SEND) ? TOUCH_C_SUCCESS : TOUCH_C_ACCENT;
			Touch_Widget_Key(c, half, Touch_Kb_CellLabel(row, cell), hot, on,
				act == TOUCH_KB_SEND, alpha, accent);
		}
	}
	Touch_Kb_DrawPopup(alpha);
}

// ---------------------------------------------------------------------------
// Hold for special characters
// ---------------------------------------------------------------------------
// Checked against Xolonium (the HUD font, unifont behind it): every one of these
// has a glyph, so none draws as a box. The engine sheet checks its fonts at
// run time (Font_HasGlyph).

string Touch_Kb_VariantsOf(string c)
{
	if (c == "a") return "àáâäæãåā";   if (c == "A") return "ÀÁÂÄÆÃÅĀ";
	if (c == "e") return "èéêëē";      if (c == "E") return "ÈÉÊËĒ";
	if (c == "i") return "ìíîïī";      if (c == "I") return "ÌÍÎÏĪ";
	if (c == "o") return "òóôöõøœō";   if (c == "O") return "ÒÓÔÖÕØŒŌ";
	if (c == "u") return "ùúûüū";      if (c == "U") return "ÙÚÛÜŪ";
	if (c == "c") return "çćč";        if (c == "C") return "ÇĆČ";
	if (c == "n") return "ñń";         if (c == "N") return "ÑŃ";
	if (c == "s") return "ßśš";        if (c == "S") return "ẞŚŠ";
	if (c == "y") return "ýÿ";         if (c == "Y") return "ÝŸ";
	if (c == "z") return "žźż";        if (c == "Z") return "ŽŹŻ";
	if (c == "l") return "ł";          if (c == "L") return "Ł";
	if (c == "-") return "–—";
	if (c == "'") return "‘’";
	if (c == "\"") return "“”";
	if (c == "?") return "¿";
	if (c == "!") return "¡";
	if (c == "$") return "€£¥";
	if (c == ".") return "…";
	return "";
}

bool Touch_Kb_PopupOpen(void)
{
	return touch_kb_pop_open;
}

float Touch_Kb_PopupCellX(int i)
{
	float w = touch_kb_pop_cell_half.x * 2;
	int n = strlen(touch_kb_pop_chars);
	// Own character over the key, the others running away from the near edge.
	return touch_kb_pop_left + w * ((touch_kb_pop_rtl ? (n - 1 - i) : i) + 0.5);
}

bool Touch_Kb_HoldCheck(int row, int cell, float held)
{
	float hold = cvar("touch_kb_hold_time");
	if (touch_kb_pop_open || hold <= 0 || held < hold)
		return false;
	if (Touch_Kb_CellAction(row, cell) != TOUCH_KB_CHAR)
		return false;
	string c = Touch_Kb_CellText(row, cell);
	string v = Touch_Kb_VariantsOf(c);
	if (c == "" || v == "")
		return false;

	vector ui = Touch_UISize();
	float m = Touch_Kb_SideMargin();
	strcpy(touch_kb_pop_chars, strcat(c, v));
	int n = strlen(touch_kb_pop_chars);
	touch_kb_pop_key_c = Touch_Kb_CellCenter(row, cell);
	touch_kb_pop_key_half = Touch_Kb_CellHalf(row, cell);
	touch_kb_pop_cell_half = eX * max(touch_kb_pop_key_half.x, touch_kb_pop_key_half.y * 0.8)
		+ eY * (touch_kb_pop_key_half.y * 1.1);
	float w = touch_kb_pop_cell_half.x * 2;
	float own_left = touch_kb_pop_key_c.x - touch_kb_pop_cell_half.x;
	touch_kb_pop_rtl = false;
	touch_kb_pop_left = own_left;
	if (own_left + n * w > ui.x - m)
	{
		touch_kb_pop_rtl = true;
		touch_kb_pop_left = own_left + w - n * w;
		if (touch_kb_pop_left < m)
		{
			touch_kb_pop_rtl = false;
			touch_kb_pop_left = max(m, ui.x - m - n * w);
		}
	}
	touch_kb_pop_y = touch_kb_pop_key_c.y - touch_kb_pop_key_half.y
		- touch_kb_pop_cell_half.y - touch_kb_pop_key_half.y * 0.3;
	touch_kb_pop_sel = 0;
	touch_kb_pop_open = true;
	return true;
}

// From the row down to the held key the nearest cell is picked; well away from
// that band nothing is, and releasing there types nothing.
void Touch_Kb_PopupSlide(vector p)
{
	if (!touch_kb_pop_open)
		return;
	int n = strlen(touch_kb_pop_chars);
	float w = touch_kb_pop_cell_half.x * 2;
	float top = touch_kb_pop_y - touch_kb_pop_cell_half.y * 2.5;
	float bottom = touch_kb_pop_key_c.y + touch_kb_pop_key_half.y * 2;
	if (p.y < top || p.y > bottom || p.x < touch_kb_pop_left - w * 0.5
		|| p.x > touch_kb_pop_left + n * w + w * 0.5)
	{
		touch_kb_pop_sel = -1;
		return;
	}
	int i;
	float best = 1000000;
	for (i = 0; i < n; i += 1)
	{
		float d = fabs(p.x - Touch_Kb_PopupCellX(i));
		if (d < best)
		{
			best = d;
			touch_kb_pop_sel = i;
		}
	}
}

void Touch_Kb_ReleaseShift(void)
{
	touch_kb_shift = false;
}

string Touch_Kb_PopupPick(void)
{
	if (!touch_kb_pop_open)
		return "";
	touch_kb_pop_open = false;
	if (touch_kb_pop_sel < 0)
		return "";
	return substring(touch_kb_pop_chars, touch_kb_pop_sel, 1);
}

void Touch_Kb_DrawPopup(float alpha)
{
	if (!touch_kb_pop_open || Touch_Kb_Hidden())
		return;
	int n = strlen(touch_kb_pop_chars);
	vector half = touch_kb_pop_cell_half;
	// Opaque, so the keys it covers do not read through it.
	drawfill_builtin(eX * (touch_kb_pop_left - 2) + eY * (touch_kb_pop_y - half.y - 2),
		eX * (n * half.x * 2 + 4) + eY * (half.y * 2 + 4), TOUCH_C_SURFACE, alpha,
		DRAWFLAG_NORMAL);
	Touch_DebugAddFills(1);
	int i;
	for (i = 0; i < n; i += 1)
	{
		vector c = eX * Touch_Kb_PopupCellX(i) + eY * touch_kb_pop_y;
		Touch_Widget_Key(c, half - '2 2 0', substring(touch_kb_pop_chars, i, 1),
			i == touch_kb_pop_sel, false, false, alpha, TOUCH_C_ACCENT);
	}
}
