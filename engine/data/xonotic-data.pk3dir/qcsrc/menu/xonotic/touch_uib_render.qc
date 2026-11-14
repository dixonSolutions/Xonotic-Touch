// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_uib_render.qh"
#include "touch_menu_scale.qh"
#include "touch_menu_sheet.qh"
#include "../item/nexposee.qh"

// ======================================================================== palette

const vector RB_BG      = '0.035 0.06 0.13';    // deep navy
const vector RB_BG2     = '0.06 0.09 0.19';
const vector RB_SURFACE = '0.075 0.11 0.22';
const vector RB_CARD    = '0.09 0.14 0.28';
const vector RB_CARD_HI = '0.12 0.18 0.36';
const vector RB_BLUE    = '0.18 0.55 1';        // electric blue outlines
const vector RB_EMBER   = '1 0.48 0.18';        // ember accents
const vector RB_TEXT    = '0.92 0.94 0.99';
const vector RB_MUTED   = '0.62 0.69 0.85';
const vector RB_DANGER  = '1 0.33 0.33';
const vector RB_OK      = '0.35 0.9 0.55';
const vector RB_WARN    = '1 0.78 0.3';
const vector RB_INK     = '0.08 0.05 0.03';
const string RB_RRECT   = "gfx/touch/rrect";

// ======================================================================== state

float rb_U;            // console units per dp
vector rb_scr;         // console size
float rb_touch;        // touch device
float rb_laid_rev = -1;
vector rb_laid_scr;
string rb_laid_top;
string rb_top;         // top screen id (stack top)
string rb_modal;       // top modal id or ""
float rb_trans;        // 0..1 screen transition
float rb_reduced;      // reduced motion
float rb_imgbudget;
string rb_loaded;      // images loaded so far, " path " separated
float rb_a;            // alpha for this draw
float rb_narrow;       // one-column layout
float rb_prev_t;
float rb_tick_next;

// items
const int RB_MAX = 1200;
entity rb_item[RB_MAX];
int rb_n;
.string rb_id, rb_kind, rb_t1, rb_t2, rb_img, rb_act, rb_icon, rb_val;
.float rb_region, rb_style, rb_sel, rb_dis, rb_focus, rb_y1, rb_hs, rb_min, rb_max, rb_step, rb_size, rb_list;
.vector rb_o, rb_s, rb_col;

// regions: 0 main, 1 header, 2 toolbar, 3 action bar, 4 side list, 5 detail tab strip, 6 modal
const int RB_REGIONS = 8;
vector rg_o[RB_REGIONS];
vector rg_s[RB_REGIONS];
float rg_scroll[RB_REGIONS];
float rg_max[RB_REGIONS];
float rg_vel[RB_REGIONS];
float rg_content[RB_REGIONS];
float rg_bar[RB_REGIONS];

// horizontal scrollers (carousels, tab strips)
const int RB_HS = 12;
string hs_id[RB_HS];
float hs_scroll[RB_HS];
float hs_max[RB_HS];
float hs_vel[RB_HS];
int hs_n;

// virtual lists
const int RB_LISTS = 6;
float rb_short;             // a very short page (a phone on its side): strips become chips
float lc_band[RB_LISTS];    // the name band under a tile's picture
float lc_cur[RB_LISTS];     // keyboard / gamepad: the row the cursor is on
float lc_rows[RB_LISTS];    // > 0: a card list of this many rows that scrolls sideways
float lc_vieww[RB_LISTS];   // ... and the width it is seen through
string lc_id[RB_LISTS];
string lc_src[RB_LISTS];
string lc_q[RB_LISTS];
string lc_sort[RB_LISTS];
string lc_cols[RB_LISTS];
string lc_anchor[RB_LISTS];
float lc_anchor_i[RB_LISTS];
float lc_buf[RB_LISTS];
float lc_first[RB_LISTS];
float lc_cnt[RB_LISTS];
float lc_rev[RB_LISTS];
float lc_desc[RB_LISTS];
float lc_total[RB_LISTS];
float lc_rowh[RB_LISTS];
float lc_cardw[RB_LISTS];
float lc_cardh[RB_LISTS];
float lc_percol[RB_LISTS];
int lc_n;
string rb_colkey[24];
// table column scratch
float rt_xs[16];
float rt_ws[16];
string rt_ck[16];
string rt_ct[16];

// scroll memory per screen
const int RB_MEM = 16;
string sm_id[RB_MEM];
float sm_off[RB_MEM];

// input
vector rb_mouse, rb_press_at, rb_last_move;
float rb_pressed, rb_dragging, rb_drag_axis, rb_press_region, rb_press_item = -1, rb_press_stopper, rb_press_hs = -1;
float rb_last_move_t, rb_hover = -1, rb_focusi = -1;
float rb_slider_item = -1;
float rb_local_val;        // a slider's value while dragged
string rb_edit_id;         // the field being typed in
string rb_edit_text;
string rb_keycap_id;       // keybind waiting for a key
// what the engine draws over the menu (the console button): kept clear of controls
vector rb_res_o, rb_res_s;
// the last row tapped in a list that tells choosing from playing (rowSelect)
string rb_tap_row, rb_tap_list;
float rb_tap_t;
const float RB_DOUBLETAP = 0.4;   // seconds between the two taps of a double tap
// local overlays: confirm, picker, info
string rb_ov_kind;         // "", "confirm", "picker", "info"
string rb_ov_title, rb_ov_body, rb_ov_ok, rb_ov_node, rb_ov_json, rb_ov_opts;
float rb_ov_destructive;

// ======================================================================== basics

bool TouchUIB_Active()
{
	return UIB_EngineHasBridge() && cvar("menu_touch_uib");
}

float D(float dp) { return dp * rb_U; }

void RB_Fill(vector o, vector s, vector c, float a) { drawfill(o, s, c, a, DRAWFLAG_NORMAL); }

// rounded rectangle from the 9-slice gfx/touch/rrect (its corner is 64 of 256 px)
void RB_RRect(vector o, vector s, float r, vector c, float a)
{
	if (s.x <= 0 || s.y <= 0)
		return;
	r = min(r, min(s.x, s.y) * 0.5);
	if (r < 1)
	{
		RB_Fill(o, s, c, a);
		return;
	}
	vector q = '0.25 0.25 0';
	vector rr = eX * r + eY * r;
	float mw = s.x - 2 * r, mh = s.y - 2 * r;
	drawsubpic(o, rr, RB_RRECT, '0 0 0', q, c, a, 0);
	drawsubpic(o + eX * (s.x - r), rr, RB_RRECT, '0.75 0 0', q, c, a, 0);
	drawsubpic(o + eY * (s.y - r), rr, RB_RRECT, '0 0.75 0', q, c, a, 0);
	drawsubpic(o + eX * (s.x - r) + eY * (s.y - r), rr, RB_RRECT, '0.75 0.75 0', q, c, a, 0);
	if (mw > 0)
	{
		drawsubpic(o + eX * r, eX * mw + eY * r, RB_RRECT, '0.25 0 0', '0.5 0.25 0', c, a, 0);
		drawsubpic(o + eX * r + eY * (s.y - r), eX * mw + eY * r, RB_RRECT, '0.25 0.75 0', '0.5 0.25 0', c, a, 0);
	}
	if (mh > 0)
		RB_Fill(o + eY * r, eX * s.x + eY * mh, c, a);
}

// a card: an outline colour under an inset fill
void RB_Card(vector o, vector s, float r, vector fill, float fa, vector line, float la)
{
	float w = max(1, D(1.25));
	if (la > 0)
		RB_RRect(o, s, r, line, la);
	RB_RRect(o + eX * w + eY * w, s - eX * 2 * w - eY * 2 * w, max(0, r - w), fill, fa);
}

float RB_TextW(string t, float sz) { return stringwidth(t, false, eX * sz + eY * sz); }

void RB_Text(vector o, string t, float sz, vector c, float a)
{
	drawstring(o, t, eX * sz + eY * sz, c, a, DRAWFLAG_NORMAL);
}

// text cut to width with an ellipsis
string RB_Fit(string t, float sz, float w)
{
	if (RB_TextW(t, sz) <= w)
		return t;
	float lo = 0, hi = strlen(t);
	while (lo < hi)
	{
		float m = ceil((lo + hi) / 2);
		if (RB_TextW(strcat(substring(t, 0, m), "…"), sz) <= w) lo = m;
		else hi = m - 1;
	}
	return strcat(substring(t, 0, lo), "…");
}

void RB_TextFit(vector o, string t, float sz, float w, vector c, float a)
{
	RB_Text(o, RB_Fit(t, sz, w), sz, c, a);
}

void RB_TextCenter(vector o, vector s, string t, float sz, vector c, float a)
{
	t = RB_Fit(t, sz, s.x);
	RB_Text(o + eX * (s.x - RB_TextW(t, sz)) * 0.5 + eY * (s.y - sz) * 0.5, t, sz, c, a);
}

// word wrap into rb_line[]
const int RB_LINES = 48;
string rb_line[RB_LINES];
int rb_nline;
int RB_Wrap(string t, float sz, float w)
{
	rb_nline = 0;
	int n = tokenizebyseparator(t, " ");
	string cur = "";
	for (int i = 0; i < n && rb_nline < RB_LINES - 1; ++i)
	{
		string word = argv(i);
		string next = (cur == "") ? word : strcat(cur, " ", word);
		if (cur != "" && RB_TextW(next, sz) > w)
		{
			strcpy(rb_line[rb_nline], cur);
			++rb_nline;
			cur = word;
		}
		else
			cur = next;
	}
	if (cur != "")
	{
		strcpy(rb_line[rb_nline], cur);
		++rb_nline;
	}
	return rb_nline;
}

// semantic icon -> a glyph the menu fonts carry (Xolonium + Unifont fallback)
string RB_Glyph(string icon)
{
	switch (icon)
	{
		case "play": case "resume": return "▶";
		case "back": return "←";
		case "close": case "delete": return "✕";
		case "chevron": return "→";
		case "settings": return "⚙";
		case "search": return "⌕";
		case "refresh": case "update": return "⟳";
		case "lock": return "■";
		case "unlock": return "□";
		case "star": return "☆";
		case "star_filled": return "★";
		case "check": return "✓";
		case "add": return "+";
		case "remove": return "−";
		case "info": return "ⓘ";
		case "warning": return "⚠";
		case "error": return "⊘";
		case "map": return "◈";
		case "mode": case "game": return "◆";
		case "bot": return "◎";
		case "players": case "multiplayer": return "◉";
		case "player": case "singleplayer": return "●";
		case "host": return "⌂";
		case "lan": case "wifi": return "≋";
		case "vpn": case "privacy": return "⛨";
		case "share": return "⇪";
		case "copy": return "⧉";
		case "download": return "↓";
		case "link": return "↗";
		case "quit": return "⏻";
		case "mods": return "⬡";
		case "credits": return "♥";
		case "more": return "⋯";
		case "disconnect": return "⇥";
		case "esp": return "◐";
		case "video": return "▣";
		case "audio": return "♪";
		case "input": case "keyboard": return "⌨";
		case "touch": return "☝";
		case "data": case "server": return "▤";
		case "menu": return "☰";
		case "timer": return "◷";
		case "trophy": return "♛";
		case "ping": return "∿";
		case "folder": return "▭";
		case "help": return "?";
	}
	return "";
}

// ======================================================================== items

entity RB_Item(string kind, string id, float region, vector o, vector s)
{
	if (rb_n >= RB_MAX)
		return NULL;
	entity e = rb_item[rb_n];
	if (!e)
	{
		e = spawn();
		rb_item[rb_n] = e;
	}
	++rb_n;
	strcpy(e.rb_kind, kind);
	strcpy(e.rb_id, id);
	e.rb_region = region;
	e.rb_o = o;
	e.rb_s = s;
	strcpy(e.rb_t1, "");
	strcpy(e.rb_t2, "");
	strcpy(e.rb_img, "");
	strcpy(e.rb_act, "");
	strcpy(e.rb_icon, "");
	strcpy(e.rb_val, "");
	e.rb_style = e.rb_sel = e.rb_dis = e.rb_focus = e.rb_y1 = 0;
	e.rb_hs = -1;
	e.rb_list = -1;
	e.rb_min = e.rb_max = e.rb_step = e.rb_size = 0;
	e.rb_col = '0 0 0';
	return e;
}

float RB_StyleOf(string s)
{
	switch (s)
	{
		case "primary": return 1;
		case "secondary": return 2;
		case "destructive": return 3;
		case "link": return 4;
		case "tonal": return 5;
	}
	return 2;
}

int RB_HsFor(string id)
{
	for (int i = 0; i < hs_n; ++i)
		if (hs_id[i] == id)
			return i;
	if (hs_n >= RB_HS)
		return RB_HS - 1;
	strcpy(hs_id[hs_n], id);
	hs_scroll[hs_n] = 0;
	return hs_n++;
}

int RB_ListFor(string id)
{
	for (int i = 0; i < lc_n; ++i)
		if (lc_id[i] == id)
			return i;
	int k = (lc_n < RB_LISTS) ? lc_n++ : RB_LISTS - 1;
	strcpy(lc_id[k], id);
	if (!lc_buf[k])
		lc_buf[k] = buf_create();
	lc_first[k] = lc_cnt[k] = 0;
	lc_rev[k] = -1;
	strcpy(lc_q[k], "");
	strcpy(lc_sort[k], "");
	strcpy(lc_anchor[k], "");
	return k;
}

// ======================================================================== layout

// Gamepad keys the QuakeC keycode list does not name (engine keys.h): the d-pad,
// the stick as keys, A and B.
const float RB_K_PAD_UP = 816, RB_K_PAD_DOWN = 817, RB_K_PAD_LEFT = 818, RB_K_PAD_RIGHT = 819;
const float RB_K_PAD_A = 826, RB_K_PAD_B = 827;
const float RB_K_STICK_UP = 840, RB_K_STICK_DOWN = 841, RB_K_STICK_LEFT = 842, RB_K_STICK_RIGHT = 843;

float RB_LayoutNode(string id, float x, float y, float w, float region);
vector rb_cell_o, rb_cell_s;
void RB_ListCell(entity e, float r);
int RB_NextFocus(int from, float dir);

float RB_LayoutChildren(string id, float x, float y, float w, float region, float gap)
{
	int n = uib_count(id);
	float y0 = y;
	for (int i = 0; i < n; ++i)
	{
		string c = uib_child(id, i);
		if (uib_get(c, "hidden") == "1")
			continue;
		string t = uib_type(c);
		if (t == "toolbar" || t == "actionbar")
			continue;
		float h = RB_LayoutNode(c, x, y, w, region);
		if (h > 0)
			y += h + gap;
	}
	return max(0, y - y0 - gap);
}

float RB_LayoutText(string id, string body, string style, float x, float y, float w, float region)
{
	float sz = D(15);
	vector col = RB_TEXT;
	switch (style)
	{
		case "caption": case "note": sz = D(13); col = RB_MUTED; break;
		case "title": sz = D(20); break;
		case "warning": col = RB_WARN; break;
	}
	int n = RB_Wrap(body, sz, w);
	float lh = sz * 1.35;
	for (int i = 0; i < n; ++i)
	{
		entity e = RB_Item("line", id, region, eX * x + eY * (y + i * lh), eX * w + eY * lh);
		if (!e) break;
		strcpy(e.rb_t1, rb_line[i]);
		e.rb_size = sz;
		e.rb_col = col;
		e.rb_style = (style == "title");
	}
	return n * lh;
}

// an action prop (e.g. "primary", "actions.1") on the item
void RB_SetAction(entity e, string node, string prop)
{
	strcpy(e.rb_act, uib_get(node, strcat(prop, ".id")));
	if (e.rb_t1 == "") strcpy(e.rb_t1, uib_get(node, strcat(prop, ".label")));
	if (e.rb_icon == "") strcpy(e.rb_icon, uib_get(node, strcat(prop, ".icon")));
	string st = uib_get(node, strcat(prop, ".style"));
	if (st != "") e.rb_style = RB_StyleOf(st);
	if (uib_get(node, strcat(prop, ".enabled")) == "0") e.rb_dis = 1; // an action can be off (welcome: no name yet)
	strcpy(e.rb_val, prop);
	e.rb_focus = 1;
}

float RB_ButtonW(string label, string icon)
{
	drawfont = FONT_USER + 3;   // labels are drawn bold
	float w = RB_TextW(label, D(15)) + D((icon != "") ? 56 : 34);
	drawfont = FONT_USER;
	return w;
}

float RB_LayoutButtonAt(string node, string prop, float x, float y, float w, float region, float style)
{
	entity e = RB_Item("button", node, region, eX * x + eY * y, eX * w + eY * D(48));
	if (!e) return D(48);
	e.rb_style = style;
	if (uib_get(node, "label") != "") strcpy(e.rb_t1, uib_get(node, "label"));
	RB_SetAction(e, node, prop);
	if (uib_get(node, "style") != "") e.rb_style = RB_StyleOf(uib_get(node, "style"));
	if (uib_get(node, "enabled") == "0") e.rb_dis = 1;
	return D(48);
}

float RB_AspectH(string a, float w)
{
	switch (a)
	{
		case "21:9": return w * 9 / 21;
		case "4:3": return w * 3 / 4;
		case "1:1": return w;
		case "3:4": return w * 4 / 3;
	}
	return w * 9 / 16;
}

float RB_LayoutCard(string id, float x, float y, float w, float region, float ih)
{
	string img = uib_get(id, "image");
	if (img == "") ih = 0;
	float th = D(62);
	if (stof(uib_get(id, "meta.#")) > 0) th += D(28);
	entity e = RB_Item("card", id, region, eX * x + eY * y, eX * w + eY * (ih + th));
	if (!e) return ih + th;
	strcpy(e.rb_t1, uib_get(id, "title"));
	strcpy(e.rb_t2, uib_get(id, "subtitle"));
	strcpy(e.rb_img, img);
	strcpy(e.rb_icon, uib_get(id, "icon"));
	e.rb_sel = uib_get(id, "selected") == "1";
	e.rb_size = ih;
	if (uib_has(id, "primary"))
	{
		strcpy(e.rb_act, uib_get(id, "primary.id"));
		strcpy(e.rb_val, "primary");
		e.rb_focus = 1;
	}
	if (uib_get(id, "accent") == "ember") e.rb_style = 1;
	return ih + th;
}

