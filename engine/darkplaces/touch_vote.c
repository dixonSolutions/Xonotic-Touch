/*
Godnotic: answering votes with a finger (see touch_vote.h).

Two kinds of vote reach a player, and a touch player had no dependable way to
answer either outside the stock screens' key binds:
  - a vote called in the match (vcall): the stock vote panel names "Yes (F1)"
    and "No (F2)". The server answers it through its vote entity (the CSQC's
    vote_active, vote_yescount, vote_nocount, vote_needed, vote_highlighted
    and vote_called_vote) and through prints every client gets, in English,
    whatever CSQC it runs (touch_vote_parse.h).
  - the map / gametype vote at match end: the stock full-screen vote, which
    a finger can already tap (touch_hud.c, the CSQC pointer). Its options and
    counts are in the CSQC's mv_* globals, named differently by Xonotic 0.8.6
    (mv_maps, mv_num_maps) and later builds (mv_entries, mv_count).
Both get the same chip and card here. Nothing is drawn while no vote is open.
*/

#include "quakedef.h"
#include "csprogs.h"
#include "touch_vote.h"
#include "touch_vote_parse.h"
#include "touch_hud.h"
#include "touch_hudmode.h"

extern cvar_t vid_conwidth;
extern cvar_t vid_conheight;

static cvar_t cl_touch_vote_noglobals = {CF_CLIENT, "_cl_touch_vote_noglobals", "0",
	"testing: 1 = follow votes from the server's prints only, as on a server whose CSQC vote state cannot be read"};

// Theme (touch_theme.qh, as touch_hud.c)
static const float C_SURFACE[3]      = {0.07f, 0.08f, 0.10f};
static const float C_INK[3]          = {1, 1, 1};
static const float C_SHADOW[3]       = {0, 0, 0};
static const float C_ACCENT[3]       = {0.208f, 0.518f, 0.894f};
static const float C_ACCENT_LIGHT[3] = {0.506f, 0.816f, 1.0f};
static const float C_DANGER[3]       = {0.902f, 0.176f, 0.259f};
static const float C_SUCCESS[3]      = {0.227f, 0.580f, 0.290f};

// A row's choice: TOUCHVOTE_YES / _NO / _ABSTAIN for a called vote, the
// option index for the map vote.
#define MAX_ROWS 20
#define MAP_NAME_LEN 48
// A vote known only from prints is dropped after this long without an end
// line (a print lost to a map change or a mod's own wording).
#define PRINT_VOTE_MAX_AGE 300.0
// How long the card shows the result before it goes.
#define RESULT_SHOW 2.5

static float cv(const char *name, float def)
{
	cvar_t *var = Cvar_FindVar(&cvars_all, name, ~0);
	return var ? var->value : def;
}
static float ui_w(void) { return max(1.0f, vid_conwidth.value); }
static float ui_h(void) { return max(1.0f, vid_conheight.value); }
static float smin(void) { return min(ui_w(), ui_h()); }

// ---------------------------------------------------------------------------
// The loaded CSQC's globals, bound again whenever another CSQC is loaded
// ---------------------------------------------------------------------------
typedef struct vglobal_s { int ofs; int type; } vglobal_t;

static void *bound_globals;
static vglobal_t g_active, g_yes, g_no, g_needed, g_mine, g_text, g_uid2name;
static vglobal_t g_mv_active, g_mv_count, g_mv_entries, g_mv_data, g_mv_votes, g_mv_own, g_mv_flags, g_mv_timeout, g_mv_gtv, g_mv_winner;

static vglobal_t vglobal(prvm_prog_t *prog, const char *name)
{
	vglobal_t g = {-1, 0};
	mdef_t *d = PRVM_ED_FindGlobal(prog, name);
	if (d)
	{
		g.ofs = d->ofs;
		g.type = d->type & ~DEF_SAVEGLOBAL;
	}
	return g;
}

static vglobal_t vglobal_either(prvm_prog_t *prog, const char *name, const char *other)
{
	vglobal_t g = vglobal(prog, name);
	return g.ofs >= 0 ? g : vglobal(prog, other);
}

// Element i of a global (0 for a plain one): QC arrays are consecutive globals.
static float gfloat(vglobal_t g, int i)
{
	prvm_prog_t *prog = CLVM_prog;
	if (g.ofs < 0 || !prog->loaded || g.ofs + i >= prog->numglobals)
		return 0;
	if (g.type == ev_float)
		return PRVM_GLOBALFIELDFLOAT(g.ofs + i);
	return (float)PRVM_GLOBALFIELDVALUE(g.ofs + i)->_int;
}

static const char *gstring(vglobal_t g, int i)
{
	prvm_prog_t *prog = CLVM_prog;
	if (g.ofs < 0 || g.type != ev_string || !prog->loaded || g.ofs + i >= prog->numglobals)
		return "";
	return PRVM_GetString(prog, PRVM_GLOBALFIELDSTRING(g.ofs + i));
}

