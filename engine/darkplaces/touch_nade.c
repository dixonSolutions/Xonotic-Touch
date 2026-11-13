/*
Godnotic: the offhand grenade from the touch controls (see touch_nade.h).

With g_nades the server hands the player a grenade on +hook (button6): held,
it is taken out and cooks; let go, it is thrown, harder the longer it cooked
(stock: 400 to 2000 u/s over its 3.5 s fuse), and never before it has been out
a second; held past the fuse it blows in the hand. A thrown one comes back
after g_nades_nade_refire (stock 6 s); bonus grenades come on top.

What the client can read: the mutator bit (touch_hook.c), and the CSQC's
copies of the NADE_TIMER (the fuse spent, 0..1, while one is out) and
NADE_BONUS stats, in its g_stat_* globals (0.8.6 and today's code alike).
The refire is not sent: after a throw the row waits cl_touch_nade_refire
seconds, an estimate; a press while the server has none ready simply cooks
nothing, and the timer says so.

Both HUDs press it the same way: the engine HUD's NADE row through
TouchNade_SetHudButton, the port's CSQC row through +touch_nade /
-touch_nade. The safety (cl_touch_nade_autothrow) lets go once the fuse is
cl_touch_nade_autothrow_at spent, so the throw goes before the blast.
_cl_touch_nade and _cl_touch_nade_timer tell the port's CSQC what to draw.
*/

#include "quakedef.h"
#include "touch_aim.h"
#include "touch_hook.h"
#include "touch_nade.h"

extern cvar_t cl_touch_aim_debug;

cvar_t cl_touch_nade_row = {CF_CLIENT | CF_ARCHIVE, "cl_touch_nade_row", "1",
	"Touch controls: a NADE row under the weapons while the server has offhand grenades: hold to cook, let go to throw"};
cvar_t cl_touch_nade_autothrow = {CF_CLIENT | CF_ARCHIVE, "cl_touch_nade_autothrow", "1",
	"Touch controls: throw a cooking grenade by itself just before it would blow up in your hand"};
cvar_t cl_touch_nade_autothrow_at = {CF_CLIENT | CF_ARCHIVE, "cl_touch_nade_autothrow_at", "0.85",
	"Touch controls: how much of the fuse may burn before the safety throws it (0.5 - 0.95)"};
cvar_t cl_touch_nade_refire = {CF_CLIENT | CF_ARCHIVE, "cl_touch_nade_refire", "6",
	"Touch controls: seconds after a throw before the NADE row offers the next (the server's g_nades_nade_refire is not sent; stock 6)"};
cvar_t cl_touch_nade_state = {CF_CLIENT, "_cl_touch_nade", "0",
	"Set by the engine: the NADE row: 0 none, 1 ready, 2 cooking, 3 waiting for the next"};
cvar_t cl_touch_nade_timer = {CF_CLIENT, "_cl_touch_nade_timer", "0",
	"Set by the engine: the fuse spent of the grenade in hand, 0..1"};

static struct
{
	int timer_ofs, timer_type, bonus_ofs, bonus_type;
	qbool hud_down, cmd_down;
	qbool safety;          // the safety let go: up until the finger lifts
	qbool out;             // button6 this frame
	qbool was_cooking;
	double last_throw;     // when the last one left the hand (or blew)
	double cook_start;
	float timer;
	int bonus;
	int state;
	int throws, safety_throws;
} tn;

static int global_ofs(prvm_prog_t *prog, const char *name, int *type)
{
	mdef_t *d = PRVM_ED_FindGlobal(prog, name);
	if (!d)
		return -1;
	*type = d->type & ~DEF_SAVEGLOBAL;
	if (*type == ev_string || *type == ev_vector || *type == ev_entity || *type == ev_field || *type == ev_function || *type == ev_void)
		return -1;
	return d->ofs;
}

static float global_num(int ofs, int type)
{
	prvm_prog_t *prog = CLVM_prog;
	if (ofs < 0 || !prog->loaded)
		return 0;
	return type == ev_float ? PRVM_GLOBALFIELDVALUE(ofs)->_float : (float)PRVM_GLOBALFIELDVALUE(ofs)->_int;
}

void TouchNade_ProgsChanged(void)
{
	prvm_prog_t *prog = CLVM_prog;
	tn.timer_ofs = tn.bonus_ofs = -1;
	tn.last_throw = 0;
	tn.was_cooking = false;
	if (!prog->loaded)
		return;
	tn.timer_ofs = global_ofs(prog, "g_stat_NADE_TIMER", &tn.timer_type);
	tn.bonus_ofs = global_ofs(prog, "g_stat_NADE_BONUS", &tn.bonus_type);
}

int TouchNade_Buttons(void)
{
	return tn.out ? 32 : 0; // button6, as +hook sends it
}