float RB_LayoutGrid(string id, float x, float y, float w, float region)
{
	int n = uib_count(id);
	float mi = stof(uib_get(id, "minItem"));
	float minw = D(mi > 0 ? max(120, mi) : 180);
	float gap = D(12);
	bool horiz = uib_get(id, "scroll") == "horizontal";
	string first = n ? uib_child(id, 0) : "";
	if (n && uib_type(first) == "button")
	{
		int cols = max(1, floor((w + gap) / (D(200) + gap)));
		float bw = (w - (cols - 1) * gap) / cols;
		int k = 0;
		for (int i = 0; i < n; ++i)
		{
			string c = uib_child(id, i);
			if (uib_get(c, "hidden") == "1") continue;
			RB_LayoutButtonAt(c, "action", x + (k % cols) * (bw + gap), y + floor(k / cols) * (D(48) + gap), bw, region, 2);
			++k;
		}
		return ceil(k / cols) * (D(48) + gap) - gap;
	}
	if (horiz)
	{
		int hs = RB_HsFor(id);
		float cw = minw;
		float ih = RB_AspectH(uib_get(first, "aspect"), cw);
		if (uib_get(first, "aspect") == "1:1") ih = cw * 0.62;
		// a short page (a tablet or phone on its side): a compact strip, art and
		// name only, so what follows it (the map rows) fits in view
		bool compact = rg_s[0].y < D(540);
		if (compact) { cw = min(cw, D(116)); ih = cw * 0.5; }
		float h = 0;
		float xx = x;
		for (int i = 0; i < n; ++i)
		{
			string cc = uib_child(id, i);
			if (rb_short)
			{
				// chips: the name only, one row
				drawfont = FONT_USER + 3;
				cw = RB_TextW(uib_get(cc, "title"), D(16)) + D(34);
				drawfont = FONT_USER;
			}
			float hh = RB_LayoutCard(cc, xx, y, cw, region, ih);
			if (rb_n)
			{
				entity ce = rb_item[rb_n - 1];
				ce.rb_hs = hs;
				if (compact) { strcpy(ce.rb_t2, ""); ce.rb_s_y = ih + D(38); hh = ih + D(38); }
				if (rb_short) { strcpy(ce.rb_img, ""); strcpy(ce.rb_icon, ""); ce.rb_size = 0; ce.rb_s_y = D(40); hh = D(40); }
			}
			h = max(h, hh);
			xx += cw + gap;
		}
		if (rb_short)
		{
			hs_max[hs] = max(0, xx - gap - x - w);
			hs_scroll[hs] = bound(0, hs_scroll[hs], hs_max[hs]);
			return h;
		}
		hs_max[hs] = max(0, n * (cw + gap) - gap - w);
		hs_scroll[hs] = bound(0, hs_scroll[hs], hs_max[hs]);
		return h;
	}
	int cols = max(1, floor((w + gap) / (minw + gap)));
	if (stof(uib_get(id, "maxColumns")) > 0) cols = min(cols, stof(uib_get(id, "maxColumns")));
	bool featured = n && uib_get(first, "featured") == "1";
	float maxc = stof(uib_get(id, "maxColumns"));
	// the big cards of Home: always one row, side by side (never stacked)
	if (featured && n <= 4 && (maxc <= 0 || n <= maxc)) cols = n;
	float cw = (w - (cols - 1) * gap) / cols;
	float rowy = y, rowh = 0;
	// ... and as tall as the page allows, so Home needs no scrolling
	float fit = rg_s[0].y - y - D(30) - D(62);
	int rowfirst = rb_n;
	// one image height for the whole grid, so images and icons line up
	bool anyimg = false, anyactions = false, actstack = false;
	float actextra = 0;
	for (int i = 0; i < n; ++i)
	{
		string gc = uib_child(id, i);
		if (uib_get(gc, "image") != "") anyimg = true;
		int gna = stof(uib_get(gc, "actions.#"));
		if (gna > 0)
		{
			anyactions = true;
			// the card's buttons sit side by side while their labels fit, else one
			// under the other inside the card
			float each = (cw - D(20) - (gna - 1) * D(8)) / gna;
			for (int ai = 0; ai < gna; ++ai)
				if (RB_ButtonW(uib_get(gc, sprintf("actions.%d.label", ai)), "") - D(22) > each) actstack = true;
			actextra = max(actextra, actstack ? gna * D(46) + D(4) : D(54));
		}
	}
	for (int i = 0; i < n; ++i)
	{
		string c = uib_child(id, i);
		float ih = anyimg ? RB_AspectH(uib_get(c, "aspect"), cw) * 0.62 : 0;
		if (featured && anyimg)
		{
			if (cols == n)
				ih = bound(D(72), fit - actextra, RB_AspectH(uib_get(c, "aspect"), cw));
			else
			{
				// stacked (the compact Home): every card's picture shares the height the page has
				float each = (rg_s[0].y - y - D(30) - n * D(62) - (n - 1) * gap - (anyactions ? D(54) : 0)) / n;
				ih = bound(D(72), each, min(RB_AspectH(uib_get(c, "aspect"), cw), D(190)));
			}
		}
		if (i % cols == 0) rowfirst = rb_n;
		float hh = RB_LayoutCard(c, x + (i % cols) * (cw + gap), rowy, cw, region, ih);
		int cna = stof(uib_get(c, "actions.#"));
		if (featured && cna > 0)
		{
			// the card's actions (Home's Play card: Singleplayer, Campaign): buttons
			// across the bottom of the card
			float cx0 = x + (i % cols) * (cw + gap) + D(10);
			float bwid = (cw - D(20) - (cna - 1) * D(8)) / cna;
			for (int ai = 0; ai < cna; ++ai)
			{
				entity ab;
				if (actstack) ab = RB_Item("button", c, region, eX * cx0 + eY * (rowy + hh + ai * D(46)), eX * (cw - D(20)) + eY * D(40));
				else ab = RB_Item("button", c, region, eX * (cx0 + ai * (bwid + D(8))) + eY * (rowy + hh), eX * bwid + eY * D(44));
				if (ab) { ab.rb_style = ai ? 2 : 1; RB_SetAction(ab, c, strcat("actions.", ftos(ai))); ab.rb_style = ai ? 2 : 1; strcpy(ab.rb_icon, ""); }
			}
		}
		if (featured && anyactions) hh += actextra;
		rowh = max(rowh, hh);
		if (i % cols == cols - 1 || i == n - 1)
		{
			for (int j = rowfirst; j < rb_n; ++j)
				if (rb_item[j].rb_kind == "card") rb_item[j].rb_s_y = rowh;
			rowy += rowh + gap;
			rowh = 0;
		}
	}
	return rowy - y - gap;
}

float RB_ActionRow(string id, float x, float y, float region)
{
	// the node's primary and secondary actions as buttons; returns the row height
	float xx = x;
	bool any = false;
	if (uib_has(id, "primary"))
	{
		entity b = RB_Item("button", id, region, eX * xx + eY * y, eX * D(150) + eY * D(48));
		if (b)
		{
			b.rb_style = 1;
			RB_SetAction(b, id, "primary");
			b.rb_style = 1;
			b.rb_s_x = RB_ButtonW(b.rb_t1, b.rb_icon) + D(24);
			xx += b.rb_s.x + D(10);
			any = true;
		}
	}
	int na = stof(uib_get(id, "actions.#"));
	for (int i = 0; i < na; ++i)
	{
		entity b = RB_Item("button", id, region, eX * xx + eY * y, eX * D(150) + eY * D(48));
		if (!b) break;
		b.rb_style = 2;
		RB_SetAction(b, id, strcat("actions.", ftos(i)));
		b.rb_s_x = RB_ButtonW(b.rb_t1, b.rb_icon);
		xx += b.rb_s.x + D(10);
		any = true;
	}
	return any ? D(48) : 0;
}

float RB_LayoutHero(string id, float x, float y, float w, float region)
{
	float ih = min(RB_AspectH("16:9", w) * 0.42, rg_s[0].y * 0.42);
	entity e = RB_Item("hero", id, region, eX * x + eY * y, eX * w + eY * ih);
	if (!e) return ih;
	strcpy(e.rb_t1, uib_get(id, "title"));
	strcpy(e.rb_t2, uib_get(id, "subtitle"));
	strcpy(e.rb_img, uib_get(id, "image"));
	if (uib_has(id, "primary"))
	{
		strcpy(e.rb_act, uib_get(id, "primary.id"));
		strcpy(e.rb_val, "primary");
		e.rb_focus = 1;
	}
	return ih + D(12) + RB_ActionRow(id, x, y + ih + D(12), region);
}

float RB_LayoutControl(string id, string t, float x, float y, float w, float region)
{
	string label = uib_get(id, "label");
	string help = uib_get(id, "help");
	string err = uib_get(id, "error");
	float h = D(52);
	entity e;
	if (t == "switch")
	{
		e = RB_Item(((uib_get(id, "style") == "checkbox") ? "check" : "switch"), id, region, eX * x + eY * y, eX * w + eY * h);
		if (!e) return h;
		strcpy(e.rb_t1, label);
		strcpy(e.rb_t2, help);
		strcpy(e.rb_icon, uib_get(id, "icon"));
		e.rb_sel = uib_get(id, "value") == "1";
		e.rb_focus = 1;
		if (help != "" && e.rb_kind == "switch") { h = D(66); e.rb_s_y = h; }
		return h;
	}
	if (t == "slider")
	{
		h = D(72);
		e = RB_Item("slider", id, region, eX * x + eY * y, eX * w + eY * h);
		if (!e) return h;
		strcpy(e.rb_t1, label);
		strcpy(e.rb_t2, uib_get(id, "format"));
		e.rb_min = stof(uib_get(id, "min"));
		e.rb_max = stof(uib_get(id, "max"));
		e.rb_step = stof(uib_get(id, "step"));
		strcpy(e.rb_val, uib_get(id, "value"));
		e.rb_focus = 1;
		return h;
	}
	if (t == "stepper")
	{
		e = RB_Item("stepper", id, region, eX * x + eY * y, eX * w + eY * h);
		if (!e) return h;
		strcpy(e.rb_t1, label);
		strcpy(e.rb_t2, (err != "") ? err : help);
		e.rb_min = stof(uib_get(id, "min"));
		e.rb_max = stof(uib_get(id, "max"));
		e.rb_step = max(1, stof(uib_get(id, "step")));
		strcpy(e.rb_val, uib_get(id, "value"));
		strcpy(e.rb_icon, uib_get(id, "unit"));
		e.rb_focus = 1;
		if (help != "" || err != "") { h = D(68); e.rb_s_y = h; }
		return h;
	}
	if (t == "choice")
	{
		string style = uib_get(id, "style");
		int no = stof(uib_get(id, "options.#"));
		string val = uib_get(id, "value");
		if (style == "dropdown" || (style != "segmented" && style != "cards" && no > 5))
		{
			e = RB_Item("dropdown", id, region, eX * x + eY * y, eX * w + eY * h);
			if (!e) return h;
			strcpy(e.rb_t1, label);
			for (int i = 0; i < no; ++i)
				if (uib_get(id, sprintf("options.%d.value", i)) == val)
					strcpy(e.rb_t2, uib_get(id, sprintf("options.%d.label", i)));
			e.rb_focus = 1;
			return h;
		}
		float lw = RB_TextW(label, D(15)) + D(16);
		float segw = 0;
		for (int i = 0; i < no; ++i)
			segw += RB_TextW(uib_get(id, sprintf("options.%d.label", i)), D(14)) + D(30);
		float sx = x, sy = y, avail = segw;
		float hh = D(52);
		bool stacked = (lw + segw > w) || style == "cards";
		if (stacked)
		{
			e = RB_Item("label", id, region, eX * x + eY * y, eX * w + eY * D(28));
			if (e) strcpy(e.rb_t1, label);
			sy = y + D(30);
			hh = D(30) + D(48);
			avail = w;
		}
		else
		{
			e = RB_Item("label", id, region, eX * x + eY * y, eX * lw + eY * D(52));
			if (e) strcpy(e.rb_t1, label);
			sx = x + w - segw;
		}
		float ow = avail / max(1, no);
		for (int i = 0; i < no; ++i)
		{
			entity o = RB_Item("opt", id, region, eX * (sx + i * ow) + eY * (sy + D(4)), eX * (ow - D(4)) + eY * D(44));
			if (!o) break;
			strcpy(o.rb_t1, uib_get(id, sprintf("options.%d.label", i)));
			strcpy(o.rb_val, uib_get(id, sprintf("options.%d.value", i)));
			o.rb_sel = o.rb_val == val;
			o.rb_focus = 1;
		}
		if (help != "")
		{
			e = RB_Item("line", id, region, eX * x + eY * (y + hh), eX * w + eY * D(18));
			if (e) { strcpy(e.rb_t1, RB_Fit(help, D(12), w)); e.rb_size = D(12); e.rb_col = RB_MUTED; }
			hh += D(20);
		}
		return hh;
	}
	if (t == "field")
	{
		h = D(76);
		e = RB_Item("field", id, region, eX * x + eY * y, eX * w + eY * h);
		if (!e) return h;
		strcpy(e.rb_t1, label);
		strcpy(e.rb_t2, uib_get(id, "placeholder"));
		strcpy(e.rb_val, uib_get(id, "value"));
		e.rb_sel = uib_get(id, "private") == "1";
		e.rb_style = uib_get(id, "hasValue") == "1";
		strcpy(e.rb_icon, uib_get(id, "kind"));
		e.rb_focus = 1;
		return h;
	}
	if (t == "keybind")
	{
		e = RB_Item("keybind", id, region, eX * x + eY * y, eX * w + eY * h);
		if (!e) return h;
		strcpy(e.rb_t1, label);
		string v = "";
		int nk = stof(uib_get(id, "value.#"));
		for (int i = 0; i < nk; ++i)
			v = strcat(v, (i ? "  ·  " : ""), uib_get(id, sprintf("value.%d", i)));
		strcpy(e.rb_t2, (v == "") ? _("not bound") : v);
		e.rb_focus = 1;
		return h;
	}
	if (t == "colour")
	{
		e = RB_Item("label", id, region, eX * x + eY * y, eX * w + eY * D(28));
		if (e) strcpy(e.rb_t1, label);
		int np = stof(uib_get(id, "palette.#"));
		string val = uib_get(id, "value");
		float sw = D(40), g = D(8);
		int per = max(1, floor((w + g) / (sw + g)));
		for (int i = 0; i < np; ++i)
		{
			entity s = RB_Item("swatch", id, region, eX * (x + (i % per) * (sw + g)) + eY * (y + D(32) + floor(i / per) * (sw + g)), eX * sw + eY * sw);
			if (!s) break;
			string hx = uib_get(id, sprintf("palette.%d", i));
			strcpy(s.rb_val, hx);
			s.rb_col = eX * (stof(strcat("0x", substring(hx, 1, 2))) / 255) + eY * (stof(strcat("0x", substring(hx, 3, 2))) / 255)
				+ eZ * (stof(strcat("0x", substring(hx, 5, 2))) / 255);
			s.rb_sel = hx == val;
			s.rb_focus = 1;
		}
		return D(32) + ceil(np / per) * (sw + g);
	}
	return 0;
}

float RB_LayoutList(string id, float x, float y, float w, float region)
{
	int k = RB_ListFor(id);
	float count = stof(uib_get(id, "count"));
	strcpy(lc_src[k], uib_get(id, "source"));
	lc_total[k] = count;
	entity e;
	lc_rows[k] = 0;
	if (uib_get(id, "display") == "cards" && stof(uib_get(id, "rows")) > 0 && uib_get(id, "scroll") == "horizontal")
	{
		// `rows: N`, scrolling sideways: N full rows of tiles (picture and name)
		// always fit the view. The tile height comes from the room below the
		// list's top when that is enough, else from the whole view (the section
		// header stays stuck above it while the page scrolls the list into view).
		int rows = bound(1, stof(uib_get(id, "rows")), 4);
		float gap = D(10), bar = D(10);
		float band = D(30);
		if (rb_short) band = D(24);
		lc_band[k] = band;
		float th = (rg_s[0].y - y - D(4) - bar - gap * (rows - 1)) / rows;
		if (th < D(rb_short ? 58 : 84))
			th = (rg_s[0].y - D(56) - D(8) - bar - gap * (rows - 1)) / rows;
		th = bound(D(56), th, D(176));
		float cw = (uib_get(id, "aspectItem") == "4:3") ? (th - band) * 4 / 3 : (th - band) * 16 / 9;
		int hs = RB_HsFor(id);
		float cols = ceil(max(1, count) / rows);
		float totalw = cols * (cw + gap) - gap;
		float h = rows * th + gap * (rows - 1) + bar;
		lc_rows[k] = rows;
		lc_percol[k] = rows;
		lc_cardw[k] = cw;
		lc_cardh[k] = th;
		lc_rowh[k] = th + gap;
		lc_vieww[k] = w;
		hs_max[hs] = max(0, totalw - w);
		hs_scroll[hs] = bound(0, hs_scroll[hs], hs_max[hs]);
		e = RB_Item("cards", id, region, eX * x + eY * y, eX * max(w, totalw) + eY * h);
		if (e) { e.rb_list = k; e.rb_hs = hs; e.rb_focus = (count > 0); }
		return h;
	}
	if (uib_get(id, "display") == "cards")
	{
		float mi = stof(uib_get(id, "minItem"));
		float minw = D(mi > 0 ? mi : 200);
		float gap = D(12);
		int cols = max(1, floor((w + gap) / (minw + gap)));
		float cw = (w - (cols - 1) * gap) / cols;
		float ch = RB_AspectH(uib_get(id, "aspectItem"), cw) * 0.7 + D(54);
		lc_percol[k] = cols;
		lc_cardw[k] = cw;
		lc_cardh[k] = ch;
		lc_rowh[k] = ch + gap;
		float h = (count > 0) ? ceil(count / cols) * (ch + gap) - gap : D(120);
		e = RB_Item("cards", id, region, eX * x + eY * y, eX * w + eY * h);
		if (e) { e.rb_list = k; e.rb_focus = (count > 0); }
		return h;
	}
	float rowh = D((uib_get(id, "rowHeight") == "compact") ? 40 : 52);
	lc_rowh[k] = rowh;
	lc_percol[k] = 1;
	float head = (uib_get(id, "display") == "table") ? D(34) : 0;
	float h = head + ((count > 0) ? count * rowh : D(140));
	e = RB_Item("table", id, region, eX * x + eY * y, eX * w + eY * h);
	if (e) { e.rb_list = k; e.rb_size = head; e.rb_focus = (count > 0); }
	return h;
}

// the tab strip (in stripregion when >= 0, sticky), then the selected pane
float RB_LayoutTabs(string id, float x, float y, float w, float region, float stripregion)
{
	int n = uib_count(id);
	string val = uib_get(id, "value");
	float tx = 0;
	int hs = RB_HsFor(strcat(id, "#strip"));
	float sh = D(46);
	bool sticky = stripregion >= 0;
	float sreg = sticky ? stripregion : region;
	for (int i = 0; i < n; ++i)
	{
		string p = uib_child(id, i);
		string lab = uib_get(p, "label");
		float tw = RB_TextW(lab, D(15)) + D(32);
		entity e = RB_Item("tab", id, sreg, eX * ((sticky ? 0 : x) + tx) + eY * (sticky ? 0 : y), eX * tw + eY * sh);
		if (!e) break;
		strcpy(e.rb_t1, lab);
		strcpy(e.rb_val, uib_get(p, "key"));
		e.rb_sel = e.rb_val == val;
		e.rb_focus = 1;
		e.rb_hs = hs;
		tx += tw + D(4);
	}
	hs_max[hs] = max(0, tx - w);
	float top = sticky ? y : y + sh + D(12);
	float ch = 0;
	for (int i = 0; i < n; ++i)
	{
		string p = uib_child(id, i);
		if (uib_get(p, "key") == val)
			ch = RB_LayoutChildren(p, x, top, w, region, D(10));
	}
	return top - y + ch;
}