static void bind_globals(void)
{
	prvm_prog_t *prog = CLVM_prog;
	bound_globals = prog->loaded ? (void *)prog->globals.fp : NULL;
	g_active.ofs = g_yes.ofs = g_no.ofs = g_needed.ofs = g_mine.ofs = g_text.ofs = g_uid2name.ofs = -1;
	g_mv_active.ofs = g_mv_count.ofs = g_mv_entries.ofs = g_mv_data.ofs = g_mv_votes.ofs = g_mv_own.ofs = -1;
	g_mv_flags.ofs = g_mv_timeout.ofs = g_mv_gtv.ofs = g_mv_winner.ofs = -1;
	if (!prog->loaded)
		return;
	g_active = vglobal(prog, "vote_active");
	g_yes = vglobal(prog, "vote_yescount");
	g_no = vglobal(prog, "vote_nocount");
	g_needed = vglobal(prog, "vote_needed");
	g_mine = vglobal(prog, "vote_highlighted");
	g_text = vglobal(prog, "vote_called_vote");
	g_uid2name = vglobal(prog, "uid2name_dialog");
	g_mv_active = vglobal(prog, "mv_active");
	g_mv_count = vglobal_either(prog, "mv_count", "mv_num_maps");
	g_mv_entries = vglobal_either(prog, "mv_entries", "mv_maps");
	g_mv_data = vglobal(prog, "mv_data");
	g_mv_votes = vglobal(prog, "mv_votes");
	g_mv_own = vglobal(prog, "mv_ownvote");
	g_mv_flags = vglobal(prog, "mv_flags");
	g_mv_timeout = vglobal(prog, "mv_timeout");
	g_mv_gtv = vglobal(prog, "gametypevote");
	g_mv_winner = vglobal(prog, "mv_winner");
	Con_DPrintf("Touch vote: CSQC vote state %s, map vote state %s\n",
		(g_active.ofs >= 0 && g_mine.ofs >= 0) ? "readable" : "not readable, following the server's prints",
		(g_mv_active.ofs >= 0 && g_mv_count.ofs >= 0 && g_mv_entries.ofs >= 0 && g_mv_votes.ofs >= 0) ? "readable" : "not readable");
}

// ---------------------------------------------------------------------------
// What the server's prints said (always kept, whatever the CSQC)
// ---------------------------------------------------------------------------
static struct
{
	qbool active;
	double start;
	char caller[TOUCHVOTE_NAME_LEN];
	char text[TOUCHVOTE_TEXT_LEN];
	int mine;               // the answer the server confirmed, 0 none
	qbool refused;          // "You have already voted."
	int end_kind;           // TOUCHVOTE_MSG_ACCEPTED ...
	double end_time;
	int res_yes, res_no;
	double res_time;
} pr;

static void apply_print(const touchvote_msg_t *m)
{
	double now = host.realtime;
	switch (m->kind)
	{
	case TOUCHVOTE_MSG_CALL:
		pr.active = true;
		pr.start = now;
		dp_strlcpy(pr.caller, m->caller, sizeof(pr.caller));
		dp_strlcpy(pr.text, m->text, sizeof(pr.text));
		pr.mine = 0;
		pr.refused = false;
		pr.end_kind = 0;
		break;
	case TOUCHVOTE_MSG_ACCEPTED:
	case TOUCHVOTE_MSG_REJECTED:
	case TOUCHVOTE_MSG_TIMEOUT:
	case TOUCHVOTE_MSG_STOPPED:
		pr.active = false;
		pr.end_kind = m->kind;
		pr.end_time = now;
		break;
	case TOUCHVOTE_MSG_NOVOTE:
		pr.active = false;
		break;
	case TOUCHVOTE_MSG_RESULTS:
		pr.res_yes = m->yes;
		pr.res_no = m->no;
		pr.res_time = now;
		break;
	case TOUCHVOTE_MSG_MINE:
		pr.mine = m->choice;
		pr.refused = false;
		break;
	case TOUCHVOTE_MSG_ALREADY:
		pr.refused = true;
		break;
	default:
		break;
	}
}

void TouchVote_ServerPrint(const char *msg, size_t len)
{
	char line[512];
	size_t start = 0, i;
	touchvote_msg_t m;
	for (i = 0; i <= len; i++)
	{
		if (i < len && msg[i] && msg[i] != '\n')
			continue;
		if (i > start)
		{
			size_t n = min(i - start, sizeof(line) - 1);
			memcpy(line, msg + start, n);
			line[n] = 0;
			if (TouchVote_ParseLine(line, &m) != TOUCHVOTE_MSG_NONE)
			{
				Con_DPrintf("Touch vote: print event %d (%s / %s)\n", m.kind, m.caller, m.text);
				apply_print(&m);
			}
		}
		start = i + 1;
		if (i < len && !msg[i])
			break;
	}
}

// ---------------------------------------------------------------------------
// The vote as shown this frame
// ---------------------------------------------------------------------------
typedef struct callview_s
{
	qbool active;
	qbool from_globals;     // counts and the confirmed answer are live
	char caller[TOUCHVOTE_NAME_LEN];
	char text[TOUCHVOTE_TEXT_LEN];
	int yes, no, needed;    // -1 unknown
	int mine;
} callview_t;

typedef struct mapview_s
{
	qbool active;
	qbool gtv;              // gametype vote
	qbool decided;          // a winner is shown: votes no longer count
	int n;
	char name[MAX_ROWS][MAP_NAME_LEN];
	int votes[MAX_ROWS];    // -1 when the server hides the counts
	qbool available[MAX_ROWS];
	int own;                // option voted for, -1 none
	float timeleft;         // seconds, -1 unknown
} mapview_t;

