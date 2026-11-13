/*
Godnotic: the ledge guard. A thumb on a virtual stick overshoots; on a
floating map one step too far is a death. While the player is on the ground
and not jumping, the move their stick asks for is flown ahead and, when it
runs off into the void, lava or a hurt zone (cl_touch_ledgeguard 2: any drop
deeper than cl_touch_ledgeguard_drop), the outgoing move (cl.cmd) is turned
along the edge, slowed, stopped or turned into a brake instead. The decisions
are touch_ledge.h's; the world is the fall rescue's (TouchHook_World), and a
walk onto one of the map's jump pads or teleporters is never held back.

In the air (a jump pad, a step off a ramp, a jump) the input is judged by
where the arc comes down: only when the player's own input comes down in
the void or a death zone and letting go, turning or braking does not is the
move changed. Never with a keyboard or a mouse (the aim helpers' rules), or
while a dodge, the hook or a fall rescue moves the player. Run from the
hook's frame after auto dodge, so it sees the move the player's own input
put in cl.cmd.
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "touch_aim.h"
#include "touch_hook.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "touch_ledge.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern cvar_t cl_touch_aim_debug;
extern cvar_t cl_forwardspeed;
extern cvar_t cl_sidespeed;
extern kbutton_t in_jump;

cvar_t cl_touch_ledgeguard = {CF_CLIENT | CF_ARCHIVE, "cl_touch_ledgeguard", "1",
	"Touch controls: keep you from walking off a ledge by accident: 0 off, 1 into the void, lava or a hurt zone, "
	"2 also any drop deeper than cl_touch_ledgeguard_drop. Jumping is never held back. Only in touch mode"};
cvar_t cl_touch_ledgeguard_drop = {CF_CLIENT | CF_ARCHIVE, "cl_touch_ledgeguard_drop", "192",
	"Ledge guard (mode 2): deepest drop you may walk off, units (64 - 1024)"};
cvar_t cl_touch_ledgeguard_ahead = {CF_CLIENT | CF_ARCHIVE, "cl_touch_ledgeguard_ahead", "0.3",
	"Ledge guard: seconds of your move judged ahead, on top of the connection's delay (0.15 - 0.8)"};

// Jump pads and teleporters (trigger_push, trigger_teleport) kept per map:
// a walk onto one is never held back, whatever lies under it.
#define MAX_LAUNCH 128
typedef struct launch_s { vec3_t mins, maxs; } launch_t;

static struct
{
	model_t *launch_model;
	launch_t launch[MAX_LAUNCH];
	int num_launch;
	int verdict;               // last frame's
	const char *blocked_said;
	int n_frames, n_steer, n_slow, n_stop, n_brake; // guarded frames, by what was done
	int edge_said;
	double said_time;
} tl;

// Seconds between this client sending a move and the server acting on it:
// the moves the server has not acknowledged yet (touch_dodge.c does the same).
static float latency_estimate(void)
{
	int i;
	float ms = 0;
	if (!cls.servermovesequence)
		return 0;
	for (i = 0; i < CL_MAX_USERCMDS; i++)
		if (cl.movecmd[i].sequence > cls.servermovesequence)
			ms += cl.movecmd[i].msec;
	return bound(0.0f, ms * 0.001f, 0.5f);
}

// The map's jump pad and teleporter boxes, from its entity list: each names
// its brush model ("*N"), whose bounds are the volume.
static void bind_map(void)
{
	const char *data;
	char key[64], classname[64], model[16];
	model_t *wm = cl.worldmodel;
	tl.launch_model = wm;
	tl.num_launch = 0;
	if (!wm || !wm->brush.entities)
		return;
	data = wm->brush.entities;
	while (COM_ParseToken_Simple(&data, false, false, true) && com_token[0] == '{')
	{
		classname[0] = model[0] = 0;
		for (;;)
		{
			if (!COM_ParseToken_Simple(&data, false, false, true))
				return;
			if (com_token[0] == '}')
				break;
			dp_strlcpy(key, com_token, sizeof(key));
			if (!COM_ParseToken_Simple(&data, false, false, true))
				return;
			if (!strcmp(key, "classname"))
				dp_strlcpy(classname, com_token, sizeof(classname));
			else if (!strcmp(key, "model"))
				dp_strlcpy(model, com_token, sizeof(model));
		}
		if ((strcmp(classname, "trigger_push") && strcmp(classname, "trigger_teleport")) || model[0] != '*' || tl.num_launch >= MAX_LAUNCH)
			continue;
		{
			int n = atoi(model + 1);
			model_t *sub;
			if (n <= 0 || n >= wm->brush.numsubmodels || !(sub = wm->brush.submodels[n]))
				continue;
			VectorCopy(sub->normalmins, tl.launch[tl.num_launch].mins);
			VectorCopy(sub->normalmaxs, tl.launch[tl.num_launch].maxs);
			tl.num_launch++;
		}
	}
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch ledge: %d jump pads and teleporters on this map\n", tl.num_launch);
}

// Whether the player's box standing at p overlaps a jump pad or teleporter.
static int in_launch(void *ctx, const float p[3])
{
	int i, k;
	(void)ctx;
	for (i = 0; i < tl.num_launch; i++)
	{
		for (k = 0; k < 3; k++)
			if (p[k] + cl.playerstandmaxs[k] <= tl.launch[i].mins[k] || p[k] + cl.playerstandmins[k] >= tl.launch[i].maxs[k])
				break;
		if (k == 3)
			return 1;
	}
	return 0;
}

void TouchLedge_Frame(void)
{
	const char *blocked = TouchAim_BlockReason();
	float own_f, own_s, own_mag, own_dir[2] = { 0, 0 }, dodge_vel[2];
	vec3_t eye, fwd, right, ang;
	tr_world_t w;
	td_player_t p;
	td_move_t m;
	tl_opts_t o;
	tl_result_t r;

	if (!blocked && !cl_touch_ledgeguard.integer)
		blocked = "cl_touch_ledgeguard is 0";
	if (!blocked && (TouchHook_InUse() || TouchHook_Rescuing()))
		blocked = "hooking or being rescued";
	if (!blocked && TouchDodge_Lateral(dodge_vel))
		blocked = "a dodge is moving the player";
	// a jump off the ground is meant; in the air the guard still watches
	// where the player comes down
	if (!blocked && cl.onground && (in_jump.state & 3))
		blocked = "jump held";
	if (blocked != tl.blocked_said && cl_touch_aim_debug.integer)
		Con_Printf("touch ledge: %s\n", blocked ? blocked : "on");
	tl.blocked_said = blocked;
	tl.verdict = TL_CLEAR;
	if (blocked)
		return;

	own_f = cl.cmd.forwardmove / max(1.0f, cl_forwardspeed.value);
	own_s = cl.cmd.sidemove / max(1.0f, cl_sidespeed.value);
	own_mag = sqrt(own_f * own_f + own_s * own_s);
	VectorSet(ang, 0, cl.viewangles[YAW], 0);
	AngleVectors(ang, fwd, right, NULL);
	if (own_mag > 0.05f)
	{
		own_dir[0] = (fwd[0] * own_f + right[0] * own_s) / own_mag;
		own_dir[1] = (fwd[1] * own_f + right[1] * own_s) / own_mag;
		own_mag = min(own_mag, 1.0f);
	}
	else
		own_mag = 0;

	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	memset(&p, 0, sizeof(p));
	VectorCopy(eye, p.org);
	p.org[2] -= cl.stats[STAT_VIEWHEIGHT] ? cl.stats[STAT_VIEWHEIGHT] : 35;
	VectorCopy(cl.mvelocity[0], p.vel);
	VectorCopy(cl.playerstandmins, p.mins);
	VectorCopy(cl.playerstandmaxs, p.maxs);
	p.onground = cl.onground;
	memset(&m, 0, sizeof(m));
	m.maxspeed = cl.movevars_maxspeed > 0 ? cl.movevars_maxspeed : 360;
	m.accel = cl.movevars_accelerate > 0 ? cl.movevars_accelerate : 15;
	m.airaccel = cl.movevars_airaccelerate > 0 ? cl.movevars_airaccelerate : 2;
	m.friction = cl.movevars_friction > 0 ? cl.movevars_friction : 6;
	m.stopspeed = cl.movevars_stopspeed > 0 ? cl.movevars_stopspeed : 100;
	m.jumpvel = cl.movevars_jumpvelocity > 0 ? cl.movevars_jumpvelocity : 260;
	m.gravity = cl.movevars_gravity > 0 ? cl.movevars_gravity : 800;
	m.fwd[0] = fwd[0];
	m.fwd[1] = fwd[1];
	m.airstrafeaccel = cl.movevars_airstrafeaccelerate;
	m.maxairstrafespeed = cl.movevars_maxairstrafespeed;
	m.maxairspeed = cl.movevars_maxairspeed;
	TouchHook_World(&w);
	if (tl.launch_model != cl.worldmodel)
		bind_map();

	o.mode = bound(0, cl_touch_ledgeguard.integer, 2);
	o.drop = bound(64.0f, cl_touch_ledgeguard_drop.value, 1024.0f);
	o.lookahead = bound(0.15f, cl_touch_ledgeguard_ahead.value, 0.8f) + latency_estimate();
	o.launch = in_launch;
	o.launch_ctx = NULL;
	if (p.onground)
		TouchLedge_Guard(&w, &p, &m, own_dir, own_mag, &o, &r);
	else
		TouchLedge_GuardAir(&w, &p, &m, own_dir, own_mag, &o, &r);
	tl.verdict = r.verdict;
	if (r.verdict == TL_CLEAR)
		return;

	tl.n_frames++;
	switch (r.verdict)
	{
	case TL_STEER: tl.n_steer++; break;
	case TL_SLOW: tl.n_slow++; break;
	case TL_STOP: tl.n_stop++; break;
	default: tl.n_brake++; break;
	}
	if (cl_touch_aim_debug.integer && (host.realtime - tl.said_time > 1 || tl.edge_said != r.edge))
	{
		Con_Printf("touch ledge: %s%s ahead at %.0f %.0f %.0f: %s (move %.2f -> %.2f, %d moves judged, %.0f ms ahead)\n",
			p.onground ? "" : "in the air, ", tl_edge_name[r.edge], r.edge_at[0], r.edge_at[1], r.edge_at[2], tl_verdict_name[r.verdict], own_mag, r.mag, r.tried, o.lookahead * 1000);
		tl.said_time = host.realtime;
		tl.edge_said = r.edge;
	}
	// the move chosen, back in the view's frame
	cl.cmd.forwardmove = (r.dir[0] * fwd[0] + r.dir[1] * fwd[1]) * r.mag * cl_forwardspeed.value;
	cl.cmd.sidemove = (r.dir[0] * right[0] + r.dir[1] * right[1]) * r.mag * cl_sidespeed.value;
}

void TouchLedge_PrintStatus(void)
{
	Con_Printf("touch ledge: %s; guarded frames %d (turned %d, slowed %d, stopped %d, braked %d)\n",
		tl.blocked_said ? tl.blocked_said : (cl_touch_ledgeguard.integer == 2 ? "on, drops too" : "on"),
		tl.n_frames, tl.n_steer, tl.n_slow, tl.n_stop, tl.n_brake);
}

void TouchLedge_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_ledgeguard);
	Cvar_RegisterVariable(&cl_touch_ledgeguard_drop);
	Cvar_RegisterVariable(&cl_touch_ledgeguard_ahead);
}