float RB_LayoutNode(string id, float x, float y, float w, float region)
{
	string t = uib_type(id);
	entity e;
	switch (t)
	{
		case "section":
		{
			float y0 = y;
			string title = uib_get(id, "title");
			entity hd = NULL;
			// a phone on its side: a section that is one strip of cards (the game
			// modes) is just that strip, as chips, so what follows keeps its room
			if (rb_short && uib_count(id) == 1 && uib_type(uib_child(id, 0)) == "grid" && uib_get(uib_child(id, 0), "scroll") == "horizontal")
				title = "";
			if (title != "")
			{
				hd = RB_Item("sechead", id, region, eX * x + eY * y, eX * w + eY * D(40));
				if (hd)
				{
					strcpy(hd.rb_t1, title);
					strcpy(hd.rb_t2, rb_short ? "" : uib_get(id, "subtitle"));
					hd.rb_sel = uib_get(id, "sticky") == "1";
				}
				y += rb_short ? D(34) : D(40);
				if (!rb_short && uib_get(id, "subtitle") != "") y += D(8);
			}
			y += RB_LayoutChildren(id, x, y, w, region, D(10));
			if (uib_get(id, "footer") != "")
				y += D(6) + RB_LayoutText(id, uib_get(id, "footer"), "caption", x, y + D(6), w, region);
			if (hd) hd.rb_y1 = y;
			return y - y0 + D(8);
		}
		case "form": case "pane":
		{
			float h = RB_LayoutChildren(id, x, y, w, region, D(8));
			if (t == "form" && uib_has(id, "submit"))
			{
				float bw = RB_ButtonW(uib_get(id, "submit.label"), uib_get(id, "submit.icon")) + D(24);
				RB_LayoutButtonAt(id, "submit", x + w - bw, y + h + D(10), bw, region, 1);
				h += D(58);
			}
			if (uib_get(id, "error") != "")
				h += RB_LayoutText(id, uib_get(id, "error"), "warning", x, y + h + D(6), w, region) + D(6);
			return h;
		}
		case "row":
		{
			int n = uib_count(id);
			if (uib_get(id, "label") != "")
			{
				// a labelled row: label, then the content, then its buttons at the end;
				// on a narrow row the buttons go on a line of their own
				float bwsum = 0;
				for (int i = 0; i < n; ++i)
					if (uib_type(uib_child(id, i)) == "button")
						bwsum += RB_ButtonW(uib_get(uib_child(id, i), "label"), uib_get(uib_child(id, i), "action.icon")) + D(8);
				float labw = min(D(96), w * 0.3);
				bool two = (w - labw - bwsum) < D(170);
				float tw = two ? w - labw : w - labw - bwsum;
				e = RB_Item("rowlabel", id, region, eX * x + eY * y, eX * labw + eY * D(48));
				if (e) { strcpy(e.rb_t1, uib_get(id, "label")); strcpy(e.rb_icon, uib_get(id, "icon")); }
				float th = 0, bx = two ? x + w - bwsum + D(8) : x + labw + tw + D(8);
				for (int i = 0; i < n; ++i)
				{
					string c = uib_child(id, i);
					if (uib_type(c) == "button")
					{
						float bw = RB_ButtonW(uib_get(c, "label"), uib_get(c, "action.icon"));
						RB_LayoutButtonAt(c, "action", bx, y + (two ? D(48) : 0), bw, region, RB_StyleOf(uib_get(c, "style")));
						bx += bw + D(8);
					}
					else if (uib_type(c) == "text")
					{
						entity ln = RB_Item("line", c, region, eX * (x + labw) + eY * y, eX * (tw - D(8)) + eY * D(48));
						if (ln) { strcpy(ln.rb_t1, uib_get(c, "body")); ln.rb_size = D(16); ln.rb_col = RB_TEXT; ln.rb_style = 1; }
						th = D(48);
					}
					else
						th = max(th, RB_LayoutNode(c, x + labw, y, tw, region));
				}
				return max(D(48), th) + (two ? D(52) : 0);
			}
			float xx = x, yy = y, lineh = 0;
			int firstitem = rb_n;
			for (int i = 0; i < n; ++i)
			{
				string c = uib_child(id, i);
				if (uib_get(c, "hidden") == "1") continue;
				string ct = uib_type(c);
				float cw;
				if (ct == "button") cw = RB_ButtonW(uib_get(c, "label"), uib_get(c, "action.icon"));
				else if (ct == "switch") cw = RB_TextW(uib_get(c, "label"), D(15)) + D((uib_get(c, "icon") != "") ? 74 : 54);
				else if (ct == "field") cw = D(260);
				else cw = D(200);
				cw = min(cw, w);
				if (xx > x && xx + cw > x + w)
				{
					xx = x;
					yy += lineh + D(8);
					lineh = 0;
				}
				float ch;
				if (ct == "button") ch = RB_LayoutButtonAt(c, "action", xx, yy, cw, region, RB_StyleOf(uib_get(c, "style")));
				else ch = RB_LayoutNode(c, xx, yy, cw, region);
				lineh = max(lineh, ch);
				xx += cw + D(8);
			}
			if (uib_get(id, "align") == "end" && yy == y)
			{
				float shift = x + w - (xx - D(8));
				for (int j = firstitem; j < rb_n; ++j)
					rb_item[j].rb_o_x += shift;
			}
			return yy - y + lineh;
		}
		case "grid": return RB_LayoutGrid(id, x, y, w, region);
		case "card": return RB_LayoutCard(id, x, y, w, region, RB_AspectH(uib_get(id, "aspect"), w) * 0.5);
		case "hero": return RB_LayoutHero(id, x, y, w, region);
		case "list": return RB_LayoutList(id, x, y, w, region);
		case "tabs": return RB_LayoutTabs(id, x, y, w, region, -1);
		case "text":
		{
			string body = uib_get(id, "body");
			string g = RB_Glyph(uib_get(id, "icon"));
			if (g != "") body = strcat(g, "  ", body);
			return RB_LayoutText(id, body, uib_get(id, "style"), x, y, w, region);
		}
		case "divider":
			RB_Item("divider", id, region, eX * x + eY * (y + D(6)), eX * w + eY * 1);
			return D(12);
		case "image":
		{
			float ih = RB_AspectH(uib_get(id, "aspect"), w);
			if (uib_get(id, "role") == "banner")
				// the Home banner: about a quarter of the page, never a thin strip
			{
				// the banner gives way before the cards do: the cards keep a 96 dp
				// picture and their two lines of text, the banner takes what is left
				// (never under 44 dp), up to 30% of the page
				float room = rg_s[0].y - D(14 + 16 + 30 + 62 + 96);
				ih = min(ih, bound(D(44), min(rg_s[0].y * 0.3, room), D(260)));
			}
			else if (uib_get(id, "fit") == "contain") ih = min(ih, D(120));
			e = RB_Item("image", id, region, eX * x + eY * y, eX * w + eY * ih);
			if (e) { strcpy(e.rb_img, uib_get(id, "ref")); e.rb_sel = uib_get(id, "fit") == "contain"; }
			return ih;
		}
		case "button":
		{
			float bw = min(w, max(D(160), RB_ButtonW(uib_get(id, "label"), uib_get(id, "action.icon"))));
			if (uib_get(id, "style") == "link") bw = min(w, RB_ButtonW(uib_get(id, "label"), uib_get(id, "action.icon")));
			return RB_LayoutButtonAt(id, "action", x, y, bw, region, RB_StyleOf(uib_get(id, "style")));
		}
		case "preview":
		{
			float y0 = y;
			string img = uib_get(id, "image");
			float imgbottom = 0;
			bool compact = uib_get(id, "density") == "compact";
			if (img != "" && compact && w >= D(460))
			{
				// the chosen map or level: its picture on the left, text and Play beside it
				float iw = min(w * 0.4, D(340));
				float ih = RB_AspectH(uib_get(id, "aspect"), iw);
				e = RB_Item("image", id, region, eX * x + eY * y, eX * iw + eY * ih);
				if (e) strcpy(e.rb_img, img);
				imgbottom = y + ih;
				x += iw + D(18);
				w -= iw + D(18);
			}
			else if (img != "")
			{
				float ih = min(RB_AspectH(uib_get(id, "aspect"), w), rg_s[0].y * (compact ? 0.32 : 0.55));
				e = RB_Item("image", id, region, eX * x + eY * y, eX * w + eY * ih);
				if (e) strcpy(e.rb_img, img);
				y += ih + D(14);
			}
			e = RB_Item("line", id, region, eX * x + eY * y, eX * w + eY * D(32));
			if (e) { strcpy(e.rb_t1, uib_get(id, "title")); e.rb_size = D(26); e.rb_col = RB_TEXT; e.rb_style = 1; }
			y += D(36);
			if (uib_get(id, "subtitle") != "")
				y += RB_LayoutText(id, uib_get(id, "subtitle"), "caption", x, y, w, region) + D(8);
			int nb = stof(uib_get(id, "badges.#"));
			for (int i = 0; i < nb; ++i)
			{
				e = RB_Item("chip", id, region, eX * (x + i * D(140)) + eY * y, eX * D(132) + eY * D(28));
				if (e)
				{
					strcpy(e.rb_t1, uib_get(id, sprintf("badges.%d.text", i)));
					strcpy(e.rb_icon, uib_get(id, sprintf("badges.%d.icon", i)));
				}
			}
			if (nb) y += D(38);
			float ah = RB_ActionRow(id, x, y, region);
			if (ah) y += ah + D(14);
			int nat = stof(uib_get(id, "attrs.#"));
			for (int i = 0; i < nat; ++i)
			{
				e = RB_Item("attr", id, region, eX * x + eY * y, eX * w + eY * D(36));
				if (!e) break;
				strcpy(e.rb_t1, uib_get(id, sprintf("attrs.%d.label", i)));
				strcpy(e.rb_t2, uib_get(id, sprintf("attrs.%d.value", i)));
				strcpy(e.rb_icon, uib_get(id, sprintf("attrs.%d.icon", i)));
				y += D(36);
			}
			if (uib_get(id, "body") != "")
				y += D(10) + RB_LayoutText(id, uib_get(id, "body"), (compact ? "caption" : "body"), x, y + D(10), w, region);
			return max(y, imgbottom) - y0;
		}
		case "status":
		{
			float h = D(170);
			e = RB_Item("status", id, region, eX * x + eY * y, eX * w + eY * h);
			if (!e) return h;
			strcpy(e.rb_t1, uib_get(id, "title"));
			strcpy(e.rb_t2, uib_get(id, "body"));
			strcpy(e.rb_icon, uib_get(id, "icon"));
			e.rb_min = uib_has(id, "progress") ? stof(uib_get(id, "progress")) : -1;
			e.rb_sel = uib_get(id, "indeterminate") == "1";
			if (uib_has(id, "primary"))
			{
				float bw = RB_ButtonW(uib_get(id, "primary.label"), "") + D(24);
				RB_LayoutButtonAt(id, "primary", x + (w - bw) / 2, y + h + D(4), bw, region, 2);
				h += D(56);
			}
			return h;
		}
		case "progress":
		{
			e = RB_Item("progress", id, region, eX * x + eY * y, eX * w + eY * D(64));
			if (!e) return D(64);
			strcpy(e.rb_t1, uib_get(id, "label"));
			strcpy(e.rb_t2, uib_get(id, "detail"));
			e.rb_min = stof(uib_get(id, "value"));
			e.rb_sel = uib_get(id, "indeterminate") == "1";
			return D(64);
		}
		case "switch": case "slider": case "stepper": case "choice": case "field": case "keybind": case "colour":
			return RB_LayoutControl(id, t, x, y, w, region);
		case "split":
			return RB_LayoutChildren(id, x, y, w, region, D(10));
	}
	// unknown: a generic row if it has a title, else its children
	string tl = uib_get(id, "title");
	if (tl == "") tl = uib_get(id, "label");
	if (tl != "")
	{
		e = RB_Item("attr", id, region, eX * x + eY * y, eX * w + eY * D(40));
		if (e) { strcpy(e.rb_t1, tl); strcpy(e.rb_t2, uib_get(id, "subtitle")); }
		return D(40) + RB_LayoutChildren(id, x, y + D(44), w, region, D(8));
	}
	return RB_LayoutChildren(id, x, y, w, region, D(8));
}

void RB_SaveScroll(string screen)
{
	if (screen == "") return;
	int k = -1;
	for (int i = 0; i < RB_MEM; ++i)
		if (sm_id[i] == screen) k = i;
	if (k < 0)
	{
		for (int i = RB_MEM - 1; i > 0; --i) { strcpy(sm_id[i], sm_id[i - 1]); sm_off[i] = sm_off[i - 1]; }
		k = 0;
		strcpy(sm_id[0], screen);
	}
	sm_off[k] = rg_scroll[0];
}

float RB_LoadScroll(string screen)
{
	for (int i = 0; i < RB_MEM; ++i)
		if (sm_id[i] == screen) return sm_off[i];
	return 0;
}

// Settings: the split as vertical tabs (wide) or a list then a page (narrow)
float RB_LayoutSplit(string split, float top, float bottom, float cx, float cw)
{
	float W = rb_scr.x;
	string sel = uib_get(split, "value");
	bool open = UIB_VMf("settings.open");
	float sidew = min(D(260), W * 0.32);
	int pn = uib_count(split);
	float y = D(12);
	if (!rb_narrow || !open)
	{
		float sx = rb_narrow ? cx : D(12);
		float sw = rb_narrow ? cw : sidew - D(24);
		int reg = rb_narrow ? 0 : 4;
		float sy = D(10);
		for (int i = 0; i < pn; ++i)
		{
			string p = uib_child(split, i);
			entity e = RB_Item("sidetab", split, reg, eX * sx + eY * sy, eX * sw + eY * D(rb_narrow ? 60 : 48));
			if (!e) break;
			strcpy(e.rb_t1, uib_get(p, "label"));
			strcpy(e.rb_t2, rb_narrow ? uib_get(p, "subtitle") : "");
			strcpy(e.rb_icon, uib_get(p, "icon"));
			strcpy(e.rb_val, uib_get(p, "key"));
			e.rb_sel = (!rb_narrow && (e.rb_val == sel));
			e.rb_focus = 1;
			sy += D(rb_narrow ? 64 : 52);
		}
		if (!rb_narrow)
		{
			rg_o[4] = eY * top;
			rg_s[4] = eX * sidew + eY * (bottom - top);
			rg_content[4] = sy + D(10);
		}
		else
			return sy;
	}
	string pane = "";
	for (int i = 0; i < pn; ++i)
		if (uib_get(uib_child(split, i), "key") == sel)
			pane = uib_child(split, i);
	if (pane == "")
		return y;
	float dx = rb_narrow ? cx : D(20);
	float dw = rb_narrow ? cw : min(W - sidew - D(40), D(820));
	if (!rb_narrow)
	{
		rg_o[0] = eX * sidew + eY * top;
		rg_s[0] = eX * (W - sidew) + eY * (bottom - top);
	}
	string tabs = "";
	int pc = uib_count(pane);
	for (int i = 0; i < pc; ++i)
		if (uib_type(uib_child(pane, i)) == "tabs") tabs = uib_child(pane, i);
	if (tabs != "")
	{
		// the sub-tabs strip is sticky at the top of the content pane
		rg_o[5] = rg_o[0] + eX * dx;
		rg_s[5] = eX * dw + eY * D(46);
		rg_o[0] = rg_o[0] + eY * D(46);
		rg_s[0] = rg_s[0] - eY * D(46);
	}
	for (int i = 0; i < pc; ++i)
	{
		string c = uib_child(pane, i);
		float h = (c == tabs) ? RB_LayoutTabs(c, dx, y, dw, 0, 5) : RB_LayoutNode(c, dx, y, dw, 0);
		if (h > 0) y += h + D(12);
	}
	return y;
}

void RB_Layout()
{
	rb_n = 0;
	float W = rb_scr.x, H = rb_scr.y;
	float pad = D(rb_touch ? 20 : 28);
	float hdr = D(58);
	rb_narrow = W < D(600);
	rb_short = (H - hdr - D(66)) < D(330);
	if (uib_narrow != rb_narrow)
	{
		// the compact breakpoint changed (first layout, a resize, a rotation):
		// the screens that describe themselves by it (Home, Settings) follow
		uib_narrow = rb_narrow;
		UIB_RebuildAll();
	}
	for (int r = 0; r < RB_REGIONS; ++r) { rg_o[r] = '0 0 0'; rg_s[r] = '0 0 0'; rg_content[r] = 0; rg_max[r] = 0; }
	string sc = rb_top;
	// header: back, title, actions
	rg_s[1] = eX * W + eY * hdr;
	int nstack = stof(uib_get("app", "stack.#"));
	bool settings_open = sc == "settings" && rb_narrow && UIB_VMf("settings.open");
	bool canback = nstack > 1 || uib_get("app", "context") == "game" || settings_open;
	if (canback)
	{
		entity bk = RB_Item("back", sc, 1, eX * D(8) + eY * D(5), eX * D(48) + eY * D(48));
		if (bk) bk.rb_focus = 1;
	}
	// the engine's console button (its rect is published by touch_shellbtn.c in
	// _touch_shell_rect, fractions of the window): the header's actions end left of it
	rb_res_o = rb_res_s = '0 0 0';
	if (tokenize_console(cvar_string("_touch_shell_rect")) >= 4)
	{
		rb_res_o = eX * stof(argv(0)) * W + eY * stof(argv(1)) * H;
		rb_res_s = eX * stof(argv(2)) * W + eY * stof(argv(3)) * H;
	}
	float hright = W - D(12);
	if (rb_res_s.x > 0 && rb_res_o.y < hdr && rb_res_o.x + rb_res_s.x > W * 0.5)
		hright = rb_res_o.x - D(8);
	int na = stof(uib_get(sc, "actions.#"));
	float aw = 0;
	for (int i = 0; i < na; ++i)
		aw += RB_ButtonW(uib_get(sc, sprintf("actions.%d.label", i)), uib_get(sc, sprintf("actions.%d.icon", i))) + D(8);
	bool home = uib_get(sc, "kind") == "home";
	float tx0 = D(canback ? 66 : 24);
	// no room for title and labelled actions: the actions become icons
	bool iconsonly = aw > (hright - tx0) * (home ? 1 : 0.5);
	if (iconsonly) aw = na * D(56);
	if (!home)
	{
		entity tt = RB_Item("title", sc, 1, eX * tx0, eX * max(D(60), hright - aw - tx0 - D(8)) + eY * hdr);
		if (tt) strcpy(tt.rb_t1, uib_get(sc, "title"));
	}
	float ax = hright;
	for (int i = na - 1; i >= 0; --i)
	{
		float bw = iconsonly ? D(48) : RB_ButtonW(uib_get(sc, sprintf("actions.%d.label", i)), uib_get(sc, sprintf("actions.%d.icon", i)));
		ax -= bw;
		entity b = RB_Item("button", sc, 1, eX * ax + eY * D(5), eX * bw + eY * D(48));
		if (b) { b.rb_style = 2; RB_SetAction(b, sc, strcat("actions.", ftos(i))); if (iconsonly) { strcpy(b.rb_t2, b.rb_t1); strcpy(b.rb_t1, ""); } }
		ax -= D(8);
	}
	float top = hdr;
	float cw = min(W - 2 * pad, D(1100));
	float cx = (W - cw) / 2;
	int n = uib_count(sc);
	string toolbar = "", actionbar = "", split = "";
	for (int i = 0; i < n; ++i)
	{
		string c = uib_child(sc, i);
		string t = uib_type(c);
		if (t == "toolbar") toolbar = c;
		else if (t == "actionbar") actionbar = c;
		else if (t == "split") split = c;
	}
	if (toolbar != "")
	{
		rg_o[2] = eY * top;
		float th = D(6);
		int tn = uib_count(toolbar);
		for (int i = 0; i < tn; ++i)
		{
			string c = uib_child(toolbar, i);
			float h = 0;
			if (uib_type(c) == "field")
			{
				entity f = RB_Item("field", c, 2, eX * cx + eY * th, eX * cw + eY * D(48));
				if (f)
				{
					strcpy(f.rb_t2, uib_get(c, "placeholder"));
					strcpy(f.rb_val, uib_get(c, "value"));
					strcpy(f.rb_icon, uib_get(c, "kind"));
					f.rb_focus = 1;
					f.rb_size = 1;
				}
				h = D(48);
			}
			else
				h = RB_LayoutNode(c, cx, th, cw, 2);
			th += h + D(6);
		}
		rg_s[2] = eX * W + eY * (th + D(2));
		top += rg_s[2].y;
	}
	float bottom = H;
	if (actionbar != "" || uib_has(sc, "primary"))
	{
		float bh = D(66);
		rg_o[3] = eY * (H - bh);
		rg_s[3] = eX * W + eY * bh;
		if (actionbar != "")
		{
			int an = uib_count(actionbar);
			float fixed = 0;
			int nf = 0;
			for (int i = 0; i < an; ++i)
			{
				string c = uib_child(actionbar, i);
				if (uib_type(c) == "field") ++nf;
				else fixed += RB_ButtonW(uib_get(c, "label"), uib_get(c, "action.icon")) + D(8);
			}
			float fw = nf ? max(D(160), (cw - fixed) / nf) : 0;
			float xx = nf ? cx : cx + cw - fixed;
			for (int i = 0; i < an; ++i)
			{
				string c = uib_child(actionbar, i);
				if (uib_type(c) == "field")
				{
					entity f = RB_Item("field", c, 3, eX * xx + eY * D(9), eX * (fw - D(8)) + eY * D(48));
					if (f) { strcpy(f.rb_t2, uib_get(c, "placeholder")); strcpy(f.rb_val, uib_get(c, "value")); f.rb_focus = 1; f.rb_size = 1; }
					xx += fw;
				}
				else
				{
					float bw = RB_ButtonW(uib_get(c, "label"), uib_get(c, "action.icon"));
					RB_LayoutButtonAt(c, "action", xx, D(9), bw, 3, RB_StyleOf(uib_get(c, "style")));
					xx += bw + D(8);
				}
			}
		}
		else
		{
			float bw = max(D(200), RB_ButtonW(uib_get(sc, "primary.label"), uib_get(sc, "primary.icon")) + D(40));
			entity b = RB_Item("button", sc, 3, eX * (cx + cw - bw) + eY * D(9), eX * bw + eY * D(48));
			if (b) { RB_SetAction(b, sc, "primary"); b.rb_style = 1; }
		}
		bottom = H - bh;
	}
	rg_o[0] = eY * top;
	rg_s[0] = eX * W + eY * (bottom - top);
	float y = rb_short ? D(8) : D(14);
	if (split != "")
	{
		y = RB_LayoutSplit(split, top, bottom, cx, cw);
		for (int i = 0; i < n; ++i)
		{
			string c = uib_child(sc, i);
			if (c == split || uib_type(c) == "toolbar" || uib_type(c) == "actionbar") continue;
			if (rb_narrow && UIB_VMf("settings.open")) continue;
			if (rb_narrow)
				y += D(10) + RB_LayoutNode(c, cx, y + D(10), cw, 0);
			else
			{
				RB_LayoutNode(c, D(12), rg_content[4], rg_s[4].x - D(24), 4);
				rg_content[4] += D(60);
			}
		}
	}
	else
	{
		for (int i = 0; i < n; ++i)
		{
			string c = uib_child(sc, i);
			string t = uib_type(c);
			if (t == "toolbar" || t == "actionbar" || uib_get(c, "hidden") == "1") continue;
			float h = RB_LayoutNode(c, cx - rg_o[0].x, y, cw, 0);
			if (h > 0) y += h + D(16);
		}
	}
	rg_content[0] = y + D(28);
	if (uib_get(sc, "kind") == "home" && split == "" && rg_content[0] < rg_s[0].y)
	{
		// Home with room to spare: most of it goes to the banner, the rest
		// above it, so the page sits centred with a large banner
		float slack = rg_s[0].y - rg_content[0];
		float grow = 0, before = 0;
		for (int i = 0; i < rb_n; ++i)
		{
			entity it = rb_item[i];
			if (it.rb_region != 0) continue;
			if (it.rb_kind == "image" && uib_get(it.rb_id, "role") == "banner" && !grow)
			{
				grow = min(slack * 0.7, max(0, min(D(340), it.rb_s.x * 9 / 21) - it.rb_s.y));
				before = (slack - grow) * 0.4;
				it.rb_o_y += before;
				it.rb_s_y += grow;
				continue;
			}
			it.rb_o_y += before + grow;
		}
		rg_content[0] += before + grow;
	}
	rg_max[0] = max(0, rg_content[0] - rg_s[0].y);
	rg_max[4] = max(0, rg_content[4] - rg_s[4].y);
	// the top modal (dialog or sheet)
	if (rb_modal != "")
	{
		float mw = min(W - D(32), D(560));
		entity t2 = RB_Item("mtitle", rb_modal, 6, eX * D(20) + eY * D(18), eX * (mw - D(40)) + eY * D(30));
		if (t2) strcpy(t2.rb_t1, uib_get(rb_modal, "title"));
		float my = D(60);
		int mn = uib_count(rb_modal);
		int lastfirst = rb_n;
		float lasty = my;
		for (int i = 0; i < mn; ++i)
		{
			lastfirst = rb_n;
			lasty = my;
			float h = RB_LayoutNode(uib_child(rb_modal, i), D(20), my, mw - D(40), 6);
			if (h > 0) my += h + D(12);
		}
		if (uib_has(rb_modal, "primary") && !uib_has(strcat(rb_modal, "/form"), "submit"))
		{
			// the dialog's one button (a form's submit already is one)
			float pbw = RB_ButtonW(uib_get(rb_modal, "primary.label"), uib_get(rb_modal, "primary.icon")) + D(24);
			string lastc = mn ? uib_child(rb_modal, mn - 1) : "";
			if (lastc != "" && uib_type(lastc) == "row" && uib_get(lastc, "align") == "end" && uib_get(lastc, "label") == "")
			{
				// a closing row of buttons (Close): the primary joins it at the end
				for (int j = lastfirst; j < rb_n; ++j)
					rb_item[j].rb_o_x -= pbw + D(8);
				my = lasty;
			}
			entity pb = RB_Item("button", rb_modal, 6, eX * (mw - D(20) - pbw) + eY * my, eX * pbw + eY * D(48));
			if (pb) { RB_SetAction(pb, rb_modal, "primary"); pb.rb_style = 1; }
			my += D(48) + D(12);
		}
		my += D(10);
		float mh = min(my, H - D(48));
		bool sheet = uib_get(rb_modal, "presentation") == "sheet";
		rg_o[6] = eX * (W - mw) / 2 + eY * (sheet ? H - mh - D(8) : (H - mh) / 2);
		if (rb_res_s.x > 0 && rg_o[6].x + mw > rb_res_o.x && rg_o[6].y < rb_res_o.y + rb_res_s.y + D(8))
		{
			// the dialog would reach under the console button: start below it
			float newtop = rb_res_o.y + rb_res_s.y + D(8);
			mh = min(mh, H - newtop - D(8));
			rg_o[6].y = sheet ? H - mh - D(8) : newtop + max(0, (H - newtop - mh) / 2 - D(4));
		}
		rg_s[6] = eX * mw + eY * mh;
		rg_content[6] = my;
		rg_max[6] = max(0, my - mh);
	}
	for (int r = 0; r < RB_REGIONS; ++r)
		rg_scroll[r] = bound(0, rg_scroll[r], rg_max[r]);
}