static callview_t call;
static mapview_t mapv;
static qbool call_was, map_was;
static unsigned int call_serial;
static qbool call_unseen, map_unseen;
static double pulse_time = -100;
static int sent_choice;             // last answer sent from the card, before the server says
static double sent_time;
static unsigned int sent_serial;
static int map_sent = -1;           // option sent from the card
static int result_kind;             // how the last called vote ended (TOUCHVOTE_MSG_*, -1 unknown)
static char shown_text[TOUCHVOTE_TEXT_LEN];   // the last called vote, kept for its result
static char shown_caller[TOUCHVOTE_NAME_LEN];
static int result_yes = -1, result_no = -1;
static double result_time = -100;

static const char *gametype_name(const char *id)
{
	static const char *names[][2] = {
		{"dm", "Deathmatch"}, {"tdm", "Team Deathmatch"}, {"ctf", "Capture the Flag"},
		{"ca", "Clan Arena"}, {"ft", "Freeze Tag"}, {"kh", "Key Hunt"}, {"lms", "Last Man Standing"},
		{"dom", "Domination"}, {"ons", "Onslaught"}, {"as", "Assault"}, {"rc", "Race"},
		{"cts", "Race CTS"}, {"ka", "Keepaway"}, {"tka", "Team Keepaway"}, {"nb", "Nexball"},
		{"inv", "Invasion"}, {"duel", "Duel"}, {"mayhem", "Mayhem"}, {"tmayhem", "Team Mayhem"},
		{"surv", "Survival"}, {"tst", "Team Survival"},
	};
	size_t i;
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(id, names[i][0]))
			return names[i][1];
	return id;
}

static void read_call(callview_t *v)
{
	double now = host.realtime;
	qbool globals = g_active.ofs >= 0 && g_mine.ofs >= 0 && !cl_touch_vote_noglobals.integer;
	memset(v, 0, sizeof(*v));
	v->yes = v->no = v->needed = -1;
	// The stats-name question (cl_allow_uid2name -1 on a server with player
	// stats) takes over the CSQC's vote state and has its own dialog; a vote
	// called meanwhile is known only from the prints.
	if (globals && g_uid2name.ofs >= 0 && gfloat(g_uid2name, 0))
		globals = false;
	if (globals)
	{
		v->from_globals = true;
		v->active = gfloat(g_active, 0) != 0;
		if (!v->active)
			return;
		TouchVote_StripColors(gstring(g_text, 0), v->text, sizeof(v->text));
		if (!v->text[0])
			dp_strlcpy(v->text, pr.text, sizeof(v->text));
		if (pr.active)
			dp_strlcpy(v->caller, pr.caller, sizeof(v->caller));
		if (g_yes.ofs >= 0)
			v->yes = (int)gfloat(g_yes, 0);
		if (g_no.ofs >= 0)
			v->no = (int)gfloat(g_no, 0);
		if (g_needed.ofs >= 0)
			v->needed = (int)gfloat(g_needed, 0);
		v->mine = (int)gfloat(g_mine, 0);
		return;
	}
	if (pr.active && now - pr.start > PRINT_VOTE_MAX_AGE)
		pr.active = false;
	v->active = pr.active;
	if (!v->active)
		return;
	dp_strlcpy(v->caller, pr.caller, sizeof(v->caller));
	dp_strlcpy(v->text, pr.text, sizeof(v->text));
	v->mine = pr.mine;
}

static void read_map(mapview_t *v)
{
	int i, n;
	memset(v, 0, sizeof(*v));
	v->own = -1;
	v->timeleft = -1;
	if (g_mv_active.ofs < 0 || g_mv_count.ofs < 0 || g_mv_entries.ofs < 0 || g_mv_votes.ofs < 0)
		return;
	if (!gfloat(g_mv_active, 0))
		return;
	n = (int)gfloat(g_mv_count, 0);
	if (n <= 0)
		return;
	v->n = n = min(n, MAX_ROWS);
	v->gtv = g_mv_gtv.ofs >= 0 && gfloat(g_mv_gtv, 0) != 0;
	for (i = 0; i < n; i++)
	{
		const char *s = (v->gtv && g_mv_data.ofs >= 0) ? gstring(g_mv_data, i) : "";
		if (!s[0])
			s = gstring(g_mv_entries, i);
		if (v->gtv)
			s = gametype_name(s);
		TouchVote_StripColors(s, v->name[i], sizeof(v->name[i]));
		if (!v->name[i][0])
			dpsnprintf(v->name[i], sizeof(v->name[i]), "#%d", i + 1);
		v->votes[i] = (int)gfloat(g_mv_votes, i);
		v->available[i] = g_mv_flags.ofs < 0 || ((int)gfloat(g_mv_flags, i) & 1);
	}
	if (g_mv_own.ofs >= 0)
		v->own = (int)gfloat(g_mv_own, 0);
	if (v->own >= n)
		v->own = -1;
	if (g_mv_timeout.ofs >= 0)
		v->timeleft = max(0.0f, gfloat(g_mv_timeout, 0) - (float)cl.time);
	v->decided = g_mv_winner.ofs >= 0 && gfloat(g_mv_winner, 0) > 0;
	v->active = true;
}

// The answer the card marks: the one just sent until the server confirms
// one (it may refuse a change), or for good on a server that never says.
static int shown_choice(void)
{
	int mine = call.mine;
	if (sent_choice && sent_serial == call_serial && (!mine || (mine != sent_choice && host.realtime - sent_time < 2.0)))
		mine = sent_choice;
	return mine;
}

static int shown_map_pick(void)
{
	if (map_sent >= 0 && (mapv.own < 0 || mapv.own != map_sent))
		return mapv.own >= 0 && host.realtime - sent_time >= 2.0 ? mapv.own : map_sent;
	return mapv.own;
}