int TouchNade_State(void) { return tn.state; }
float TouchNade_Timer(void) { return tn.timer; }
int TouchNade_Bonus(void) { return tn.bonus; }

void TouchNade_SetHudButton(qbool down)
{
	tn.hud_down = down;
}

static void TouchNade_Down_f(cmd_state_t *cmd) { tn.cmd_down = true; }
static void TouchNade_Up_f(cmd_state_t *cmd) { tn.cmd_down = false; }

static void publish(void)
{
	char buf[16];
	if (cl_touch_nade_state.integer != tn.state)
	{
		dpsnprintf(buf, sizeof(buf), "%d", tn.state);
		Cvar_SetQuick(&cl_touch_nade_state, buf);
	}
	if (fabs(cl_touch_nade_timer.value - tn.timer) > 0.005f)
	{
		dpsnprintf(buf, sizeof(buf), "%.3f", tn.timer);
		Cvar_SetQuick(&cl_touch_nade_timer, buf);
	}
}

void TouchNade_Frame(void)
{
	double now = host.realtime;
	qbool down = tn.hud_down || tn.cmd_down, cooking, alive;
	float at = bound(0.5f, cl_touch_nade_autothrow_at.value, 0.95f);
	int state;

	alive = cls.state == ca_connected && cls.signon == SIGNONS && cl.stats[STAT_HEALTH] > 0 && !cl.intermission;
	if (!cl_touch_nade_row.integer || !TouchHook_NadesActive() || !alive || TouchAim_BlockReason())
	{
		tn.out = false;
		tn.safety = false;
		if (!alive)
			tn.last_throw = 0; // a new life starts with one (g_nades_onspawn)
		tn.was_cooking = false;
		tn.timer = 0;
		tn.state = TOUCH_NADE_HIDDEN;
		publish();
		return;
	}
	tn.timer = bound(0.0f, global_num(tn.timer_ofs, tn.timer_type), 1.0f);
	tn.bonus = (int)global_num(tn.bonus_ofs, tn.bonus_type);
	cooking = tn.timer > 0;
	if (cooking && !tn.was_cooking)
		tn.cook_start = now;
	if (tn.was_cooking && !cooking)
	{
		// thrown (or blown, or taken by death)
		tn.last_throw = now;
		tn.throws++;
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch nade: thrown after %.2f s of cooking%s\n", now - tn.cook_start, tn.safety ? " (the safety)" : "");
	}
	tn.was_cooking = cooking;

	// the safety: let go before the blast
	if (down && cooking && cl_touch_nade_autothrow.integer && tn.timer >= at && !tn.safety)
	{
		tn.safety = true;
		tn.safety_throws++;
	}
	if (!down)
		tn.safety = false;
	tn.out = down && !tn.safety;

	if (cooking)
		state = TOUCH_NADE_COOKING;
	else if (tn.bonus >= 1 || tn.last_throw <= 0 || now - tn.last_throw >= bound(0.0f, cl_touch_nade_refire.value, 60.0f))
		state = TOUCH_NADE_READY;
	else
		state = TOUCH_NADE_WAITING;
	tn.state = state;
	publish();
}

static void TouchNade_Status_f(cmd_state_t *cmd)
{
	Con_Printf("touch nade: mutator %d, row %d, state %d, fuse %.2f, bonus %d, held %d (hud %d, cmd %d), safety %d; %d thrown, %d by the safety; stats %s\n",
		TouchHook_NadesActive(), cl_touch_nade_row.integer, tn.state, tn.timer, tn.bonus, tn.out, tn.hud_down, tn.cmd_down, tn.safety,
		tn.throws, tn.safety_throws, tn.timer_ofs >= 0 ? "found" : "not found");
}

void TouchNade_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_nade_row);
	Cvar_RegisterVariable(&cl_touch_nade_autothrow);
	Cvar_RegisterVariable(&cl_touch_nade_autothrow_at);
	Cvar_RegisterVariable(&cl_touch_nade_refire);
	Cvar_RegisterVariable(&cl_touch_nade_state);
	Cvar_RegisterVariable(&cl_touch_nade_timer);
	Cmd_AddCommand(CF_CLIENT, "+touch_nade", TouchNade_Down_f, "hold the offhand grenade from the touch controls' NADE row (cooks while held)");
	Cmd_AddCommand(CF_CLIENT, "-touch_nade", TouchNade_Up_f, "let go of the grenade held by +touch_nade (throws it)");
	Cmd_AddCommand(CF_CLIENT, "touch_nade_status", TouchNade_Status_f, "print the touch grenade's state: mutator, row, fuse, bonus, held, safety, throws");
	tn.timer_ofs = tn.bonus_ofs = -1;
}