// ======================================================================== virtual lists

// rows near the view: re-windowed only when the range, the query or the data changed
void RB_ListFetch(int k, float first, float want)
{
	string id = lc_id[k];
	string sort = (lc_sort[k] != "") ? lc_sort[k] : uib_get(id, "sort");
	float desc = (lc_sort[k] != "") ? lc_desc[k] : (uib_get(id, "desc") == "1");
	string filter = uib_get(id, "filter");
	string q = sprintf("%s|%s|%d|%s", lc_src[k], sort, desc, filter);
	float rev = uib_srcrev(lc_src[k]);
	first = max(0, floor(first));
	bool same = q == lc_q[k] && rev == lc_rev[k];
	if (same && first >= lc_first[k] && (first + want <= lc_first[k] + lc_cnt[k] || lc_first[k] + lc_cnt[k] >= lc_total[k]))
		return;
	string cols = "";
	int nc = stof(uib_get(id, "columns.#"));
	for (int i = 0; i < nc; ++i)
		cols = strcat(cols, uib_get(id, sprintf("columns.%d.key", i)), " ");
	cols = strcat(cols, "max bots lock title author name stale");
	strcpy(lc_cols[k], cols);
	float start = max(0, first - 24);
	int got = uib_window(lc_src[k], start, 120, sort, desc, filter);
	lc_total[k] = uib_wtotal();
	int nk = min(24, tokenize_console(cols));
	for (int j = 0; j < nk; ++j)
		strcpy(rb_colkey[j], argv(j));
	for (int i = 0; i < got; ++i)
	{
		string row = strcat(uib_wkey(i), "\t", uib_wimage(i));
		for (int j = 0; j < nk; ++j)
			row = strcat(row, "\t", uib_wcell(i, rb_colkey[j]));
		bufstr_set(lc_buf[k], i, row);
	}
	// keep the first visible row where it was when the rows above it changed
	if (!same && lc_anchor[k] != "" && q == lc_q[k])
	{
		for (int i = 0; i < got; ++i)
		{
			tokenizebyseparator(bufstr_get(lc_buf[k], i), "\t");
			if (argv(0) == lc_anchor[k])
			{
				float moved = (start + i) - lc_anchor_i[k];
				if (moved != 0)
					rg_scroll[0] = bound(0, rg_scroll[0] + moved * lc_rowh[k], rg_max[0] + fabs(moved) * lc_rowh[k]);
				break;
			}
		}
	}
	lc_first[k] = start;
	lc_cnt[k] = got;
	strcpy(lc_q[k], q);
	lc_rev[k] = rev;
}

string RB_Row(int k, float r)
{
	if (r < lc_first[k] || r >= lc_first[k] + lc_cnt[k])
		return "";
	return bufstr_get(lc_buf[k], r - lc_first[k]);
}

string RB_Cell(string row, string cols, string key)
{
	int nk = tokenize_console(cols);
	int col = -1;
	for (int j = 0; j < nk; ++j)
		if (argv(j) == key) { col = j; break; }
	if (col < 0) return "";
	tokenizebyseparator(row, "\t");
	return argv(col + 2);
}

// ======================================================================== images

bool RB_ImageReady(string path)
{
	if (path == "") return false;
	if (strstrofs(rb_loaded, strcat(" ", path, " "), 0) >= 0)
		return true;
	if (rb_imgbudget <= 0 || fabs(rg_vel[0]) > D(900))
		return false;
	--rb_imgbudget;
	precache_pic(path, PRECACHE_PIC_MIPMAP);
	if (cvar("_uib_r_debug") >= 3) print(sprintf("uibr load %s %v\n", path, drawgetimagesize(path)));
	string s = strcat(rb_loaded, path, " ");
	if (strlen(s) > 6000)
		s = strcat(" ", path, " ");
	if (substring(s, 0, 1) != " ") s = strcat(" ", s);
	strcpy(rb_loaded, s);
	return true;
}

void RB_Shimmer(vector o, vector s, float a)
{
	RB_Fill(o, s, RB_CARD_HI, 0.55 * a);
	if (rb_reduced) return;
	float p = (time * 0.8) - floor(time * 0.8);
	float bw = s.x * 0.35;
	float bx = o.x - bw + p * (s.x + bw);
	float x0 = max(o.x, bx), x1 = min(o.x + s.x, bx + bw);
	if (x1 > x0)
		RB_Fill(eX * x0 + eY * o.y, eX * (x1 - x0) + eY * s.y, RB_BLUE, 0.08 * a);
}

void RB_Image(string ref, vector o, vector s, float a, bool contain)
{
	// small boxes draw a cached thumbnail (cheap to load again); big ones the full picture
	string path;
	if (s.x < D(420) && s.y < D(260) && substring(ref, 0, 4) == "pk3:")
	{
		string key = strcat(ref, "@thumb");
		if (strstrofs(rb_loaded, strcat(" ", key, "=", ""), 0) >= 0)
		{
			int k = strstrofs(rb_loaded, strcat(" ", key, "="), 0) + strlen(key) + 2;
			path = substring(rb_loaded, k, strstrofs(rb_loaded, " ", k) - k);
		}
		else if (rb_imgbudget > 0 && fabs(rg_vel[0]) <= D(900))
		{
			path = uib_thumb(ref, 384, 216);
			if (cvar("_uib_r_debug") >= 3) print(sprintf("uibr thumb %s -> %s\n", ref, path));
			string nl = strcat(((rb_loaded == "") ? " " : rb_loaded), key, "=", path, " ");   // strcpy frees its target first: build the new value before
			strcpy(rb_loaded, nl);
		}
		else
		{
			RB_Shimmer(o, s, a);
			return;
		}
	}
	else
		path = uib_imgpath(ref);
	if (!RB_ImageReady(path))
	{
		RB_Shimmer(o, s, a);
		return;
	}
	vector isz = drawgetimagesize(path);
	if (isz.x <= 0 || isz.y <= 0)
	{
		RB_Shimmer(o, s, a);
		return;
	}
	float ia = isz.x / isz.y, ba = s.x / max(1, s.y);
	if (contain)
	{
		vector d = (ia > ba) ? (eX * s.x + eY * s.x / ia) : (eX * s.y * ia + eY * s.y);
		drawpic(o + (s - d) * 0.5, path, d, '1 1 1', a, DRAWFLAG_NORMAL);
		return;
	}
	vector sp = '0 0 0', ss = '1 1 0';
	if (ia > ba) { ss.x = ba / ia; sp.x = (1 - ss.x) / 2; }
	else { ss.y = ia / ba; sp.y = (1 - ss.y) / 2; }
	drawsubpic(o, s, path, sp, ss, '1 1 1', a, DRAWFLAG_NORMAL);
}

// ======================================================================== drawing

vector RB_ItemPos(entity e)
{
	vector p = rg_o[e.rb_region] + e.rb_o;
	if (e.rb_region == 0 || e.rb_region == 4 || e.rb_region == 6)
		p.y -= rg_scroll[e.rb_region];
	if (e.rb_hs >= 0)
		p.x -= hs_scroll[e.rb_hs];
	return p;
}

string RB_FmtValue(float v, string fmt)
{
	switch (fmt)
	{
		case "percent": return sprintf("%d%%", rint(v * 100));
		case "deg": return sprintf("%d°", rint(v));
		case "x": return sprintf("%.2f×", v);
		case "float1": return sprintf("%.1f", v);
		case "float2": return sprintf("%.2f", v);
		case "ms": return sprintf("%d ms", rint(v));
	}
	return ftos(rint(v));
}

void RB_DrawButton(entity e, vector p, bool hot, bool pressed)
{
	vector fill = RB_CARD, txt = RB_TEXT, line = RB_BLUE;
	float la = 0.45;
	switch (e.rb_style)
	{
		case 1: fill = RB_EMBER; txt = RB_INK; line = RB_EMBER; la = 1; break;
		case 3: fill = '0.35 0.08 0.1'; txt = '1 0.78 0.78'; line = RB_DANGER; la = 0.7; break;
		case 4: txt = RB_BLUE; la = 0; break;
		case 5: fill = '0.12 0.22 0.45'; la = 0.25; break;
	}
	if (hot) fill = fill * 1.18;
	if (pressed) fill = fill * 0.82;
	float a = rb_a * (e.rb_dis ? 0.4 : 1);
	if (e.rb_style != 4)
		RB_Card(p, e.rb_s, D(12), fill, a, line, la * a);
	string g = RB_Glyph(e.rb_icon);
	float sz = D(15);
	drawfont = FONT_USER + 3;
	float tw = RB_TextW(e.rb_t1, sz) + ((g != "") ? D(24) : 0);
	drawfont = FONT_USER;
	float tx = p.x + max(D(12), (e.rb_s.x - tw) / 2);
	if (g != "")
	{
		RB_Text(eX * tx + eY * (p.y + (e.rb_s.y - D(17)) / 2), g, D(17), txt, a);
		tx += D(24);
	}
	drawfont = FONT_USER + 3;
	RB_TextFit(eX * tx + eY * (p.y + (e.rb_s.y - sz) / 2), e.rb_t1, sz, e.rb_s.x - (tx - p.x) - D(8), txt, a);
	drawfont = FONT_USER;
}

float rt_stale;   // the row being drawn has not answered yet this session
void RB_DrawTable(entity e, vector p)
{
	int k = e.rb_list;
	if (k < 0) return;
	string id = e.rb_id;
	float head = e.rb_size;
	float rowh = lc_rowh[k];
	float W = e.rb_s.x;
	float top = rg_o[e.rb_region].y, bot = top + rg_s[e.rb_region].y;
	float first = max(0, floor((top - (p.y + head)) / rowh));
	float last = min(lc_total[k], ceil((bot - (p.y + head)) / rowh) + 1);
	RB_ListFetch(k, first, last - first + 1);
	p = RB_ItemPos(e);   // the fetch may have moved the scroll to keep the anchor row in place
	last = min(lc_total[k], last);
	int nc = min(16, stof(uib_get(id, "columns.#")));
	float fixed = 0, fills = 0;
	for (int i = 0; i < nc; ++i)
	{
		rt_ck[i] = uib_get(id, sprintf("columns.%d.key", i));
		rt_ct[i] = uib_get(id, sprintf("columns.%d.type", i));
		float pr = stof(uib_get(id, sprintf("columns.%d.priority", i)));
		string wc = uib_get(id, sprintf("columns.%d.width", i));
		float cw = 0;
		switch (wc)
		{
			case "xs": cw = D(32); break;
			case "s": cw = D(84); break;
			case "m": cw = D(140); break;
			case "l": cw = D(220); break;
		}
		if ((W < D(560) && pr >= 2) || (W < D(760) && pr >= 3)) { rt_ws[i] = -1; continue; }
		rt_ws[i] = cw;
		if (cw > 0) fixed += cw; else ++fills;
	}
	float fillw = fills ? max(D(80), (W - fixed - D(16)) / fills) : 0;
	float xx = p.x + D(8);
	for (int i = 0; i < nc; ++i)
	{
		if (rt_ws[i] < 0) continue;
		if (rt_ws[i] == 0) rt_ws[i] = fillw;
		rt_xs[i] = xx;
		xx += rt_ws[i];
	}
	if (lc_total[k] <= 0)
	{
		vector eo = p + eY * (head + D(24));
		drawfont = FONT_USER + 3;
		RB_TextCenter(eo, eX * W + eY * D(26), uib_get(id, "empty.title"), D(17), RB_TEXT, rb_a);
		drawfont = FONT_USER;
		RB_TextCenter(eo + eY * D(32), eX * W + eY * D(20), uib_get(id, "empty.text"), D(13), RB_MUTED, rb_a);
	}
	string rt_sel = uib_get(id, "selected.0");
	string firstkey = "";
	for (float r = first; r < last; ++r)
	{
		vector ro = p + eY * (head + r * rowh);
		if (r % 2 == 1) RB_Fill(ro, eX * W + eY * rowh, RB_SURFACE, 0.55 * rb_a);
		if (rb_hover >= 0 && rb_item[rb_hover] == e && rb_mouse.y >= ro.y && rb_mouse.y < ro.y + rowh)
			RB_Fill(ro, eX * W + eY * rowh, RB_BLUE, 0.14 * rb_a);
		string row = RB_Row(k, r);
		rt_stale = (row != "" && RB_Cell(row, lc_cols[k], "stale") == "1");
		if (row != "" && rt_sel != "")
		{
			// the chosen row: ember wash and a bar at its left edge
			tokenizebyseparator(row, "\t");
			if (argv(0) == rt_sel)
			{
				RB_Fill(ro, eX * W + eY * rowh, RB_EMBER, 0.22 * rb_a);
				RB_Fill(ro, eX * D(4) + eY * rowh, RB_EMBER, rb_a);
			}
		}
		if (row == "")
		{
			RB_Shimmer(ro + eX * D(12) + eY * rowh * 0.3, eX * (W * 0.5) + eY * rowh * 0.4, rb_a);
			continue;
		}
		if (firstkey == "" && ro.y + rowh * 0.5 >= top + head)
		{
			tokenizebyseparator(row, "\t");
			firstkey = argv(0);
			lc_anchor_i[k] = r;
		}
		for (int i = 0; i < nc; ++i)
		{
			if (rt_ws[i] < 0) continue;
			string v = RB_Cell(row, lc_cols[k], rt_ck[i]);
			vector co = eX * rt_xs[i] + eY * (ro.y + (rowh - D(14)) / 2);
			vector col = RB_TEXT;
			if (rt_ct[i] == "lock") { if (v == "1") RB_Text(co, "■", D(13), RB_WARN, rb_a); continue; }
			if (rt_ct[i] == "bool") { RB_Text(co, ((v == "1") ? "✓" : "–"), D(14), ((v == "1") ? RB_OK : RB_MUTED), rb_a); continue; }
			if (rt_ct[i] == "ping")
			{
				float pg = stof(v);
				col = (pg <= 0) ? RB_MUTED : ((pg < 80) ? RB_OK : ((pg < 160) ? RB_WARN : RB_DANGER));
				v = (pg > 0) ? ftos(pg) : "–";
			}
			else if (rt_ct[i] == "players")
				v = sprintf("%s/%s", v, RB_Cell(row, lc_cols[k], "max"));
			else if (rt_ct[i] == "mode") { v = UIB_ModeName(v); col = RB_MUTED; }
			else if (rt_ct[i] == "map") col = RB_MUTED;
			if (uib_get(id, sprintf("columns.%d.align", i)) == "end")
				co.x = rt_xs[i] + rt_ws[i] - D(10) - min(RB_TextW(v, D(14)), rt_ws[i] - D(10));
			RB_TextFit(co, v, D(14), rt_ws[i] - D(10), col, rb_a * (rt_stale ? 0.45 : 1));   // a row saved from last time, not yet answered: dimmed
		}
	}
	if (firstkey != "") strcpy(lc_anchor[k], firstkey);
	if (head > 0)
	{
		// the column header row sticks to the top of the region while rows scroll
		vector ho = p;
		if (ho.y < top && p.y + e.rb_s.y > top + head) ho.y = top;
		RB_Fill(ho, eX * W + eY * head, RB_SURFACE, 0.98 * rb_a);
		RB_Fill(ho + eY * (head - 1), eX * W + eY * 1, RB_BLUE, 0.35 * rb_a);
		string sort = (lc_sort[k] != "") ? lc_sort[k] : uib_get(id, "sort");
		float desc = (lc_sort[k] != "") ? lc_desc[k] : (uib_get(id, "desc") == "1");
		drawfont = FONT_USER + 3;
		for (int i = 0; i < nc; ++i)
		{
			if (rt_ws[i] < 0) continue;
			string lab = uib_get(id, sprintf("columns.%d.label", i));
			if (rt_ck[i] == sort) lab = strcat(lab, (desc ? " ▼" : " ▲"));
			RB_TextFit(eX * rt_xs[i] + eY * (ho.y + (head - D(13)) / 2), lab, D(13), rt_ws[i] - D(8), ((rt_ck[i] == sort) ? RB_BLUE : RB_MUTED), rb_a);
		}
		drawfont = FONT_USER;
	}
}