static qbool call_pending(void) { return call.active && !shown_choice(); }
// The next-map vote is the game's own full screen, tapped directly (the CSQC
// pointer): no chip or card over it unless asked for (touch_vote_mapcard 1).
static qbool map_pending(void) { return mapv.active && !mapv.decided && shown_map_pick() < 0 && cv("touch_vote_mapcard", 0); }

// ---------------------------------------------------------------------------
// Card state
// ---------------------------------------------------------------------------
enum { CARD_CALL, CARD_MAP };
typedef struct rect_s { float x, y, w, h; } rect_t;

static struct
{
	qbool open;
	int kind;
	double close_at;        // the result has been shown long enough
	rect_t box;
	float close_cx, close_cy, close_r;
	float header_bottom;    // the header band's bottom: the x's touch area stops there
	int nrows;
	rect_t row[MAX_ROWS];
	int choice[MAX_ROWS];
	qbool enabled[MAX_ROWS];
	qbool drawn;            // geometry above is what is on screen
} card;

static struct { float cx, cy, r; qbool drawn; } chip;

static void card_open(void)
{
	card.open = true;
	card.close_at = 0;
	card.drawn = false;
	if (call.active || (host.realtime - result_time < RESULT_SHOW && !mapv.active))
	{
		card.kind = CARD_CALL;
		call_unseen = false;
	}
	else
	{
		card.kind = CARD_MAP;
		map_unseen = false;
	}
}

static void card_close(void)
{
	card.open = false;
	card.drawn = false;
}

static qbool in_play(void)
{
	if (!CLVM_prog->loaded)
		return false;
	if (cls.state != ca_connected || cls.signon != SIGNONS)
		return false;
	if (key_dest != key_game || (key_consoleactive & KEY_CONSOLEACTIVE_USER))
		return false;
	return true;
}

static qbool touch_device(void)
{
	return VID_SDL_HasTouchDevices() || vid_touchscreen_detected.integer;
}

static void reset_all(void)
{
	memset(&pr, 0, sizeof(pr));
	call_was = map_was = false;
	call_unseen = map_unseen = false;
	call.active = mapv.active = false;
	sent_choice = 0;
	map_sent = -1;
	result_time = -100;
	card_close();
}