// A card list of N rows that scrolls sideways (the map picker): tiles are laid
// column by column, only the columns in view are fetched and drawn, and a thin
// scroll bar sits inside the list under the tiles.
void RB_DrawCardsSideways(entity e, vector p, string sel)
{
	int k = e.rb_list;
	int rows = lc_rows[k];
	float ch = lc_cardh[k], cw = lc_cardw[k], gap = D(10), band = lc_band[k];
	float W = rb_scr.x;
	float c0 = max(0, floor((0 - p.x) / (cw + gap))), c1 = ceil((W - p.x) / (cw + gap));
	float first = c0 * rows, last = min(lc_total[k], (c1 + 1) * rows);
	RB_ListFetch(k, first, last - first + 1);
	if (lc_total[k] <= 0)
	{
		RB_TextCenter(eX * (p.x + hs_scroll[e.rb_hs]) + eY * (p.y + D(30)), eX * lc_vieww[k] + eY * D(26), uib_get(e.rb_id, "empty.title"), D(16), RB_MUTED, rb_a);
		return;
	}
	for (float i = first; i < last; ++i)
	{
		vector co = p + eX * floor(i / rows) * (cw + gap) + eY * (i % rows) * (ch + gap);
		if (co.x > W || co.x + cw < 0) continue;
		string row = RB_Row(k, i);
		tokenizebyseparator(row, "\t");
		string key = argv(0), img = argv(1);
		bool issel = key != "" && key == sel;
		bool hot = !rb_touch && rb_hover >= 0 && rb_item[rb_hover] == e && rb_mouse.x >= co.x && rb_mouse.x < co.x + cw && rb_mouse.y >= co.y && rb_mouse.y < co.y + ch;
		RB_Card(co, eX * cw + eY * ch, D(12), (hot ? RB_CARD_HI : RB_CARD), rb_a, (issel ? RB_EMBER : RB_BLUE), (issel ? 1 : 0.3) * rb_a);
		vector io = co + eX * D(3) + eY * D(3);
		vector is = eX * (cw - D(6)) + eY * (ch - band - D(3));
		if (row == "")
		{
			RB_Shimmer(io, is, rb_a);
			continue;
		}
		RB_Image(img, io, is, rb_a, false);
		string title = RB_Cell(row, lc_cols[k], "title");
		if (title == "") title = RB_Cell(row, lc_cols[k], "name");
		drawfont = FONT_USER + 3;
		RB_TextFit(co + eX * D(10) + eY * (ch - band + (rb_short ? D(5) : D(7))), title, (rb_short ? D(12) : D(14)), cw - D(20) - (issel ? D(24) : 0), (issel ? RB_EMBER : RB_TEXT), rb_a);
		drawfont = FONT_USER;
		if (issel)
		{
			RB_RRect(co + eX * (cw - D(28)) + eY * (ch - band + D(4)), eX * D(22) + eY * D(22), D(11), RB_EMBER, rb_a);
			RB_TextCenter(co + eX * (cw - D(28)) + eY * (ch - band + D(4)), eX * D(22) + eY * D(22), "✓", D(14), RB_INK, rb_a);
		}
	}
	// the scroll bar, inside the list
	float hs = e.rb_hs;
	if (hs >= 0 && hs_max[hs] > 0)
	{
		float vw = lc_vieww[k];
		vector bo = eX * (p.x + hs_scroll[hs]) + eY * (p.y + e.rb_s.y - D(6));
		RB_RRect(bo, eX * vw + eY * D(4), D(2), RB_SURFACE, 0.9 * rb_a);
		float tw = max(D(40), vw * vw / (vw + hs_max[hs]));
		RB_RRect(bo + eX * (vw - tw) * (hs_scroll[hs] / hs_max[hs]), eX * tw + eY * D(4), D(2), RB_BLUE, 0.85 * rb_a);
	}
}

void RB_DrawCards(entity e, vector p)
{
	int k = e.rb_list;
	if (k < 0) return;
	string id = e.rb_id;
	if (lc_rows[k] > 0)
	{
		RB_DrawCardsSideways(e, p, uib_get(id, "selected.0"));
		return;
	}
	int cols = lc_percol[k];
	float ch = lc_cardh[k], cw = lc_cardw[k], rowh = lc_rowh[k], gap = D(12);
	float top = rg_o[e.rb_region].y, bot = top + rg_s[e.rb_region].y;
	float r0 = max(0, floor((top - p.y) / rowh)), r1 = ceil((bot - p.y) / rowh);
	float first = r0 * cols, last = min(lc_total[k], (r1 + 1) * cols);
	RB_ListFetch(k, first, last - first + 1);
	last = min(lc_total[k], last);
	if (lc_total[k] <= 0)
	{
		RB_TextCenter(p + eY * D(30), eX * e.rb_s.x + eY * D(26), uib_get(id, "empty.title"), D(16), RB_MUTED, rb_a);
		return;
	}
	string sel = uib_get(id, "selected.0");
	for (float i = first; i < last; ++i)
	{
		vector co = p + eX * (i % cols) * (cw + gap) + eY * floor(i / cols) * rowh;
		if (co.y > bot || co.y + ch < top) continue;
		string row = RB_Row(k, i);
		bool hot = !rb_touch && rb_hover >= 0 && rb_item[rb_hover] == e && rb_mouse.x >= co.x && rb_mouse.x < co.x + cw && rb_mouse.y >= co.y && rb_mouse.y < co.y + ch;
		tokenizebyseparator(row, "\t");
		string key = argv(0), img = argv(1);
		bool issel = key != "" && key == sel;
		RB_Card(co, eX * cw + eY * ch, D(14), (hot ? RB_CARD_HI : RB_CARD), rb_a, (issel ? RB_EMBER : RB_BLUE), (issel ? 1 : 0.3) * rb_a);
		float ih = ch - D(54);
		vector io = co + eX * D(4) + eY * D(4);
		vector is = eX * (cw - D(8)) + eY * (ih - D(4));
		if (row == "")
		{
			RB_Shimmer(io, is, rb_a);
			continue;
		}
		RB_Image(img, io, is, rb_a, false);
		string title = RB_Cell(row, lc_cols[k], "title");
		if (title == "") title = RB_Cell(row, lc_cols[k], "name");
		drawfont = FONT_USER + 3;
		RB_TextFit(co + eX * D(12) + eY * (ih + D(8)), title, D(15), cw - D(24), RB_TEXT, rb_a);
		drawfont = FONT_USER;
		string line2 = RB_Cell(row, lc_cols[k], "author");
		if (line2 == "") line2 = RB_Cell(row, lc_cols[k], "modeName");
		string state = RB_Cell(row, lc_cols[k], "state");
		if (state == "locked") line2 = strcat(_("Locked"), " · ", line2);
		else if (state == "done") line2 = strcat("✓ ", line2);
		RB_TextFit(co + eX * D(12) + eY * (ih + D(29)), line2, D(12), cw - D(24), RB_MUTED, rb_a);
		if (state == "locked")
			RB_RRect(co, eX * cw + eY * ch, D(14), RB_BG, 0.55 * rb_a);
		if (issel)
		{
			RB_RRect(co + eX * (cw - D(36)) + eY * D(10), eX * D(26) + eY * D(26), D(13), RB_EMBER, rb_a);
			RB_TextCenter(co + eX * (cw - D(36)) + eY * D(10), eX * D(26) + eY * D(26), "✓", D(16), RB_INK, rb_a);
		}
	}
}

void RB_DrawItem(entity e, int idx)
{
	vector p = RB_ItemPos(e);
	vector s = e.rb_s;
	float rt = rg_o[e.rb_region].y, rbt = rt + rg_s[e.rb_region].y;
	if (p.y > rbt || p.y + s.y < rt)
		return;
	bool hot = (idx == rb_hover) && !rb_touch;
	bool pressed = (idx == rb_press_item) && rb_pressed && !rb_dragging;
	float a = rb_a;
	switch (e.rb_kind)
	{
		case "title":
			drawfont = FONT_USER + 3;
			RB_TextFit(p + eY * (s.y - D(22)) / 2, e.rb_t1, D(22), s.x, RB_TEXT, a);
			drawfont = FONT_USER;
			break;
		case "back":
			RB_Card(p, s, D(24), (hot ? RB_CARD_HI : RB_CARD), a, RB_BLUE, 0.4 * a);
			RB_TextCenter(p, s, "←", D(24), RB_TEXT, a);
			break;
		case "mtitle":
			drawfont = FONT_USER + 3;
			RB_TextFit(p, e.rb_t1, D(20), s.x, RB_TEXT, a);
			drawfont = FONT_USER;
			break;
		case "line":
			if (e.rb_style) drawfont = FONT_USER + 3;
			RB_TextFit(p + eY * (s.y - e.rb_size) / 2, e.rb_t1, e.rb_size, s.x, e.rb_col, a);
			drawfont = FONT_USER;
			break;
		case "label":
			RB_TextFit(p + eY * (min(s.y, D(52)) - D(15)) / 2, e.rb_t1, D(15), s.x, RB_TEXT, a);
			break;
		case "sechead":
			drawfont = FONT_USER + 3;
			RB_Text(p + eY * D(12), strtoupper(e.rb_t1), D(13), RB_BLUE, a);
			drawfont = FONT_USER;
			RB_Fill(p + eX * (RB_TextW(strtoupper(e.rb_t1), D(13)) + D(12)) + eY * D(18), eX * D(40) + eY * max(1, D(1.5)), RB_EMBER, 0.8 * a);
			if (e.rb_t2 != "") RB_TextFit(p + eY * D(30), e.rb_t2, D(12), s.x, RB_MUTED, a);
			break;
		case "divider": RB_Fill(p, s, RB_BLUE, 0.2 * a); break;
		case "rowlabel":
			RB_TextFit(p + eY * (s.y - D(13)) / 2, strcat(RB_Glyph(e.rb_icon), "  ", e.rb_t1), D(13), s.x - D(6), RB_MUTED, a);
			break;
		case "button": RB_DrawButton(e, p, hot, pressed); break;
		case "card":
		{
			vector line = e.rb_sel ? RB_EMBER : RB_BLUE;
			RB_Card(p, s, D(16), ((hot || pressed) ? RB_CARD_HI : RB_CARD), a, line, (e.rb_sel ? 1 : (e.rb_style ? 0.85 : 0.3)) * a);
			float ty = p.y + D(14);
			if (e.rb_img != "")
			{
				RB_Image(e.rb_img, p + eX * D(4) + eY * D(4), eX * (s.x - D(8)) + eY * (e.rb_size - D(4)), a, false);
				ty = p.y + e.rb_size + D(10);
			}
			float tx = p.x + D(16);
			string g = RB_Glyph(e.rb_icon);
			if (g != "" && e.rb_img == "")
			{
				RB_RRect(eX * tx + eY * (ty - D(2)), eX * D(34) + eY * D(34), D(10), (e.rb_style ? RB_EMBER : RB_BLUE), 0.18 * a);
				RB_TextCenter(eX * tx + eY * (ty - D(2)), eX * D(34) + eY * D(34), g, D(18), (e.rb_style ? RB_EMBER : RB_BLUE), a);
				tx += D(46);
			}
			drawfont = FONT_USER + 3;
			RB_TextFit(eX * tx + eY * ty, e.rb_t1, D(16), p.x + s.x - tx - D(12), RB_TEXT, a);
			drawfont = FONT_USER;
			RB_TextFit(eX * tx + eY * (ty + D(22)), e.rb_t2, D(12), p.x + s.x - tx - D(12), RB_MUTED, a);
			int nm = stof(uib_get(e.rb_id, "meta.#"));
			float mx = p.x + D(16);
			for (int i = 0; i < nm && i < 4; ++i)
			{
				string mt = strcat(RB_Glyph(uib_get(e.rb_id, sprintf("meta.%d.icon", i))), " ", uib_get(e.rb_id, sprintf("meta.%d.text", i)));
				float mw = RB_TextW(mt, D(12)) + D(16);
				if (mx + mw > p.x + s.x - D(8)) break;
				RB_Card(eX * mx + eY * (ty + D(44)), eX * mw + eY * D(22), D(11), RB_SURFACE, a, RB_BLUE, 0.25 * a);
				RB_Text(eX * (mx + D(8)) + eY * (ty + D(49)), mt, D(12), RB_TEXT, a);
				mx += mw + D(6);
			}
			int nb = stof(uib_get(e.rb_id, "badges.#"));
			for (int i = 0; i < nb && i < 2; ++i)
			{
				string bt = strcat(RB_Glyph(uib_get(e.rb_id, sprintf("badges.%d.icon", i))), " ", uib_get(e.rb_id, sprintf("badges.%d.text", i)));
				float bw = RB_TextW(bt, D(12)) + D(16);
				vector bo = p + eX * (s.x - bw - D(10)) + eY * (D(10) + i * D(26));
				RB_Card(bo, eX * bw + eY * D(22), D(11), '0.3 0.2 0.05', a, RB_WARN, 0.6 * a);
				RB_Text(bo + eX * D(8) + eY * D(5), bt, D(12), RB_WARN, a);
			}
			if (e.rb_sel && s.y > D(60))   // a chip is marked by its outline alone
			{
				RB_RRect(p + eX * (s.x - D(36)) + eY * D(10), eX * D(26) + eY * D(26), D(13), RB_EMBER, a);
				RB_TextCenter(p + eX * (s.x - D(36)) + eY * D(10), eX * D(26) + eY * D(26), "✓", D(16), RB_INK, a);
			}
			break;
		}
		case "hero":
		{
			RB_Card(p, s, D(20), RB_CARD, a, RB_EMBER, 0.95 * a);
			RB_Image(e.rb_img, p + eX * D(2) + eY * D(2), s - eX * D(4) - eY * D(4), a, false);
			float bh = s.y * 0.55;
			float y0 = p.y + s.y - D(2) - bh;
			for (int i = 0; i < 24; ++i)
			{
				float ya = floor(y0 + i * bh / 24), yb = floor(y0 + (i + 1) * bh / 24);
				RB_Fill(eX * (p.x + D(2)) + eY * ya, eX * (s.x - D(4)) + eY * (yb - ya), RB_BG, a * (0.04 + 0.85 * (i / 23) * (i / 23)));
			}
			drawfont = FONT_USER + 3;
			RB_TextFit(p + eX * D(24) + eY * (s.y - D(82)), e.rb_t1, D(38), s.x - D(48), RB_TEXT, a);
			drawfont = FONT_USER;
			RB_TextFit(p + eX * D(26) + eY * (s.y - D(36)), e.rb_t2, D(15), s.x - D(52), RB_TEXT * 0.9, a);
			RB_Fill(p + eX * D(24) + eY * (s.y - D(42)), eX * D(56) + eY * max(1, D(2)), RB_EMBER, a);
			break;
		}
		case "image":
			RB_Image(e.rb_img, p, s, a, e.rb_sel);
			break;
		case "chip":
			RB_Card(p, s, D(14), '0.3 0.2 0.05', a, RB_WARN, 0.6 * a);
			RB_TextCenter(p, s, strcat(RB_Glyph(e.rb_icon), " ", e.rb_t1), D(12), RB_WARN, a);
			break;
		case "attr":
			RB_Fill(p + eY * (s.y - 1), eX * s.x + eY * 1, RB_BLUE, 0.12 * a);
			RB_TextFit(p + eY * D(11), strcat(RB_Glyph(e.rb_icon), "  ", e.rb_t1), D(13), s.x * 0.34, RB_MUTED, a);
			RB_TextFit(p + eX * s.x * 0.36 + eY * D(10), e.rb_t2, D(14), s.x * 0.64, RB_TEXT, a);
			break;
		case "switch": case "check":
		{
			if (hot) RB_RRect(p - eX * D(6), s + eX * D(12), D(10), RB_CARD_HI, 0.5 * a);
			bool on = e.rb_sel;
			string lab = e.rb_t1;
			if (e.rb_icon != "" && RB_Glyph(e.rb_icon) != "") lab = strcat(RB_Glyph(e.rb_icon), " ", lab);
			if (e.rb_kind == "check")
			{
				vector bo = p + eY * (D(52) - D(26)) / 2;
				RB_Card(bo, eX * D(26) + eY * D(26), D(7), (on ? RB_EMBER : RB_SURFACE), a, (on ? RB_EMBER : RB_BLUE), 0.8 * a);
				if (on) RB_TextCenter(bo, eX * D(26) + eY * D(26), "✓", D(18), RB_INK, a);
				RB_TextFit(p + eX * D(36) + eY * (D(52) - D(15)) / 2, lab, D(15), s.x - D(40), RB_TEXT, a);
			}
			else
			{
				RB_TextFit(p + eY * (D(52) - D(15)) / 2, lab, D(15), s.x - D(70), RB_TEXT, a);
				if (e.rb_t2 != "") RB_TextFit(p + eY * D(42), e.rb_t2, D(12), s.x - D(70), RB_MUTED, a);
				vector to = p + eX * (s.x - D(54)) + eY * (D(52) - D(28)) / 2;
				RB_Card(to, eX * D(52) + eY * D(28), D(14), (on ? RB_EMBER : RB_SURFACE), a, (on ? RB_EMBER : RB_BLUE), 0.6 * a);
				RB_RRect(to + eX * (on ? D(27) : D(5)) + eY * D(4), eX * D(20) + eY * D(20), D(10), (on ? '1 1 1' : RB_MUTED), a);
			}
			break;
		}
		case "slider":
		{
			float v = (rb_slider_item == idx) ? rb_local_val : stof(e.rb_val);
			RB_TextFit(p + eY * D(10), e.rb_t1, D(15), s.x * 0.7, RB_TEXT, a);
			string vt = RB_FmtValue(v, e.rb_t2);
			RB_Text(p + eX * (s.x - RB_TextW(vt, D(15))) + eY * D(10), vt, D(15), RB_EMBER, a);
			float f = (e.rb_max > e.rb_min) ? bound(0, (v - e.rb_min) / (e.rb_max - e.rb_min), 1) : 0;
			vector tr = p + eY * D(46);
			RB_RRect(tr, eX * s.x + eY * D(6), D(3), RB_SURFACE, a);
			RB_RRect(tr, eX * s.x * f + eY * D(6), D(3), RB_BLUE, a);
			RB_RRect(tr + eX * (s.x * f - D(11)) - eY * D(8), eX * D(22) + eY * D(22), D(11), ((rb_slider_item == idx) ? RB_EMBER : RB_TEXT), a);
			break;
		}
		case "stepper":
		{
			RB_TextFit(p + eY * D(16), e.rb_t1, D(15), s.x - D(170), RB_TEXT, a);
			if (e.rb_t2 != "") RB_TextFit(p + eY * D(44), e.rb_t2, D(12), s.x - D(170), ((uib_get(e.rb_id, "error") != "") ? RB_WARN : RB_MUTED), a);
			vector bo = p + eX * (s.x - D(160)) + eY * D(4);
			RB_Card(bo, eX * D(44) + eY * D(44), D(10), RB_CARD, a, RB_BLUE, 0.4 * a);
			RB_TextCenter(bo, eX * D(44) + eY * D(44), "−", D(20), RB_TEXT, a);
			RB_Card(bo + eX * D(116), eX * D(44) + eY * D(44), D(10), RB_CARD, a, RB_BLUE, 0.4 * a);
			RB_TextCenter(bo + eX * D(116), eX * D(44) + eY * D(44), "+", D(20), RB_TEXT, a);
			drawfont = FONT_USER + 3;
			RB_TextCenter(bo + eX * D(44), eX * D(72) + eY * D(44), strcat(e.rb_val, (e.rb_icon != "") ? strcat(" ", e.rb_icon) : ""), D(16), RB_TEXT, a);
			drawfont = FONT_USER;
			break;
		}
		case "opt":
			RB_Card(p, s, D(10), (e.rb_sel ? RB_BLUE : RB_SURFACE), a, RB_BLUE, (e.rb_sel ? 1 : 0.35) * a);
			drawfont = e.rb_sel ? FONT_USER + 3 : FONT_USER;
			RB_TextCenter(p, s, e.rb_t1, D(14), (e.rb_sel ? '1 1 1' : RB_TEXT), a);
			drawfont = FONT_USER;
			break;
		case "dropdown":
		{
			RB_TextFit(p + eY * (D(52) - D(15)) / 2, e.rb_t1, D(15), s.x * 0.45, RB_TEXT, a);
			vector bo = p + eX * (s.x * 0.5) + eY * D(4);
			vector bs = eX * (s.x * 0.5) + eY * D(44);
			RB_Card(bo, bs, D(10), (hot ? RB_CARD_HI : RB_CARD), a, RB_BLUE, 0.45 * a);
			RB_TextFit(bo + eX * D(12) + eY * D(14), e.rb_t2, D(15), bs.x - D(40), RB_TEXT, a);
			RB_Text(bo + eX * (bs.x - D(26)) + eY * D(13), "▼", D(14), RB_BLUE, a);
			break;
		}
		case "field":
		{
			bool editing = rb_edit_id == e.rb_id;
			bool compact = e.rb_size;   // toolbar and action bar fields: no label line
			vector bo = compact ? p : p + eY * D(24);
			vector bs = eX * s.x + eY * D(48);
			if (!compact) RB_TextFit(p, e.rb_t1, D(13), s.x, RB_MUTED, a);
			RB_Card(bo, bs, D(10), RB_SURFACE, a, (editing ? RB_EMBER : RB_BLUE), (editing ? 1 : 0.45) * a);
			string txt = editing ? rb_edit_text : e.rb_val;
			if (e.rb_sel)
			{
				int nchar = editing ? strlen(rb_edit_text) : (e.rb_style ? 6 : 0);
				txt = "";
				for (int i = 0; i < nchar; ++i) txt = strcat(txt, "•");
			}
			float gx = (e.rb_icon == "search") ? D(34) : D(12);
			if (e.rb_icon == "search") RB_Text(bo + eX * D(10) + eY * D(13), "⌕", D(18), RB_MUTED, a);
			if (txt == "" && !editing)
				RB_TextFit(bo + eX * gx + eY * D(15), e.rb_t2, D(15), bs.x - gx - D(12), RB_MUTED * 0.8, a);
			else
			{
				string shown = txt;
				while (RB_TextW(shown, D(15)) > bs.x - gx - D(18) && strlen(shown) > 1) shown = substring(shown, 1, -1);
				RB_Text(bo + eX * gx + eY * D(15), shown, D(15), RB_TEXT, a);
				if (editing && (time * 2 - floor(time * 2)) < 0.5)
					RB_Fill(bo + eX * (gx + D(1) + RB_TextW(shown, D(15))) + eY * D(12), eX * max(1, D(2)) + eY * D(22), RB_EMBER, a);
			}
			break;
		}
		case "keybind":
		{
			RB_TextFit(p + eY * (D(52) - D(15)) / 2, e.rb_t1, D(15), s.x * 0.45, RB_TEXT, a);
			vector bo = p + eX * (s.x * 0.5) + eY * D(4);
			bool wait = rb_keycap_id == e.rb_id;
			RB_Card(bo, eX * (s.x * 0.5) + eY * D(44), D(10), (wait ? '0.3 0.15 0.05' : RB_CARD), a, (wait ? RB_EMBER : RB_BLUE), 0.5 * a);
			RB_TextCenter(bo, eX * (s.x * 0.5) + eY * D(44), (wait ? _("Press a key…") : e.rb_t2), D(14), RB_TEXT, a);
			break;
		}
		case "swatch":
			RB_RRect(p, s, D(10), (e.rb_sel ? RB_EMBER : RB_BLUE), (e.rb_sel ? 1 : 0.3) * a);
			RB_RRect(p + eX * D(3) + eY * D(3), s - eX * D(6) - eY * D(6), D(8), e.rb_col, a);
			break;
		case "tab":
			drawfont = e.rb_sel ? FONT_USER + 3 : FONT_USER;
			RB_TextCenter(p, s, e.rb_t1, D(15), (e.rb_sel ? RB_TEXT : RB_MUTED), a);
			drawfont = FONT_USER;
			if (e.rb_sel) RB_RRect(p + eX * D(10) + eY * (s.y - D(4)), eX * (s.x - D(20)) + eY * D(4), D(2), RB_EMBER, a);
			else if (hot) RB_Fill(p + eY * (s.y - 2), eX * s.x + eY * 2, RB_BLUE, 0.4 * a);
			break;
		case "sidetab":
		{
			if (e.rb_sel) RB_Card(p, s, D(12), RB_CARD_HI, a, RB_EMBER, 0.9 * a);
			else if (hot) RB_RRect(p, s, D(12), RB_CARD, a);
			RB_TextCenter(p + eX * D(6), eX * D(30) + eY * s.y, RB_Glyph(e.rb_icon), D(22), (e.rb_sel ? RB_EMBER : RB_BLUE), a);
			drawfont = e.rb_sel ? FONT_USER + 3 : FONT_USER;
			if (e.rb_t2 != "")
			{
				RB_TextFit(p + eX * D(46) + eY * D(10), e.rb_t1, D(16), s.x - D(76), RB_TEXT, a);
				drawfont = FONT_USER;
				RB_TextFit(p + eX * D(46) + eY * D(33), e.rb_t2, D(12), s.x - D(76), RB_MUTED, a);
				RB_Text(p + eX * (s.x - D(22)) + eY * (s.y - D(22)) / 2, "›", D(22), RB_MUTED, a);
			}
			else
				RB_TextFit(p + eX * D(46) + eY * (s.y - D(16)) / 2, e.rb_t1, D(16), s.x - D(56), (e.rb_sel ? RB_TEXT : RB_TEXT * 0.82), a);
			drawfont = FONT_USER;
			break;
		}
		case "table": RB_DrawTable(e, p); break;
		case "cards": RB_DrawCards(e, p); break;
		case "status":
		{
			RB_TextCenter(p + eY * D(4), eX * s.x + eY * D(44), RB_Glyph(e.rb_icon), D(40), RB_BLUE, a);
			drawfont = FONT_USER + 3;
			RB_TextCenter(p + eY * D(56), eX * s.x + eY * D(28), e.rb_t1, D(22), RB_TEXT, a);
			drawfont = FONT_USER;
			RB_TextCenter(p + eY * D(90), eX * s.x + eY * D(20), e.rb_t2, D(15), RB_MUTED, a);
			vector bo = p + eX * s.x * 0.15 + eY * D(130);
			vector bs = eX * s.x * 0.7 + eY * D(8);
			RB_RRect(bo, bs, D(4), RB_SURFACE, a);
			if (e.rb_sel)
			{
				float ph = rb_reduced ? 0.5 : (time * 0.7) - floor(time * 0.7);
				float x0 = max(0, ph - 0.3), x1 = min(1, ph);
				if (x1 > x0) RB_RRect(bo + eX * bs.x * x0, eX * bs.x * (x1 - x0) + eY * bs.y, D(4), RB_EMBER, a);
			}
			else if (e.rb_min >= 0)
				RB_RRect(bo, eX * bs.x * bound(0, e.rb_min, 1) + eY * bs.y, D(4), RB_EMBER, a);
			break;
		}
		case "progress":
		{
			RB_TextFit(p, e.rb_t1, D(14), s.x, RB_TEXT, a);
			vector bo = p + eY * D(24);
			RB_RRect(bo, eX * s.x + eY * D(8), D(4), RB_SURFACE, a);
			if (!e.rb_sel) RB_RRect(bo, eX * s.x * bound(0, e.rb_min, 1) + eY * D(8), D(4), RB_EMBER, a);
			RB_TextFit(p + eY * D(40), e.rb_t2, D(12), s.x, RB_MUTED, a);
			break;
		}
	}
	if (idx == rb_focusi && e.rb_list >= 0 && lc_total[e.rb_list] > 0)
	{
		// the keyboard cursor on a row of the focused list
		RB_ListCell(e, bound(0, lc_cur[e.rb_list], lc_total[e.rb_list] - 1));
		// four thin bars: RB_Card would paint its outline colour over the whole cell
		vector ro = p + rb_cell_o - '2 2 0', rs = rb_cell_s + '4 4 0';
		float rt = max(2, D(2));
		RB_Fill(ro, eX * rs.x + eY * rt, RB_TEXT, 0.95 * a);
		RB_Fill(ro + eY * (rs.y - rt), eX * rs.x + eY * rt, RB_TEXT, 0.95 * a);
		RB_Fill(ro, eX * rt + eY * rs.y, RB_TEXT, 0.95 * a);
		RB_Fill(ro + eX * (rs.x - rt), eX * rt + eY * rs.y, RB_TEXT, 0.95 * a);
	}
	if (idx == rb_focusi && e.rb_kind != "table" && e.rb_kind != "cards")
	{
		RB_Fill(p - eX * D(3) - eY * D(3), eX * (s.x + D(6)) + eY * D(2), RB_EMBER, 0.9 * a);
		RB_Fill(p - eX * D(3) + eY * (s.y + D(1)), eX * (s.x + D(6)) + eY * D(2), RB_EMBER, 0.9 * a);
		RB_Fill(p - eX * D(3) - eY * D(3), eX * D(2) + eY * (s.y + D(6)), RB_EMBER, 0.9 * a);
		RB_Fill(p + eX * (s.x + D(1)) - eY * D(3), eX * D(2) + eY * (s.y + D(6)), RB_EMBER, 0.9 * a);
	}
}

void RB_DrawScrollBar(int r)
{
	if (rg_max[r] <= 0 || rg_s[r].y <= 0) return;
	float view = rg_s[r].y, total = rg_content[r];
	float bh = max(D(28), view * view / total);
	float by = rg_o[r].y + (view - bh) * (rg_scroll[r] / rg_max[r]);
	float a = rb_touch ? bound(0, rg_bar[r], 1) : 0.55;
	if (a <= 0.01) return;
	RB_RRect(eX * (rg_o[r].x + rg_s[r].x - D(8)) + eY * by, eX * D(4) + eY * bh, D(2), RB_BLUE, a * rb_a);
}

void RB_DrawRegion(int r)
{
	if (rg_s[r].x <= 0 || rg_s[r].y <= 0) return;
	drawsetcliparea(rg_o[r].x, rg_o[r].y, rg_s[r].x, rg_s[r].y);
	for (int i = 0; i < rb_n; ++i)
		if (rb_item[i].rb_region == r)
			RB_DrawItem(rb_item[i], i);
	if (r == 0)
	{
		// sticky section headers: held at the top while their section scrolls under
		// them, pushed up by the end of the section
		float top = rg_o[0].y;
		for (int i = 0; i < rb_n; ++i)
		{
			entity e = rb_item[i];
			if (e.rb_region != 0 || e.rb_kind != "sechead" || !e.rb_sel) continue;
			vector p = RB_ItemPos(e);
			float end = rg_o[0].y + e.rb_y1 - rg_scroll[0];
			if (p.y < top && end > top)
			{
				float y = min(top, end - D(40));
				RB_Fill(eX * rg_o[0].x + eY * y, eX * rg_s[0].x + eY * D(40), RB_BG, 0.97 * rb_a);
				RB_Fill(eX * rg_o[0].x + eY * (y + D(40) - 1), eX * rg_s[0].x + eY * 1, RB_BLUE, 0.25 * rb_a);
				drawfont = FONT_USER + 3;
				RB_Text(eX * p.x + eY * (y + D(13)), strtoupper(e.rb_t1), D(13), RB_BLUE, rb_a);
				drawfont = FONT_USER;
			}
		}
	}
	RB_DrawScrollBar(r);
	drawresetcliparea();
}

void RB_DrawChrome()
{
	RB_Fill(rg_o[1], rg_s[1], RB_BG2, 0.97 * rb_a);
	RB_Fill(rg_o[1] + eY * (rg_s[1].y - 1), eX * rg_s[1].x + eY * 1, RB_BLUE, 0.3 * rb_a);
	if (rg_s[2].y > 0)
	{
		RB_Fill(rg_o[2], rg_s[2], RB_BG2, 0.97 * rb_a);
		RB_Fill(rg_o[2] + eY * (rg_s[2].y - 1), eX * rg_s[2].x + eY * 1, RB_BLUE, 0.2 * rb_a);
	}
	if (rg_s[3].y > 0)
	{
		RB_Fill(rg_o[3], rg_s[3], RB_BG2, 0.97 * rb_a);
		RB_Fill(rg_o[3], eX * rg_s[3].x + eY * 1, RB_BLUE, 0.3 * rb_a);
	}
	if (rg_s[4].x > 0)
	{
		RB_Fill(rg_o[4], rg_s[4], RB_BG2, 0.7 * rb_a);
		RB_Fill(rg_o[4] + eX * (rg_s[4].x - 1), eX * 1 + eY * rg_s[4].y, RB_BLUE, 0.25 * rb_a);
	}
	if (rg_s[5].y > 0)
	{
		RB_Fill(rg_o[5] - eX * D(20), rg_s[5] + eX * D(40), RB_BG, 0.97 * rb_a);
		RB_Fill(rg_o[5] + eY * (rg_s[5].y - 1), eX * rg_s[5].x + eY * 1, RB_BLUE, 0.25 * rb_a);
	}
}

void RB_DrawToasts()
{
	int n = uib_count("app");
	float y = rb_scr.y - D(130);
	for (int i = n - 1; i >= 0; --i)
	{
		string c = uib_child("app", i);
		if (uib_type(c) != "toast") continue;
		string t = uib_get(c, "text");
		float w = RB_TextW(t, D(15)) + D(60);
		vector o = eX * (rb_scr.x - w) / 2 + eY * y;
		string k = uib_get(c, "kind");
		RB_Card(o, eX * w + eY * D(44), D(22), RB_SURFACE, rb_a, ((k == "error") ? RB_DANGER : ((k == "success") ? RB_OK : RB_BLUE)), 0.85 * rb_a);
		RB_Text(o + eX * D(16) + eY * D(13), RB_Glyph(uib_get(c, "icon")), D(17), RB_EMBER, rb_a);
		RB_Text(o + eX * D(42) + eY * D(14), t, D(15), RB_TEXT, rb_a);
		y -= D(52);
	}
}

void RB_DrawOverlay()
{
	if (rb_ov_kind == "") return;
	RB_Fill('0 0 0', rb_scr, '0 0 0', 0.6 * rb_a);
	float mw = min(rb_scr.x - D(32), D(480));
	if (rb_ov_kind == "picker")
	{
		int n = tokenizebyseparator(rb_ov_opts, "\n");
		float mh = min(rb_scr.y - D(48), D(64) + n * D(48));
		vector o = eX * (rb_scr.x - mw) / 2 + eY * (rb_scr.y - mh) / 2;
		RB_Card(o, eX * mw + eY * mh, D(18), RB_SURFACE, rb_a, RB_BLUE, 0.6 * rb_a);
		drawfont = FONT_USER + 3;
		RB_TextFit(o + eX * D(20) + eY * D(18), rb_ov_title, D(18), mw - D(40), RB_TEXT, rb_a);
		drawfont = FONT_USER;
		for (int i = 0; i < n; ++i)
		{
			tokenizebyseparator(rb_ov_opts, "\n");
			string line = argv(i);
			tokenizebyseparator(line, "\t");
			vector ro = o + eX * D(12) + eY * (D(56) + i * D(48));
			if (argv(2) == "1") RB_Card(ro, eX * (mw - D(24)) + eY * D(44), D(10), RB_CARD_HI, rb_a, RB_EMBER, 0.8 * rb_a);
			RB_TextFit(ro + eX * D(14) + eY * D(14), argv(1), D(16), mw - D(52), RB_TEXT, rb_a);
		}
		return;
	}
	int nl = RB_Wrap(rb_ov_body, D(15), mw - D(40));
	float mh = D(140) + nl * D(20);
	vector o = eX * (rb_scr.x - mw) / 2 + eY * (rb_scr.y - mh) / 2;
	RB_Card(o, eX * mw + eY * mh, D(18), RB_SURFACE, rb_a, (rb_ov_destructive ? RB_DANGER : RB_BLUE), 0.7 * rb_a);
	drawfont = FONT_USER + 3;
	RB_TextFit(o + eX * D(20) + eY * D(20), rb_ov_title, D(19), mw - D(40), RB_TEXT, rb_a);
	drawfont = FONT_USER;
	for (int i = 0; i < nl; ++i)
		RB_Text(o + eX * D(20) + eY * (D(54) + i * D(20)), rb_line[i], D(15), RB_MUTED, rb_a);
	vector b1 = o + eX * (mw - D(260)) + eY * (mh - D(64));
	RB_Card(b1, eX * D(116) + eY * D(48), D(12), RB_CARD, rb_a, RB_BLUE, 0.4 * rb_a);
	RB_TextCenter(b1, eX * D(116) + eY * D(48), ((rb_ov_kind == "info") ? _("Close") : _("Cancel")), D(15), RB_TEXT, rb_a);
	if (rb_ov_kind == "confirm")
	{
		vector b2 = b1 + eX * D(128);
		RB_Card(b2, eX * D(116) + eY * D(48), D(12), (rb_ov_destructive ? '0.45 0.1 0.12' : RB_EMBER), rb_a, (rb_ov_destructive ? RB_DANGER : RB_EMBER), rb_a);
		drawfont = FONT_USER + 3;
		RB_TextCenter(b2, eX * D(116) + eY * D(48), rb_ov_ok, D(15), (rb_ov_destructive ? '1 0.8 0.8' : RB_INK), rb_a);
		drawfont = FONT_USER;
	}
}

// ======================================================================== input

bool RB_In(vector m, vector o, vector s)
{
	return m.x >= o.x && m.y >= o.y && m.x < o.x + s.x && m.y < o.y + s.y;
}

int RB_HitItem(vector m)
{
	for (int pass = 0; pass < 7; ++pass)
	{
		int r = 0;
		switch (pass)
		{
			case 0: r = 6; break;
			case 1: r = 1; break;
			case 2: r = 2; break;
			case 3: r = 5; break;
			case 4: r = 3; break;
			case 5: r = 4; break;
			case 6: r = 0; break;
		}
		if (rb_modal != "" && r != 6) return -1;
		if (rg_s[r].x <= 0 || !RB_In(m, rg_o[r], rg_s[r]))
			continue;
		for (int i = rb_n - 1; i >= 0; --i)
		{
			entity e = rb_item[i];
			if (e.rb_region != r) continue;
			string k = e.rb_kind;
			if (!(e.rb_focus || k == "table" || k == "cards" || k == "back" || k == "slider")) continue;
			if (RB_In(m, RB_ItemPos(e), e.rb_s))
				return i;
		}
		return -1;
	}
	return -1;
}

int RB_RegionAt(vector m)
{
	if (rb_modal != "") return 6;
	if (rg_s[4].x > 0 && RB_In(m, rg_o[4], rg_s[4])) return 4;
	if (RB_In(m, rg_o[0], rg_s[0])) return 0;
	if (rg_s[2].y > 0 && RB_In(m, rg_o[2], rg_s[2])) return 2;
	if (rg_s[5].y > 0 && RB_In(m, rg_o[5], rg_s[5])) return 5;
	return -1;
}

void RB_Send(string json) { uib_event(json); }

void RB_SendAction(string node, string action)
{
	RB_Send(sprintf("{\"ev\":\"action\",\"node\":%s,\"action\":%s}", uib_qv(node), uib_qv(action)));
}

void RB_SendValue(string node, string jsonvalue)
{
	RB_Send(sprintf("{\"ev\":\"value\",\"node\":%s,\"value\":%s}", uib_qv(node), jsonvalue));
}

void RB_OpenOverlay(string kind, string title, string body, string ok, float destr, string node, string json)
{
	strcpy(rb_ov_kind, kind);
	strcpy(rb_ov_title, title);
	strcpy(rb_ov_body, body);
	strcpy(rb_ov_ok, ok);
	rb_ov_destructive = destr;
	strcpy(rb_ov_node, node);
	strcpy(rb_ov_json, json);
}

// an action held in a prop of node: confirm first, links, copy/share, file
void RB_Invoke(string node, string prop)
{
	string id = uib_get(node, strcat(prop, ".id"));
	string kind = uib_get(node, strcat(prop, ".kind"));
	if (id == "") return;
	if (kind == "back") { RB_Send("{\"ev\":\"back\"}"); return; }
	if (kind == "link")
	{
		RB_OpenOverlay("info", _("Open this in your browser"), uib_get(node, strcat(prop, ".url")), "", 0, "", "");
		return;
	}
	if (kind == "copy" && cvar("_uib_clipboard_ok"))
	{
		// the engine puts it on the system clipboard (touch_uib_stream.c)
		cvar_set("_uib_clipboard", uib_get(node, strcat(prop, ".text")));
		uib_toast(sprintf(_("Copied %s"), uib_get(node, strcat(prop, ".text"))), "success", "copy", 3);
		RB_SendAction(node, id);
		return;
	}
	if (kind == "copy" || kind == "share")
	{
		RB_OpenOverlay("info", uib_get(node, strcat(prop, ".label")), uib_get(node, strcat(prop, ".text")), "", 0, "", "");
		return;
	}
	if (kind == "file")
	{
		RB_OpenOverlay("info", _("Install from file"), _("This menu cannot open a file picker. Use Install from link, or the app's own file picker."), "", 0, "", "");
		return;
	}
	if (uib_has(node, strcat(prop, ".confirm")))
	{
		RB_OpenOverlay("confirm", uib_get(node, strcat(prop, ".confirm.title")), uib_get(node, strcat(prop, ".confirm.body")),
			uib_get(node, strcat(prop, ".confirm.ok")), uib_get(node, strcat(prop, ".confirm.destructive")) == "1", node, id);
		return;
	}
	RB_SendAction(node, id);
}