static void update_state(void)
{
	double now = host.realtime;
	static double last_start;
	if (cls.state != ca_connected || cls.signon != SIGNONS)
	{
		if (call_was || map_was || pr.active || card.open)
			reset_all();
		return;
	}
	if (bound_globals != (CLVM_prog->loaded ? (void *)CLVM_prog->globals.fp : NULL))
		bind_globals();

	read_call(&call);
	if (call.active && (!call_was || strcmp(call.text, shown_text) || (pr.active && pr.start != last_start && !call.from_globals)))
	{
		// a new vote: badge, pulse
		call_serial++;
		call_unseen = true;
		pulse_time = now;
		sent_choice = 0;
		result_time = -100;
		shown_caller[0] = 0;
		if (call.caller[0])
			dp_strlcpy(shown_caller, call.caller, sizeof(shown_caller));
		Con_DPrintf("Touch vote: vote open (%s): %s\n", call.from_globals ? "CSQC" : "prints", call.text);
	}
	if (!call.active && call_was)
	{
		// How it ended, if the prints say; the last counts seen otherwise.
		result_kind = (now - pr.end_time < 3.0 && pr.end_kind) ? pr.end_kind : -1;
		if (now - pr.res_time < 3.0)
		{
			result_yes = pr.res_yes;
			result_no = pr.res_no;
		}
		result_time = now;
		if (card.open && card.kind == CARD_CALL)
			card.close_at = now + RESULT_SHOW;
		Con_DPrintf("Touch vote: vote closed (%d)\n", result_kind);
	}
	if (call.active)
	{
		dp_strlcpy(shown_text, call.text, sizeof(shown_text));
		if (call.caller[0])
			dp_strlcpy(shown_caller, call.caller, sizeof(shown_caller));
		result_yes = call.yes;
		result_no = call.no;
	}
	last_start = pr.start;
	call_was = call.active;

	read_map(&mapv);
	if (mapv.active && !map_was)
	{
		map_unseen = true;
		map_sent = -1;
		pulse_time = now;
	}
	if (!mapv.active && map_was && card.open && card.kind == CARD_MAP)
		card_close();
	map_was = mapv.active;

	if (card.open && card.close_at && now >= card.close_at)
		card_close();
	if (card.open && !in_play())
		card_close();
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
// The chip: the last thing on the top row, just past CHAT on the side away
// from MENU (the top-right corner; top-left when left-handed), as wide as
// CHAT is high and centred on its row. The weapon strip keeps below that row
// (WEP_MARGIN_TOP in touch_hud.c), so a long strip never reaches it.
static void chip_place(float *cx, float *cy, float *r)
{
	float s = smin(), scale = max(0.01f, cv("touch_scale", 1.0f));
	qbool flip = cv("touch_handedness", 0) != 0;
	float mx = cv("touch_pause_x", 0.655f), chx_n = cv("touch_chat_x", 0.889f);
	// as touch_hud.c widget_half: CHAT is never under 36 dp tall, the chip never under 48 dp across
	float dpu = TouchDp_Density(cv("vid_touchscreen_ydpi", 0), (float)min(vid.mode.width, vid.mode.height)) * (vid.mode.height > 0 ? ui_h() / (float)vid.mode.height : 1.0f);
	float chy = max(s * cv("touch_chat_size", 0.060f) * scale * 0.5f, TOUCHDP_PILL_HEIGHT * 0.5f * dpu);
	float chx = chy * max(1.0f, cv("touch_chat_aspect", 2.4f));
	float cxp, margin;
	{ float tx, ty, tw, th; if (TouchHUD_TopBarRect(1, &tx, &ty, &tw, &th)) { *r = min(tw, th) * 0.5f; *cx = tx + tw * 0.5f; *cy = ty + th * 0.5f; return; } } // the top bar's slot (touch_topbar.h)
	if (flip)
	{
		mx = 1 - mx;
		chx_n = 1 - chx_n;
	}
	cxp = chx_n * ui_w();
	*r = max(max(0.024f * s, chy), TOUCHDP_MIN_TARGET * 0.5f * dpu);
	*cy = cv("touch_chat_y", 0.088f) * ui_h();
	margin = *r * 0.6f;
	*cx = cxp + (chx_n >= mx ? 1 : -1) * (chx + margin + *r);
	*cx = bound(*r + margin * 0.5f, *cx, ui_w() - *r - margin * 0.5f);
	*cy = bound(*r + margin, *cy, ui_h() - *r - margin);
}

static qbool in_rect(const rect_t *b, float x, float y, float grow)
{
	return x >= b->x - grow && x <= b->x + b->w + grow && y >= b->y - grow && y <= b->y + b->h + grow;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
static float text_width(const char *s, float fs)
{
	float w = DrawQ_TextWidth(s, 0, fs, fs, true, FONT_CHAT);
	return w > 0 ? w : strlen(s) * fs * 0.55f;
}

static void text_at(float x, float y, float fs, const char *s, const float *rgb, float a)
{
	if (s[0] && a > 0)
		DrawQ_String(x, y, s, 0, fs, fs, rgb[0], rgb[1], rgb[2], a, 0, NULL, true, FONT_CHAT);
}

// The text cut to a width, with "..." when cut (never inside a UTF-8 sequence).
static void fit_text(const char *in, float fs, float maxw, char *out, size_t outsize)
{
	size_t len = strlen(in);
	dp_strlcpy(out, in, outsize);
	if (text_width(out, fs) <= maxw)
		return;
	while (len > 0)
	{
		len--;
		while (len > 0 && ((unsigned char)in[len] & 0xC0) == 0x80)
			len--;
		dpsnprintf(out, outsize, "%.*s...", (int)len, in);
		if (text_width(out, fs) <= maxw)
			return;
	}
}

static void box(float x, float y, float w, float h, float radius, const float *rgb, float a)
{
	DrawQ_RoundedBox(x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, radius, 0, rgb[0], rgb[1], rgb[2], a, 0);
}

static void box_rim(float x, float y, float w, float h, float radius, float stroke, const float *rgb, float a)
{
	DrawQ_RoundedBox(x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, radius, stroke, rgb[0], rgb[1], rgb[2], a, 0);
}

static void draw_chip(qbool badge, qbool pressed)
{
	float cx = chip.cx, cy = chip.cy, r = chip.r, t = (float)(host.realtime - pulse_time);
	float stroke = max(1.0f, smin() / 360.0f);
	float a = bound(0.3f, cv("touch_opacity", 0.90f), 1.0f);
	// a short pulse when a vote arrives
	if (t >= 0 && t < 1.5f)
		r *= 1 + 0.14f * fabs(sin(t * (float)M_PI * 2)) * (1 - t / 1.5f);
	DrawQ_RoundedBox(cx, cy + r * 0.06f, r * 1.06f, r * 1.06f, r * 1.06f, 0, C_SHADOW[0], C_SHADOW[1], C_SHADOW[2], a * 0.35f, 0);
	// a dark backing under the glass: it sits over the stock score panel
	DrawQ_RoundedBox(cx, cy, r, r, r, 0, C_SURFACE[0], C_SURFACE[1], C_SURFACE[2], 0.75f, 0);
	if (badge || pressed)
		DrawQ_RoundedBox(cx, cy, r, r, r, 0, C_ACCENT[0], C_ACCENT[1], C_ACCENT[2], a * (pressed ? 0.85f : 0.62f), 0);
	else
		DrawQ_RoundedBox(cx, cy, r, r, r, 0, C_SURFACE[0], C_SURFACE[1], C_SURFACE[2], a * 0.62f, 0);
	DrawQ_RoundedBox(cx, cy, r, r, r, stroke * 2, C_SHADOW[0], C_SHADOW[1], C_SHADOW[2], a * 0.5f, 0);
	DrawQ_RoundedBox(cx, cy, r - stroke * 0.5f, r - stroke * 0.5f, r - stroke * 0.5f, stroke, C_INK[0], C_INK[1], C_INK[2], a * 0.75f, 0);
	// the "i": a dot over a bar
	DrawQ_RoundedBox(cx, cy - r * 0.40f, r * 0.13f, r * 0.13f, r * 0.13f, 0, C_INK[0], C_INK[1], C_INK[2], a * 0.95f, 0);
	DrawQ_RoundedBox(cx, cy + r * 0.14f, r * 0.10f, r * 0.34f, r * 0.10f, 0, C_INK[0], C_INK[1], C_INK[2], a * 0.95f, 0);
	if (badge)
	{
		float bx = cx + r * 0.72f, by = cy - r * 0.72f, br = r * 0.30f;
		DrawQ_RoundedBox(bx, by, br + stroke * 1.5f, br + stroke * 1.5f, br + stroke * 1.5f, 0, C_SHADOW[0], C_SHADOW[1], C_SHADOW[2], 0.6f, 0);
		DrawQ_RoundedBox(bx, by, br, br, br, 0, C_DANGER[0], C_DANGER[1], C_DANGER[2], 1, 0);
	}
}

// Which claimed finger is on what (touch targets)
enum { T_NONE, T_CHIP, T_CLOSE, T_CARD, T_OUTSIDE, T_ROW };
#define MAX_CLAIMED 4
static struct { int id; int target; float x0, y0; } claimed[MAX_CLAIMED];

static qbool target_held(int target)
{
	int i;
	for (i = 0; i < MAX_CLAIMED; i++)
		if (claimed[i].id && claimed[i].target == target)
			return true;
	return false;
}

static const char *result_label(const float **rgb)
{
	*rgb = C_INK;
	switch (result_kind)
	{
	case TOUCHVOTE_MSG_ACCEPTED: *rgb = C_SUCCESS; return "Accepted";
	case TOUCHVOTE_MSG_REJECTED: *rgb = C_DANGER; return "Rejected";
	case TOUCHVOTE_MSG_TIMEOUT: return "Timed out";
	case TOUCHVOTE_MSG_STOPPED: return "Stopped";
	default: return "Vote closed";
	}
}

static const char *choice_name(int c)
{
	return c == TOUCHVOTE_YES ? "Yes" : c == TOUCHVOTE_NO ? "No" : c == TOUCHVOTE_ABSTAIN ? "Abstain" : "";
}

static void draw_row(int i, const char *label, int count, qbool chosen, float a)
{
	rect_t *b = &card.row[i];
	float rad = b->h * 0.28f, fs = b->h * 0.42f, pad = b->h * 0.30f;
	float stroke = max(1.0f, smin() / 360.0f);
	qbool held = target_held(T_ROW + i);
	char buf[MAP_NAME_LEN + 8];
	float ra = card.enabled[i] ? a : a * 0.45f;
	box(b->x, b->y, b->w, b->h, rad, held ? C_ACCENT : C_INK, ra * (held ? 0.55f : 0.10f));
	// the player's own pick: a green border (2 px at 960x600), nothing else
	if (chosen)
		box_rim(b->x, b->y, b->w, b->h, rad, max(1.5f, smin() / 300.0f), C_SUCCESS, 1.0f);
	else
		box_rim(b->x, b->y, b->w, b->h, rad, stroke, C_INK, ra * 0.45f);
	if (count >= 0)
	{
		dpsnprintf(buf, sizeof(buf), "%d", count);
		text_at(b->x + b->w - pad - text_width(buf, fs), b->y + (b->h - fs) * 0.5f, fs, buf, C_INK, ra * 0.95f);
	}
	fit_text(label, fs, b->w - pad * 3 - fs * 0.8f - (count >= 0 ? fs * 2 : 0), buf, sizeof(buf));
	text_at(b->x + pad + fs * 0.8f, b->y + (b->h - fs) * 0.5f, fs, buf, C_INK, ra * 0.95f);
}

// The header band: the title at the left and the close "x" at the right,
// tall enough for the x, so the content below (and the first row) starts
// clear of it. The x's touch area is kept inside this band.
static float card_header(float x, float y, float w, float s)
{
	float cr = 0.030f * s, inset = 0.018f * s;
	card.close_r = cr;
	card.close_cx = x + w - inset - cr;
	card.close_cy = y + inset + cr;
	card.header_bottom = y + 2 * (inset + cr);
	return card.header_bottom;
}

static void draw_close(void)
{
	float cr = card.close_r, k = cr * 0.40f, lw = max(1.0f, cr * 0.09f);
	qbool held = target_held(T_CLOSE);
	int d;
	DrawQ_RoundedBox(card.close_cx, card.close_cy, cr, cr, cr, 0, held ? C_ACCENT[0] : C_INK[0], held ? C_ACCENT[1] : C_INK[1], held ? C_ACCENT[2] : C_INK[2], held ? 0.7f : 0.12f, 0);
	// the cross: two diagonals of overlapping dots (DrawQ_RoundedBox is
	// axis-aligned, and GL lines have no width on GLES)
	for (d = -6; d <= 6; d++)
	{
		float o = k * d / 6.0f;
		DrawQ_RoundedBox(card.close_cx + o, card.close_cy + o, lw, lw, lw, 0, 1, 1, 1, 0.9f, 0);
		DrawQ_RoundedBox(card.close_cx + o, card.close_cy - o, lw, lw, lw, 0, 1, 1, 1, 0.9f, 0);
	}
}

static void draw_card(void)
{
	float s = smin(), w, x, y, pad = 0.035f * s, h, cy, head_h;
	float fs_title = 0.042f * s, fs_sub = 0.040f * s, fs_text = 0.052f * s;
	float row_h, row_gap = 0.020f * s, a = 1.0f, stroke = max(1.0f, s / 360.0f);
	char buf[256], line[256];
	const float *rgb;
	int i, n;
	qbool ended = card.kind == CARD_CALL && !call.active;

	w = min(0.86f * s, ui_w() - 2 * pad);
	// anchored under the chip, outer edges in line (the corner it sits in)
	x = chip.cx < ui_w() * 0.5f ? chip.cx - chip.r : chip.cx + chip.r - w;
	x = bound(pad, x, ui_w() - pad - w);
	y = chip.cy + chip.r + 0.025f * s;
	card.nrows = 0;

	// rows first, to know the height
	if (card.kind == CARD_CALL)
	{
		int mine = shown_choice();
		int yes = ended ? result_yes : call.yes, no = ended ? result_no : call.no;
		static const int choices[3] = {TOUCHVOTE_YES, TOUCHVOTE_NO, TOUCHVOTE_ABSTAIN};
		n = 3;
		row_h = 0.115f * s;
		for (i = 0; i < n; i++)
		{
			card.choice[i] = choices[i];
			// a refused change leaves only the answer already given
			card.enabled[i] = !ended && !(pr.refused && mine && mine != choices[i]);
		}
		head_h = card_header(0, 0, w, s);
		h = head_h + pad * 0.3f + fs_sub * 1.3f + fs_text * 1.4f + pad * 0.6f + n * (row_h + row_gap) + pad * 0.5f;
		y = min(y, ui_h() - pad - h);
		card_header(x, y, w, s);
		card.box.x = x; card.box.y = y; card.box.w = w; card.box.h = h;
		box(x, y + stroke * 2, w, h, pad * 0.6f, C_SHADOW, 0.45f);
		box(x, y, w, h, pad * 0.6f, C_SURFACE, 0.90f);
		box_rim(x, y, w, h, pad * 0.6f, stroke, C_INK, 0.35f);
		// the title, or how the vote ended while the card is still up
		rgb = C_ACCENT_LIGHT;
		text_at(x + pad, card.close_cy - fs_title * 0.5f, fs_title, ended ? result_label(&rgb) : "VOTE", rgb, a);
		cy = card.header_bottom + pad * 0.3f;
		if (shown_caller[0])
			dpsnprintf(buf, sizeof(buf), "%s calls a vote for", shown_caller);
		else
			dp_strlcpy(buf, "A vote has been called for", sizeof(buf));
		fit_text(buf, fs_sub, w - 2 * pad, line, sizeof(line));
		text_at(x + pad, cy, fs_sub, line, C_INK, a * 0.65f);
		cy += fs_sub * 1.3f;
		fit_text(shown_text, fs_text, w - 2 * pad, line, sizeof(line));
		text_at(x + pad, cy, fs_text, line, C_INK, a);
		cy += fs_text * 1.4f + pad * 0.6f;
		for (i = 0; i < n; i++)
		{
			rect_t *b = &card.row[i];
			b->x = x + pad; b->y = cy; b->w = w - 2 * pad; b->h = row_h;
			card.nrows = i + 1;
			draw_row(i, choice_name(card.choice[i]), i == 0 ? yes : i == 1 ? no : -1, mine == card.choice[i], a);
			cy += row_h + row_gap;
		}
	}
	else
	{
		// map / gametype vote: options ranked by votes, the player's pick marked
		int order[MAX_ROWS], j, pick = shown_map_pick();
		qbool counts = false;
		n = mapv.n;
		for (i = 0; i < n; i++)
		{
			order[i] = i;
			if (mapv.votes[i] >= 0)
				counts = true;
		}
		for (i = 1; i < n; i++)
			for (j = i; j > 0 && mapv.votes[order[j]] > mapv.votes[order[j - 1]]; j--)
			{
				int t = order[j];
				order[j] = order[j - 1];
				order[j - 1] = t;
			}
		row_gap = 0.012f * s;
		row_h = 0.095f * s;
		head_h = card_header(0, 0, w, s);
		h = head_h + pad * 0.4f + n * (row_h + row_gap) + pad * 0.5f;
		if (h > ui_h() - 2 * pad)
		{
			// shrink the rows to fit the screen
			float fixed = head_h + pad * 0.4f + pad * 0.5f;
			row_h = max(0.05f * s, (ui_h() - 2 * pad - fixed) / n - row_gap);
			h = fixed + n * (row_h + row_gap);
		}
		y = bound(pad, y, ui_h() - pad - h);
		card_header(x, y, w, s);
		card.box.x = x; card.box.y = y; card.box.w = w; card.box.h = h;
		box(x, y + stroke * 2, w, h, pad * 0.6f, C_SHADOW, 0.45f);
		box(x, y, w, h, pad * 0.6f, C_SURFACE, 0.90f);
		box_rim(x, y, w, h, pad * 0.6f, stroke, C_INK, 0.35f);
		cy = card.close_cy - fs_title * 0.5f;
		text_at(x + pad, cy, fs_title, mapv.gtv ? "GAMETYPE VOTE" : "MAP VOTE", C_ACCENT_LIGHT, a);
		if (mapv.timeleft >= 0 && !mapv.decided)
		{
			dpsnprintf(buf, sizeof(buf), "%d s", (int)ceil(mapv.timeleft));
			text_at(card.close_cx - card.close_r - pad * 0.6f - text_width(buf, fs_title), cy, fs_title, buf, C_INK, a * 0.7f);
		}
		cy = card.header_bottom + pad * 0.4f;
		for (i = 0; i < n && i < MAX_ROWS; i++)
		{
			int k = order[i];
			rect_t *b = &card.row[i];
			b->x = x + pad; b->y = cy; b->w = w - 2 * pad; b->h = row_h;
			card.choice[i] = k;
			card.enabled[i] = mapv.available[k] && !mapv.decided;
			card.nrows = i + 1;
			draw_row(i, mapv.name[k], counts ? max(0, mapv.votes[k]) : -1, pick == k, a);
			cy += row_h + row_gap;
		}
	}

	draw_close();
	if (!card.drawn && card.nrows > 0)
		Con_DPrintf("Touch vote: card %.0f %.0f %.0fx%.0f of %.0fx%.0f, header to %.0f, x at %.0f %.0f r %.0f, first row %.0f..%.0f x %.0f..%.0f\n",
			x, y, w, card.box.h, ui_w(), ui_h(), card.header_bottom, card.close_cx, card.close_cy, card.close_r,
			card.row[0].y, card.row[0].y + card.row[0].h, card.row[0].x, card.row[0].x + card.row[0].w);
	card.drawn = true;
}

void TouchVote_Draw(void)
{
	qbool show;
	update_state();
	chip.drawn = false;
	// The end-of-match scoreboard and the next-map vote own the screen: MENU
	// and the console button only (touch_hudmode.h TouchMode_Controls).
	if (!in_play() || !touch_device() || !(TouchMode_Controls(TouchHudMode_Current()->mode, 0) & TOUCHCTL_VOTECHIP))
	{
		card.drawn = false;
		return;
	}
	show = call_pending() || map_pending() || card.open;
	if (!show)
	{
		card.drawn = false;
		return;
	}
	chip_place(&chip.cx, &chip.cy, &chip.r);
	draw_chip((call_unseen && call_pending()) || (map_unseen && map_pending()), card.open || target_held(T_CHIP));
	chip.drawn = true;
	if (card.open)
	{
		if (card.kind == CARD_MAP && !mapv.active)
			card_close();
		else
			draw_card();
	}
	if (!card.open)
		card.drawn = false;
}

// ---------------------------------------------------------------------------
// Fingers
// ---------------------------------------------------------------------------
static int target_at(float px, float py)
{
	int i;
	float dx = px - chip.cx, dy = py - chip.cy, cr;
	if (card.open && card.drawn)
	{
		float cdx = px - card.close_cx, cdy = py - card.close_cy;
		for (i = 0; i < card.nrows; i++)
			if (in_rect(&card.row[i], px, py, card.row[i].h * 0.05f))
				return card.enabled[i] ? T_ROW + i : T_CARD;
		// the x: a little larger than drawn, but never below the header band
		cr = card.close_r * 1.5f;
		if (cdx * cdx + cdy * cdy <= cr * cr && py < card.header_bottom && px <= card.box.x + card.box.w)
			return T_CLOSE;
		if (in_rect(&card.box, px, py, 0))
			return T_CARD;
	}
	cr = chip.r * 1.35f;
	if (chip.drawn && dx * dx + dy * dy <= cr * cr)
		return T_CHIP;
	return card.open && card.drawn ? T_OUTSIDE : T_NONE;
}

static void vote_row(int i)
{
	char cmd[64];
	if (card.kind == CARD_CALL)
	{
		int c = card.choice[i];
		dpsnprintf(cmd, sizeof(cmd), "cmd vote %s\n", c == TOUCHVOTE_YES ? "yes" : c == TOUCHVOTE_NO ? "no" : "abstain");
		sent_choice = c;
		sent_serial = call_serial;
		pr.refused = false;
	}
	else
	{
		// the stock vote screen sends its choice as an impulse (MapVote_WritePlayerVote)
		dpsnprintf(cmd, sizeof(cmd), "impulse %d\n", card.choice[i] + 1);
		map_sent = card.choice[i];
	}
	sent_time = host.realtime;
	Con_DPrintf("Touch vote: %s", cmd);
	Cbuf_AddText(cmd_local, cmd);
}

qbool TouchVote_Finger(int type, int id, float nx, float ny)
{
	float px = nx * ui_w(), py = ny * ui_h();
	int i, slot = -1;
	// The join handle and the match-end MENU (touch_hud.c) come first: this is
	// the one hook vid_sdl.c calls for every finger before the finger table.
	if (TouchHUD_OverlayFinger(type, id, nx, ny))
		return true;
	for (i = 0; i < MAX_CLAIMED; i++)
		if (claimed[i].id == id)
			slot = i;
	if (type == 0)
	{
		int t;
		if (slot >= 0)
			claimed[slot].id = 0; // a stale entry for a reused id
		if (!in_play() || !(chip.drawn || card.drawn))
			return false;
		t = target_at(px, py);
		if (t == T_NONE)
			return false;
		for (i = 0; i < MAX_CLAIMED; i++)
			if (!claimed[i].id)
			{
				claimed[i].id = id;
				claimed[i].target = t;
				claimed[i].x0 = px;
				claimed[i].y0 = py;
				return true;
			}
		return false;
	}
	if (slot < 0)
		return false;
	if (type == 1)
		return true;
	// up: act if it is still on what it went down on
	{
		int t = claimed[slot].target, now_t = target_at(px, py);
		claimed[slot].id = 0;
		if (t == T_OUTSIDE)
			card_close();
		else if (now_t == t)
		{
			if (t == T_CHIP)
			{
				if (card.open)
					card_close();
				else
					card_open();
			}
			else if (t == T_CLOSE)
				card_close();
			else if (t >= T_ROW && t - T_ROW < card.nrows)
				vote_row(t - T_ROW);
		}
	}
	return true;
}

qbool TouchVote_Back(void)
{
	if (!card.open)
		return false;
	card_close();
	return true;
}

void TouchVote_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_vote_noglobals);
}