void RB_StartEdit(entity e)
{
	strcpy(rb_edit_id, e.rb_id);
	strcpy(rb_edit_text, e.rb_sel ? "" : e.rb_val);
	// a touch-only device has no keys: ask for its screen keyboard, placed so
	// it leaves this field in view (2: shown again even after Back put it away).
	// It used to ask only when something else had already asked, so tapping
	// the welcome's Name field on a phone brought up no keyboard at all.
	if (cvar("vid_touchscreen"))
	{
		vector p = RB_ItemPos(e);
		cvar_set("vid_touchscreen_textinput_x", ftos(p.x));
		cvar_set("vid_touchscreen_textinput_y", ftos(p.y));
		cvar_set("vid_touchscreen_textinput_w", ftos(e.rb_s.x));
		cvar_set("vid_touchscreen_textinput_h", ftos(e.rb_s.y));
		cvar_set("vid_touchscreen_showkeyboard", "2");
	}
}

void RB_EndEdit(bool submit)
{
	if (rb_edit_id == "") return;
	string id = rb_edit_id;
	string jv = uib_qv(rb_edit_text);
	if (submit && uib_type(id) == "field")
		RB_Send(sprintf("{\"ev\":\"submit\",\"node\":%s,\"value\":%s}", uib_qv(id), jv));
	else if (uib_get(id, "private") == "1" && rb_edit_text != "")
		RB_SendValue(id, jv);
	strcpy(rb_edit_id, "");
	strcpy(rb_edit_text, "");
	menu_touch_keyboard_hide();   // the screen keyboard goes with the field
}

// A tap on a row. In a list with selection single or multi one tap chooses the row (kind select)
// and a second tap on it within RB_DOUBLETAP plays or joins it (kind activate);
// in any other list a tap activates.
void RB_SendRowTap(string list, string key)
{
	string kind = "activate";
	string selmode = uib_get(list, "selection");
	if (selmode == "single" || selmode == "multi")
	{
		float now = uib_gettime(1);
		bool dbl = (key == rb_tap_row && list == rb_tap_list && now - rb_tap_t <= RB_DOUBLETAP);
		strcpy(rb_tap_row, dbl ? "" : key);
		strcpy(rb_tap_list, list);
		rb_tap_t = now;
		kind = dbl ? "activate" : "select";
	}
	RB_Send(sprintf("{\"ev\":\"row\",\"node\":%s,\"row\":%s,\"kind\":\"%s\"}", uib_qv(list), uib_qv(key), kind));
}

void RB_TableTap(entity e, vector p, vector m)
{
	int kk = e.rb_list;
	float head = e.rb_size;
	float stick = rg_o[e.rb_region].y;
	bool onhead = head > 0 && ((m.y >= p.y && m.y < p.y + head) || (p.y < stick && m.y >= stick && m.y < stick + head));
	if (onhead)
	{
		int nc = min(16, stof(uib_get(e.rb_id, "columns.#")));
		for (int i = 0; i < nc; ++i)
			if (rt_ws[i] > 0 && m.x >= rt_xs[i] && m.x < rt_xs[i] + rt_ws[i] && uib_get(e.rb_id, sprintf("columns.%d.sortable", i)) == "1")
			{
				string key = uib_get(e.rb_id, sprintf("columns.%d.key", i));
				string cur = (lc_sort[kk] != "") ? lc_sort[kk] : uib_get(e.rb_id, "sort");
				lc_desc[kk] = (cur == key) ? !lc_desc[kk] : (key == "players");
				strcpy(lc_sort[kk], key);
				return;
			}
		return;
	}
	float r = floor((m.y - p.y - head) / lc_rowh[kk]);
	string row = RB_Row(kk, r);
	if (row == "") return;
	lc_cur[kk] = r;
	tokenizebyseparator(row, "\t");
	RB_SendRowTap(e.rb_id, argv(0));
}

void RB_Activate(int idx, vector m)
{
	entity e = rb_item[idx];
	string k = e.rb_kind;
	vector p = RB_ItemPos(e);
	if (rb_edit_id != "" && rb_edit_id != e.rb_id)
		RB_EndEdit(false);
	switch (k)
	{
		case "back": RB_Send("{\"ev\":\"back\"}"); return;
		case "button": case "card": case "hero":
			if (e.rb_val != "" && !e.rb_dis) RB_Invoke(e.rb_id, e.rb_val);
			return;
		case "switch": case "check":
			RB_SendValue(e.rb_id, e.rb_sel ? "false" : "true");
			e.rb_sel = !e.rb_sel;   // optimistic; the engine's diff confirms it
			return;
		case "opt":
			RB_SendValue(e.rb_id, uib_qv(e.rb_val));
			for (int i = 0; i < rb_n; ++i)
				if (rb_item[i].rb_id == e.rb_id && rb_item[i].rb_kind == "opt")
					rb_item[i].rb_sel = rb_item[i] == e;
			return;
		case "tab": case "sidetab":
			RB_SendValue(e.rb_id, uib_qv(e.rb_val));
			rg_scroll[0] = 0;
			return;
		case "stepper":
		{
			float v = stof(e.rb_val);
			float bx = p.x + e.rb_s.x - D(160);
			if (m.x >= bx && m.x < bx + D(52)) v -= e.rb_step;
			else if (m.x >= bx + D(108)) v += e.rb_step;
			else return;
			v = bound(e.rb_min, v, e.rb_max);
			strcpy(e.rb_val, ftos(v));
			RB_SendValue(e.rb_id, ftos(v));
			return;
		}
		case "slider":
		{
			float f = bound(0, (m.x - p.x) / max(1, e.rb_s.x), 1);
			float v = e.rb_min + f * (e.rb_max - e.rb_min);
			if (e.rb_step > 0) v = e.rb_min + rint((v - e.rb_min) / e.rb_step) * e.rb_step;
			strcpy(e.rb_val, ftos(v));
			RB_SendValue(e.rb_id, ftos(v));
			return;
		}
		case "dropdown":
		{
			string opts = "";
			string val = uib_get(e.rb_id, "value");
			int no = stof(uib_get(e.rb_id, "options.#"));
			for (int i = 0; i < no; ++i)
			{
				string v = uib_get(e.rb_id, sprintf("options.%d.value", i));
				opts = strcat(opts, (i ? "\n" : ""), v, "\t", uib_get(e.rb_id, sprintf("options.%d.label", i)), strcat("\t", (v == val) ? "1" : "0"));
			}
			RB_OpenOverlay("picker", e.rb_t1, "", "", 0, e.rb_id, "");
			strcpy(rb_ov_opts, opts);
			return;
		}
		case "field": RB_StartEdit(e); return;
		case "keybind": strcpy(rb_keycap_id, e.rb_id); return;
		case "swatch": RB_SendValue(e.rb_id, uib_qv(e.rb_val)); return;
		case "table": RB_TableTap(e, p, m); return;
		case "cards":
		{
			int kk = e.rb_list;
			if (lc_rows[kk] > 0)
			{
				// rows that scroll sideways: column first
				float hc = floor((m.x - p.x) / (lc_cardw[kk] + D(10)));
				float hr = floor((m.y - p.y) / lc_rowh[kk]);
				if (hr < 0 || hr >= lc_rows[kk] || m.x - p.x - hc * (lc_cardw[kk] + D(10)) > lc_cardw[kk] || m.y - p.y - hr * lc_rowh[kk] > lc_cardh[kk]) return;
				string hrow = RB_Row(kk, hc * lc_rows[kk] + hr);
				if (hrow == "") return;
				lc_cur[kk] = hc * lc_rows[kk] + hr;
				tokenizebyseparator(hrow, "\t");
				RB_SendRowTap(e.rb_id, argv(0));
				return;
			}
			float col = floor((m.x - p.x) / (lc_cardw[kk] + D(12)));
			float r = floor((m.y - p.y) / lc_rowh[kk]);
			if (col < 0 || col >= lc_percol[kk] || m.x - p.x - col * (lc_cardw[kk] + D(12)) > lc_cardw[kk]) return;   // the gap between cards
			string row = RB_Row(kk, r * lc_percol[kk] + col);
			if (row == "") return;
			tokenizebyseparator(row, "\t");
			RB_SendRowTap(e.rb_id, argv(0));
			return;
		}
	}
}

void RB_OverlayTap(vector m)
{
	float mw = min(rb_scr.x - D(32), D(480));
	if (rb_ov_kind == "picker")
	{
		int n = tokenizebyseparator(rb_ov_opts, "\n");
		float mh = min(rb_scr.y - D(48), D(64) + n * D(48));
		vector o = eX * (rb_scr.x - mw) / 2 + eY * (rb_scr.y - mh) / 2;
		float i = floor((m.y - o.y - D(56)) / D(48));
		if (m.x >= o.x && m.x < o.x + mw && i >= 0 && i < n)
		{
			tokenizebyseparator(rb_ov_opts, "\n");
			string line = argv(i);
			tokenizebyseparator(line, "\t");
			RB_SendValue(rb_ov_node, uib_qv(argv(0)));
		}
		strcpy(rb_ov_kind, "");
		return;
	}
	int nl = RB_Wrap(rb_ov_body, D(15), mw - D(40));
	float mh = D(140) + nl * D(20);
	vector o = eX * (rb_scr.x - mw) / 2 + eY * (rb_scr.y - mh) / 2;
	vector b1 = o + eX * (mw - D(260)) + eY * (mh - D(64));
	vector b2 = b1 + eX * D(128);
	vector bs = eX * D(116) + eY * D(48);
	if (rb_ov_kind == "confirm" && RB_In(m, b2, bs))
		RB_SendAction(rb_ov_node, rb_ov_json);
	else if (!RB_In(m, b1, bs) && RB_In(m, o, eX * mw + eY * mh))
		return;  // inside the card, not on a button
	strcpy(rb_ov_kind, "");
}

void RB_Press(vector m)
{
	rb_pressed = 1;
	rb_dragging = 0;
	rb_press_at = m;
	rb_last_move = m;
	rb_last_move_t = time;
	rb_press_region = RB_RegionAt(m);
	rb_press_item = RB_HitItem(m);
	rb_press_hs = (rb_press_item >= 0) ? rb_item[rb_press_item].rb_hs : -1;
	if (cvar("_uib_r_debug") >= 3) print(sprintf("uibr press %v item %d vel %d\n", m, rb_press_item, rg_vel[0]));
	// a press that stops a fling only stops it: it never activates what it lands on
	rb_press_stopper = 0;
	for (int r = 0; r < RB_REGIONS; ++r)
		if (fabs(rg_vel[r]) > D(60)) { rb_press_stopper = 1; rg_vel[r] = 0; }
	for (int i = 0; i < hs_n; ++i)
		if (fabs(hs_vel[i]) > D(60)) { rb_press_stopper = 1; hs_vel[i] = 0; }
	if (rb_press_item >= 0 && rb_item[rb_press_item].rb_kind == "slider" && !rb_touch)
	{
		rb_slider_item = rb_press_item;
		entity e = rb_item[rb_press_item];
		vector p = RB_ItemPos(e);
		rb_local_val = e.rb_min + bound(0, (m.x - p.x) / max(1, e.rb_s.x), 1) * (e.rb_max - e.rb_min);
	}
}

void RB_Drag(vector m)
{
	vector d = m - rb_press_at;
	float slop = D(TOUCH_SHEET_SLOP_DP);
	if (!rb_dragging && (fabs(d.x) > slop || fabs(d.y) > slop))
	{
		rb_dragging = 1;
		rb_drag_axis = (fabs(d.x) > fabs(d.y)) ? 1 : 2;
		// sideways on a slider: the slider follows the finger; otherwise scroll
		if (rb_drag_axis == 1 && rb_press_item >= 0 && rb_item[rb_press_item].rb_kind == "slider")
			rb_slider_item = rb_press_item;
		else if (rb_touch)
			rb_slider_item = -1;
	}
	if (!rb_dragging && rb_slider_item < 0) return;
	vector mv = m - rb_last_move;
	float dt = max(0.001, time - rb_last_move_t);
	if (rb_slider_item >= 0)
	{
		entity e = rb_item[rb_slider_item];
		vector p = RB_ItemPos(e);
		float v = e.rb_min + bound(0, (m.x - p.x) / max(1, e.rb_s.x), 1) * (e.rb_max - e.rb_min);
		if (e.rb_step > 0) v = e.rb_min + rint((v - e.rb_min) / e.rb_step) * e.rb_step;
		rb_local_val = v;
	}
	else if (rb_drag_axis == 1 && rb_press_hs >= 0)
	{
		hs_scroll[rb_press_hs] = bound(0, hs_scroll[rb_press_hs] - mv.x, hs_max[rb_press_hs]);
		hs_vel[rb_press_hs] = -mv.x / dt;
	}
	else if (rb_press_region == 0 || rb_press_region == 4 || rb_press_region == 6)
	{
		int r = rb_press_region;
		rg_scroll[r] = bound(0, rg_scroll[r] - mv.y, rg_max[r]);
		rg_vel[r] = 0.5 * rg_vel[r] + 0.5 * (-mv.y / dt);
		rg_bar[r] = 1.5;
	}
	rb_last_move = m;
	rb_last_move_t = time;
}

void RB_Release(vector m)
{
	if (cvar("_uib_r_debug") >= 3) print(sprintf("uibr release %v dragging %d stopper %d vel %d\n", m, rb_dragging, rb_press_stopper, rg_vel[0]));
	if (!rb_pressed) return;
	rb_pressed = 0;
	if (rb_slider_item >= 0)
	{
		entity e = rb_item[rb_slider_item];
		strcpy(e.rb_val, ftos(rb_local_val));
		RB_SendValue(e.rb_id, ftos(rb_local_val));
		rb_slider_item = -1;
		rb_dragging = 0;
		return;
	}
	if (rb_dragging)
	{
		// a finger that stopped before lifting does not fling
		if (time - rb_last_move_t > 0.08)
		{
			for (int r = 0; r < RB_REGIONS; ++r) rg_vel[r] = 0;
			for (int i = 0; i < hs_n; ++i) hs_vel[i] = 0;
		}
		rb_dragging = 0;
		return;
	}
	if (rb_press_stopper)
		return;
	if (rb_ov_kind != "")
	{
		RB_OverlayTap(m);
		return;
	}
	int i = RB_HitItem(m);
	if (i >= 0 && i == rb_press_item)
	{
		rb_focusi = rb_touch ? -1 : i;
		RB_Activate(i, m);
	}
	else if (i < 0 && rb_modal != "" && !RB_In(m, rg_o[6], rg_s[6]) && uib_get(rb_modal, "dismissable") == "1")
		RB_Send(sprintf("{\"ev\":\"dismiss\",\"node\":%s}", uib_qv(rb_modal)));
	if (rb_edit_id != "" && (i < 0 || rb_item[i].rb_id != rb_edit_id))
		RB_EndEdit(false);
}

void RB_Physics(float dt)
{
	for (int r = 0; r < RB_REGIONS; ++r)
	{
		if (!rb_pressed && fabs(rg_vel[r]) > D(12))
		{
			rg_scroll[r] += rg_vel[r] * dt;
			if (rg_scroll[r] <= 0 || rg_scroll[r] >= rg_max[r]) rg_vel[r] = 0;
			rg_scroll[r] = bound(0, rg_scroll[r], rg_max[r]);
			rg_vel[r] *= exp(-dt * 3.2);
			rg_bar[r] = 1.5;
		}
		else if (!rb_pressed)
			rg_vel[r] = 0;
		rg_bar[r] = max(0, rg_bar[r] - dt * 1.5);
	}
	for (int i = 0; i < hs_n; ++i)
		if (!rb_pressed && fabs(hs_vel[i]) > D(12))
		{
			hs_scroll[i] = bound(0, hs_scroll[i] + hs_vel[i] * dt, hs_max[i]);
			hs_vel[i] *= exp(-dt * 3.2);
		}
}

// The rect of row r of a list item, relative to the item (as the draw code lays it out).
void RB_ListCell(entity e, float r)
{
	int k = e.rb_list;
	if (lc_rows[k] > 0)
	{
		rb_cell_o = eX * floor(r / lc_rows[k]) * (lc_cardw[k] + D(10)) + eY * (r % lc_rows[k]) * lc_rowh[k];
		rb_cell_s = eX * lc_cardw[k] + eY * lc_cardh[k];
	}
	else if (e.rb_kind == "cards")
	{
		rb_cell_o = eX * (r % lc_percol[k]) * (lc_cardw[k] + D(12)) + eY * floor(r / lc_percol[k]) * lc_rowh[k];
		rb_cell_s = eX * lc_cardw[k] + eY * lc_cardh[k];
	}
	else
	{
		rb_cell_o = eY * (e.rb_size + r * lc_rowh[k]);
		rb_cell_s = eX * e.rb_s.x + eY * lc_rowh[k];
	}
}

// Arrow keys between controls: Left and Right stay in the row (a strip of
// cards, a row of buttons), Up and Down go to the nearest control of another
// row, so a strip of ten cards is one step, not ten. Tab walks everything.
int RB_FocusStep(int from, float key)
{
	if (from < 0 || from >= rb_n)
		return RB_NextFocus(-1, (key == K_UPARROW || key == K_LEFTARROW) ? -1 : 1);
	entity c = rb_item[from];
	float dir = (key == K_UPARROW || key == K_LEFTARROW) ? -1 : 1;
	bool across = (key == K_LEFTARROW || key == K_RIGHTARROW);
	for (int i = from + dir; i >= 0 && i < rb_n; i += dir)
	{
		entity e = rb_item[i];
		if (!e.rb_focus || (rb_modal != "" && e.rb_region != 6))
			continue;
		bool samerow = (e.rb_region == c.rb_region && fabs(e.rb_o.y - c.rb_o.y) < D(8));
		if (across)
			return samerow ? i : from;
		if (!samerow)
			return i;
	}
	return from;
}

// Keyboard and gamepad in a focused list: the arrows move a cursor over the rows
// (and choose the row, where the list tells choosing from activating); at the
// list's edge the focus moves on. True when the key was the list's.
bool RB_ListKey(entity e, float key)
{
	int k = e.rb_list;
	float n = lc_total[k];
	if (n <= 0) return false;
	float cur = bound(0, lc_cur[k], n - 1);
	float across = 1, down = 1;   // what one step sideways / down is, in rows
	if (lc_rows[k] > 0) { across = lc_rows[k]; down = 1; }
	else if (e.rb_kind == "cards") { across = 1; down = lc_percol[k]; }
	else across = 0;
	switch (key)
	{
		case K_RIGHTARROW: if (!across) return false; cur += across; break;
		case K_LEFTARROW: if (!across) return false; cur -= across; break;
		case K_DOWNARROW:
			if (lc_rows[k] > 0 && (cur % lc_rows[k]) == lc_rows[k] - 1) return false;
			cur += down;
			break;
		case K_UPARROW:
			if (lc_rows[k] > 0 && (cur % lc_rows[k]) == 0) return false;
			cur -= down;
			break;
		default: return false;
	}
	if (cur < 0 || cur >= n) return false;   // off the list: the focus moves on
	lc_cur[k] = cur;
	// bring the row into view
	RB_ListCell(e, cur);
	if (e.rb_hs >= 0)
	{
		float vx = rb_cell_o.x - hs_scroll[e.rb_hs];
		if (vx < 0) hs_scroll[e.rb_hs] = rb_cell_o.x;
		else if (vx + rb_cell_s.x > lc_vieww[k]) hs_scroll[e.rb_hs] = rb_cell_o.x + rb_cell_s.x - lc_vieww[k];
		hs_scroll[e.rb_hs] = bound(0, hs_scroll[e.rb_hs], hs_max[e.rb_hs]);
	}
	int r = e.rb_region;
	float vy = e.rb_o.y + rb_cell_o.y - rg_scroll[r];
	if (vy < D(40)) rg_scroll[r] = e.rb_o.y + rb_cell_o.y - D(48);
	else if (vy + rb_cell_s.y > rg_s[r].y) rg_scroll[r] = e.rb_o.y + rb_cell_o.y + rb_cell_s.y - rg_s[r].y + D(8);
	rg_scroll[r] = bound(0, rg_scroll[r], rg_max[r]);
	// moving the cursor chooses the row
	string selmode = uib_get(e.rb_id, "selection");
	if (selmode == "single" || selmode == "multi")
	{
		RB_ListFetch(k, max(0, cur - 20), 60);
		string row = RB_Row(k, cur);
		if (row != "")
		{
			tokenizebyseparator(row, "\t");
			RB_Send(sprintf("{\"ev\":\"row\",\"node\":%s,\"row\":%s,\"kind\":\"select\"}", uib_qv(e.rb_id), uib_qv(argv(0))));
		}
	}
	return true;
}

// Enter / A on a focused list: the cursor's row is activated (play the map, join the server).
void RB_ListEnter(entity e)
{
	int k = e.rb_list;
	float cur = bound(0, lc_cur[k], max(0, lc_total[k] - 1));
	RB_ListFetch(k, max(0, cur - 20), 60);
	string row = RB_Row(k, cur);
	if (row == "") return;
	tokenizebyseparator(row, "\t");
	string key = argv(0);
	string selmode = uib_get(e.rb_id, "selection");
	// the menu takes an activate on a row that is not the chosen one as choosing it
	RB_Send(sprintf("{\"ev\":\"row\",\"node\":%s,\"row\":%s,\"kind\":\"%s\"}", uib_qv(e.rb_id), uib_qv(key),
		((selmode == "single" || selmode == "multi") && uib_get(e.rb_id, "selected.0") != key) ? "select" : "activate"));
}

void RB_EnsureVisible(int i)
{
	if (i < 0) return;
	entity e = rb_item[i];
	int r = e.rb_region;
	if (r != 0 && r != 4 && r != 6) return;
	float y = e.rb_o.y - rg_scroll[r];
	if (y < 0) rg_scroll[r] = e.rb_o.y - D(8);
	else if (y + e.rb_s.y > rg_s[r].y) rg_scroll[r] = e.rb_o.y + e.rb_s.y - rg_s[r].y + D(8);
	rg_scroll[r] = bound(0, rg_scroll[r], rg_max[r]);
}

// ======================================================================== frame

// test instrumentation (_uib_r_debug 1): state and cost of the renderer, in _uib_r_state
float rb_dbg_frames, rb_dbg_cost, rb_dbg_worst, rb_dbg_t0, rb_dbg_layouts;
string RB_DebugState()
{
	string k = "";
	for (int i = 0; i < lc_n; ++i) if (lc_anchor[i] != "") { k = lc_anchor[i]; break; }
	vector hdr = rg_o[1];
	// the engine's sprintf takes at most 7 values after the format: three calls
	string a = sprintf("top=%s scroll=%d max=%d vel=%d anchor=%s hdr_y=%d", rb_top, rg_scroll[0], rg_max[0], rg_vel[0], k, hdr.y);
	string b = sprintf(" bar_y=%d bar_h=%d strip_y=%d items=%d layouts=%d frames=%d", rg_o[3].y, rg_s[3].y, rg_o[2].y, rb_n, rb_dbg_layouts, rb_dbg_frames);
	string c = sprintf(" avg_ms=%.3f worst_ms=%.3f pressed=%d", (rb_dbg_frames ? 1000 * rb_dbg_cost / rb_dbg_frames : 0), 1000 * rb_dbg_worst, rb_pressed);
	return strcat(a, b, c);
}

// A stock dialog over the bridge menu. The bridge renderer draws in place of the
// stock menu's root; whatever else the stock menu opens is the stock menu's to
// draw and to take input for: a dialog the bridge has no screen for (Advanced
// settings, Media, the full credits; opened through m_goto, where menu.qc calls
// TouchUIB_Classic), and anything the stock menu puts up by itself out of a
// match (the first-run dialog, the data download, a password prompt from the
// engine). When that dialog closes and the stock root is back, or the menu
// hides, the bridge menu is back, so the old launcher never shows.
float rb_classic;        // 1: a stock dialog is up
float rb_classic_seen;   // ... and the stock menu has shown it
float rb_classic_t;

void TouchUIB_Classic(string itemname)
{
	if (itemname == "" || itemname == "nexposee")
		return;
	if (!UIB_EngineHasBridge() || !cvar("menu_touch_uib") || uib_native())
		return;
	rb_classic = 1;
	rb_classic_seen = 0;
	rb_classic_t = time;
}

bool RB_ClassicUp()
{
	bool root = !main || !main.focusedChild || main.focusedChild == main.mainNexposee;
	// the desktop root is the Nexposee overview: a window zoomed out of it
	// (state 1, 2 or 3; 0 and -1 are the overview) is a dialog like any other
	if (root && main && main.mainNexposee && main.mainNexposee.animationState > 0)
		root = false;
	bool ingame = (gamestatus & (GAME_CONNECTED | GAME_ISSERVER)) != 0;
	if (!rb_classic)
	{
		// out of a match the stock menu opened something by itself
		if (root || ingame || !Menu_Active)
			return false;
		rb_classic = 1;
		rb_classic_seen = 1;
		rb_classic_t = time;
		return true;
	}
	if (!root)
		rb_classic_seen = 1;
	// over: the stock root is back, the menu hid, or the dialog never came up
	if ((root && (rb_classic_seen || time - rb_classic_t > 1)) || (!Menu_Active && rb_classic_seen))
	{
		rb_classic = 0;
		if (!ingame && Menu_Active && main && main.mainNexposee && main.focusedChild != main.mainNexposee)
			m_activate_window(main.mainNexposee);
		return false;
	}
	return true;
}

// tests: uib_input puts the pointer in _uib_r_pointer ("fx fy", fractions of the window)
bool RB_TestPointer()
{
	string tp = cvar_string("_uib_r_pointer");
	if (tp == "")
		return false;
	tokenize_console(tp);
	rb_mouse = eX * stof(argv(0)) * rb_scr.x + eY * stof(argv(1)) * rb_scr.y;
	return true;
}

bool TouchUIB_Draw(float alpha)
{
	if (!UIB_EngineHasBridge())
		return false;
	if (uib_native())
		return true;   // a native renderer shows the menu: describe, do not draw
	if (!cvar("menu_touch_uib"))
	{
		// the classic menu shows its own "Could not join" again
		if (uib_inited && !cvar("touch_joinfail_dialog")) cvar_set("touch_joinfail_dialog", "1");
		return false;
	}
	// m_draw publishes the menu's alpha as _menu_alpha only when it draws the
	// stock menu itself, and the game's HUD fades out by it. After a stock
	// dialog over the bridge menu (the game menu, the join dialog) closed,
	// this renderer has the frames of the fade-out, the cvar stayed at 1 and
	// the HUD (clock, scores, scoreboard, map vote) never came back.
	if (cvar("_menu_alpha") != alpha)
		cvar_set("_menu_alpha", ftos(alpha));
	if (!uib_inited)
		UIB_Init();
	if (time > rb_tick_next)
	{
		rb_tick_next = time + 0.1;
		UIB_Tick();
	}
	if (RB_ClassicUp())
		return false;   // the stock menu draws its dialog
	if (alpha <= 0)
		return true;
	float rb_t_start = uib_gettime(2);  // GETTIME_REALTIME: real time now, not the frame start
	float dt = bound(0, time - rb_prev_t, 0.1);
	rb_prev_t = time;
	rb_a = alpha;
	rb_scr = eX * cvar("vid_conwidth") + eY * cvar("vid_conheight");
	rb_touch = cvar("vid_touchscreen");
	rb_U = 1 / max(0.05, TouchMenuScale_conToDp(1));
	if (rb_touch) rb_U *= TouchMenuScale_size();
	rb_reduced = cvar("menu_uib_reducedmotion");
	rb_imgbudget = 1;
	drawfont = FONT_USER;
	int ns = stof(uib_get("app", "stack.#"));
	int nm = stof(uib_get("app", "modal.#"));
	string top = ns ? uib_get("app", sprintf("stack.%d", ns - 1)) : "";
	string modal = nm ? uib_get("app", sprintf("modal.%d", nm - 1)) : "";
	float rev = uib_rev();
	if (top != rb_top)
	{
		RB_SaveScroll(rb_top);
		strcpy(rb_top, top);
		rg_scroll[0] = RB_LoadScroll(top);
		rg_scroll[4] = rg_scroll[6] = 0;
		for (int r = 0; r < RB_REGIONS; ++r) rg_vel[r] = 0;
		for (int i = 0; i < hs_n; ++i) hs_scroll[i] = 0;
		rb_trans = rb_reduced ? 1 : 0;
		rb_focusi = -1;
		RB_EndEdit(false);
	}
	strcpy(rb_modal, modal);
	// layout only when the description, the screen or the size changed (and not
	// in the middle of a press, so the pressed item stays the same item)
	if ((rev != rb_laid_rev || rb_scr != rb_laid_scr || top != rb_laid_top || rb_n == 0) && !rb_pressed)
	{
		RB_Layout();
		++rb_dbg_layouts;
		rb_laid_rev = rev;
		rb_laid_scr = rb_scr;
		strcpy(rb_laid_top, top);
	}
	RB_Physics(dt);
	rb_trans = min(1, rb_trans + dt / 0.18);
	vector m = getmousepos();
	vector mprev = rb_mouse;
	bool testptr = RB_TestPointer();   // tests (uib_input): the pointer is where the test put it
	if (testptr)
	{
		m = rb_mouse;
		rb_mouse = mprev;
	}
	else if (!cvar("menu_mouse_absolute"))
		m = rb_mouse + m;
	m.x = bound(0, m.x, rb_scr.x);
	m.y = bound(0, m.y, rb_scr.y);
	if (m != rb_mouse)
	{
		rb_mouse = m;
		if (rb_pressed) RB_Drag(m);
	}
	rb_hover = (!rb_pressed && !rb_touch) ? RB_HitItem(rb_mouse) : -1;

	drawresetcliparea();
	RB_Fill('0 0 0', rb_scr, RB_BG, alpha);
	string bg = uib_get(top, "background");
	if (bg != "")
		RB_Image(bg, '0 0 0', rb_scr, 0.2 * alpha, false);
	for (int i = 0; i < 6; ++i)
		RB_Fill(eY * (rb_scr.y * (0.5 + i * 0.085)), eX * rb_scr.x + eY * (rb_scr.y * 0.09), RB_BG, 0.13 * alpha);
	float ta = rb_reduced ? 1 : (rb_trans * rb_trans * (3 - 2 * rb_trans));
	rb_a = alpha * ta;
	RB_DrawRegion(0);
	RB_DrawRegion(4);
	RB_DrawChrome();
	RB_DrawRegion(1);
	RB_DrawRegion(2);
	RB_DrawRegion(5);
	RB_DrawRegion(3);
	rb_a = alpha;
	if (rb_modal != "")
	{
		RB_Fill('0 0 0', rb_scr, '0 0 0', 0.55 * alpha);
		RB_Card(rg_o[6] - eX * D(4) - eY * D(4), rg_s[6] + eX * D(8) + eY * D(8), D(22), RB_SURFACE, alpha, RB_BLUE, 0.6 * alpha);
		RB_DrawRegion(6);
	}
	RB_DrawToasts();
	RB_DrawOverlay();
	if (cvar("_uib_glyphtest"))
	{
		string gs = "‹ < ← → ↑ ↓ ↻ ⟳ ▲ ▼ ▴ ▾ ▶ ● ○ ■ □ ◆ ◈ ◉ ◎ ◐ ★ ☆ ✓ ✕ ⚙ ⌕ ⏻ ⬡ ♥ ♪ ⌨ ☝ ▤ ☰ ◷ ♛ ∿ ⛨ ⇪ ⧉ ⌂ ≋ ⓘ ⚠ ⊘ ⋯ ⇥ ▣ ▭ ✎ ⌫ ⏎ • … ×";
		RB_Fill('0 0 0', rb_scr, '0 0 0', 0.9);
		drawfont = FONT_USER;
		RB_Text('20 40 0', gs, 22, RB_TEXT, 1);
		drawfont = FONT_USER + 3;
		RB_Text('20 100 0', gs, 22, RB_TEXT, 1);
		drawfont = FONT_USER;
	}
	if (!rb_touch && cvar("menu_mouse_absolute") && rb_mouse != '0 0 0')
	{
		RB_RRect(rb_mouse - eX * D(6) - eY * D(6), eX * D(12) + eY * D(12), D(6), RB_EMBER, 0.95 * alpha);
		RB_RRect(rb_mouse - eX * D(3) - eY * D(3), eX * D(6) + eY * D(6), D(3), RB_BG, alpha);
	}
	drawresetcliparea();
	string findid = cvar_string("_uib_r_find");
	if (findid != "")
	{
		// tests (uib_input): where the item of a node is on screen; a row of a
		// list is asked for as <list id>#<row index>
		string found = "";
		int hash = strstrofs(findid, "#", 0);
		string fid = (hash >= 0) ? substring(findid, 0, hash) : findid;
		float frow = (hash >= 0) ? stof(substring(findid, hash + 1, -1)) : -1;
		for (int i = 0; i < rb_n && found == ""; ++i)
		{
			entity fe = rb_item[i];
			if (fe.rb_id != fid) continue;
			if (!fe.rb_focus && fe.rb_list < 0) continue;   // only what can be pressed
			vector fp = RB_ItemPos(fe);
			vector fs = fe.rb_s;
			if (frow >= 0 && fe.rb_list >= 0)
			{
				int fk = fe.rb_list;
				if (lc_rows[fk] > 0)
				{
					fp = fp + eX * floor(frow / lc_rows[fk]) * (lc_cardw[fk] + D(10)) + eY * (frow % lc_rows[fk]) * lc_rowh[fk];
					fs = eX * lc_cardw[fk] + eY * lc_cardh[fk];
				}
				else if (fe.rb_kind == "cards")
				{
					fp = fp + eX * (frow % lc_percol[fk]) * (lc_cardw[fk] + D(12)) + eY * floor(frow / lc_percol[fk]) * lc_rowh[fk];
					fs = eX * lc_cardw[fk] + eY * lc_cardh[fk];
				}
				else
				{
					fp = fp + eY * (fe.rb_size + frow * lc_rowh[fk]);
					fs = eX * fs.x + eY * lc_rowh[fk];
				}
			}
			found = sprintf("%f %f %f %f", fp.x / rb_scr.x, fp.y / rb_scr.y, fs.x / rb_scr.x, fs.y / rb_scr.y);
		}
		if (cvar_string("_uib_r_found") != found) cvar_set("_uib_r_found", found);
	}
	if (cvar("_uib_r_debug"))
	{
		float c = uib_gettime(2) - rb_t_start;
		++rb_dbg_frames;
		rb_dbg_cost += c;
		rb_dbg_worst = max(rb_dbg_worst, c);
		cvar_set("_uib_r_state", RB_DebugState());
		if (cvar("_uib_r_debug") >= 3) print(sprintf("uibr cost %.3f\n", 1000 * c));
		if (cvar("_uib_r_debug") >= 3 || (cvar("_uib_r_debug") >= 2 && time > rb_dbg_t0 + 0.5))
		{
			rb_dbg_t0 = time;
			print("uibr ", RB_DebugState(), "\n");
		}
	}
	return true;
}

int RB_NextFocus(int from, float dir)
{
	if (from < 0 && dir > 0 && rb_modal == "")
		for (int j = 0; j < rb_n; ++j)
			if (rb_item[j].rb_focus && rb_item[j].rb_region == 0)
				return j;
	int i = from;
	for (int k = 0; k < rb_n; ++k)
	{
		i = (from < 0 && k == 0) ? ((dir > 0) ? 0 : rb_n - 1) : i + dir;
		if (i < 0 || i >= rb_n) return from;
		entity e = rb_item[i];
		if (e.rb_focus && (rb_modal == "" || e.rb_region == 6))
			return i;
	}
	return from;
}

bool TouchUIB_KeyDown(float key, float ascii)
{
	if (!UIB_EngineHasBridge() || !cvar("menu_touch_uib") || uib_native() || rb_classic)
		return false;
	if (rb_keycap_id != "")
	{
		if (key != K_ESCAPE)
			RB_SendValue(rb_keycap_id, sprintf("[%s]", uib_qv(keynumtostring(key))));
		strcpy(rb_keycap_id, "");
		return true;
	}
	if (key == K_MOUSE1 || key == K_MOUSE2)
	{
		if (RB_TestPointer()) { }
		else if (cvar("menu_mouse_absolute")) rb_mouse = getmousepos();
		RB_Press(rb_mouse);
		return true;
	}
	if (key == K_MWHEELUP || key == K_MWHEELDOWN)
	{
		int r = RB_RegionAt(rb_mouse);
		if (r == 0 || r == 4 || r == 6)
		{
			rg_scroll[r] = bound(0, rg_scroll[r] + ((key == K_MWHEELUP) ? -D(64) : D(64)), rg_max[r]);
			rg_bar[r] = 1;
		}
		return true;
	}
	if (rb_edit_id != "")
	{
		switch (key)
		{
			case K_ENTER: case K_KP_ENTER: RB_EndEdit(true); return true;
			case K_ESCAPE: RB_EndEdit(false); return true;
			case K_BACKSPACE:
				if (strlen(rb_edit_text)) { string shorter = substring(rb_edit_text, 0, strlen(rb_edit_text) - 1); strcpy(rb_edit_text, shorter); }
				break;
			default:
				if (ascii >= 32 && strlen(rb_edit_text) < 120)
					{ string longer = strcat(rb_edit_text, chr2str(ascii)); strcpy(rb_edit_text, longer); }
				else
					return true;
				break;
		}
		// fields without a submit (search) follow every edit
		if (!uib_has(rb_edit_id, "submit") && uib_get(rb_edit_id, "private") != "1")
			RB_SendValue(rb_edit_id, uib_qv(rb_edit_text));
		return true;
	}
	if (rb_ov_kind != "")
	{
		if (key == K_ESCAPE) strcpy(rb_ov_kind, "");
		else if ((key == K_ENTER || key == K_KP_ENTER) && rb_ov_kind == "confirm") { RB_SendAction(rb_ov_node, rb_ov_json); strcpy(rb_ov_kind, ""); }
		return true;
	}
	switch (key)
	{
		case K_ESCAPE: case K_BACKSPACE: case K_JOY2: case RB_K_PAD_B:
			RB_Send("{\"ev\":\"back\"}");
			return true;
		case K_DOWNARROW: case K_TAB: case K_RIGHTARROW: case RB_K_PAD_DOWN: case RB_K_PAD_RIGHT: case RB_K_STICK_DOWN: case RB_K_STICK_RIGHT:
			if (key == RB_K_PAD_DOWN || key == RB_K_STICK_DOWN) key = K_DOWNARROW;
			if (key == RB_K_PAD_RIGHT || key == RB_K_STICK_RIGHT) key = K_RIGHTARROW;
			if (rb_focusi >= 0 && rb_focusi < rb_n && rb_item[rb_focusi].rb_list >= 0 && RB_ListKey(rb_item[rb_focusi], key))
				return true;
			rb_focusi = (key == K_TAB) ? RB_NextFocus(rb_focusi, 1) : RB_FocusStep(rb_focusi, key);
			RB_EnsureVisible(rb_focusi);
			return true;
		case K_UPARROW: case K_LEFTARROW: case RB_K_PAD_UP: case RB_K_PAD_LEFT: case RB_K_STICK_UP: case RB_K_STICK_LEFT:
			if (key == RB_K_PAD_UP || key == RB_K_STICK_UP) key = K_UPARROW;
			if (key == RB_K_PAD_LEFT || key == RB_K_STICK_LEFT) key = K_LEFTARROW;
			if (rb_focusi >= 0 && rb_focusi < rb_n && rb_item[rb_focusi].rb_list >= 0 && RB_ListKey(rb_item[rb_focusi], key))
				return true;
			rb_focusi = RB_FocusStep(rb_focusi, key);
			RB_EnsureVisible(rb_focusi);
			return true;
		case K_ENTER: case K_KP_ENTER: case K_JOY1: case K_SPACE: case RB_K_PAD_A:
			if (rb_focusi >= 0 && rb_focusi < rb_n)
			{
				entity e = rb_item[rb_focusi];
				if (e.rb_list >= 0) RB_ListEnter(e);
				else RB_Activate(rb_focusi, RB_ItemPos(e) + e.rb_s * 0.5);
			}
			return true;
		case K_PGDN: rg_scroll[0] = min(rg_max[0], rg_scroll[0] + rg_s[0].y * 0.9); return true;
		case K_PGUP: rg_scroll[0] = max(0, rg_scroll[0] - rg_s[0].y * 0.9); return true;
	}
	return true;
}

bool TouchUIB_KeyUp(float key, float ascii)
{
	if (!UIB_EngineHasBridge() || !cvar("menu_touch_uib") || uib_native() || rb_classic)
		return false;
	if (key == K_MOUSE1 || key == K_MOUSE2)
	{
		if (RB_TestPointer()) { }
		else if (cvar("menu_mouse_absolute")) rb_mouse = getmousepos();
		if (rb_pressed && rb_mouse != rb_last_move) RB_Drag(rb_mouse);
		RB_Release(rb_mouse);
	}
	return true;
}
