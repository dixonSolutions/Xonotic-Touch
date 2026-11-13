/*
Godnotic: auto dodge. When an enemy projectile the player can see is
about to hit, the move steps aside for them; the view, auto-shoot and the aim
pull carry on untouched.

What the client sees of a projectile: the CSQC edict drawn by
Projectile_Draw (FL_PROJECTILE where that is missing), its
csqcprojectile_type (.cnt in 0.8.6 CSQCs), origin and velocity, its gravity
factor, and a team byte the CSQC turns into .colormap -- the owner's team
colours in a team game, the owner's own colours otherwise. No owner id: in a
team game a projectile in the player's team colours is a teammate's and is
left alone; in a free-for-all colours say nothing (an enemy may wear the
player's own), and the player's own shots are told by flying away. Hitscan
shots give no warning, and the 6000 u/s Blaster and HLAC bolts next to none:
neither is dodged (touch_dodge.h's table).

What the client draws is old: the server's projectile is ahead of it by the
interpolation delay and half the round trip, and a move sent now reaches the
server half a round trip later. Each threat is flown forward by the measured
round trip plus the interpolation delay before it is judged, so the escape is
judged in the server's time.

The player's own steering is judged, not obeyed blindly: the damage along
the path they are already taking is what a dodge is measured against. If that
path clears the shot, nothing happens; if it does not, the escape goes into
the outgoing move (cl.cmd, after the player's own input is in it), keeping
their run along the shot's path where that clears it as well, and a jump or a
dodging-mutator burst into the move's button bits (touch_hook.c ORs them in).
The judgement is redone every frame while a threat is in the window. Never
with a keyboard or a mouse aiming (the aim helpers' rules), while hooking or
being rescued, or during a quick turn.

touch_hook_status prints the counts (threats, dodges, stand-downs by reason,
reaction in frames) and why the dodge is off, if it is; cl_touch_autododge_cue
flashes an arrow at the screen's edge the way a dodge goes.
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "csprogs.h"
#include "touch_aim.h"
#include "touch_hook.h"
// touch_dodge.h brings in touch_rescue.h, whose fall-rescue functions this
// file does not use.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "touch_dodge.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern cvar_t cl_touch_aim_debug;
extern cvar_t cl_forwardspeed;
extern cvar_t cl_sidespeed;

cvar_t cl_touch_autododge = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge", "1",
	"Touch controls: step aside from an enemy rocket, grenade, bolt or orb you can see coming, while you keep aiming. "
	"Only in touch mode; a path of your own that already clears the shot is left alone"};
cvar_t cl_touch_autododge_window = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge_window", "1.2",
	"Auto dodge: how far ahead a shot is judged, seconds (0.3 - 1.5), on top of the connection's delay"};
cvar_t cl_touch_autododge_mindamage = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge_mindamage", "15",
	"Auto dodge: only for a shot that would do at least this much damage, and an escape that saves at least half of it (5 - 100)"};
cvar_t cl_touch_autododge_cue = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge_cue", "1",
	"Auto dodge: flash an arrow at the screen's edge the way a dodge goes"};
cvar_t cl_touch_autododge_scope = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge_scope", "0",
	"Auto dodge: where it works: 0 on servers and in local matches, 1 in local matches only"};
cvar_t cl_touch_autododge_rear = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge_rear", "1",
	"Auto dodge: also shots from behind or the side, out of view (never one behind a wall); 0: only shots in view"};

#define FL_PROJECTILE_BIT 32768   // FL_PROJECTILE, BIT(15) in common/constants.qh
#define MUT_DODGING_BIT   0
#define DODGE_BUTTON      8192    // button11: +dodge
#define JUMP_BUTTON       2
#define WINDOW_MIN        0.3f
#define WINDOW_MAX        1.5f
#define LEAD_MAX          0.6f    // the most a threat is flown ahead for the connection's delay
#define LEAD_MARGIN       150.0f  // units of extra berth per second of that delay
#define LEAD_STRICT       0.15f   // past this delay an escape must halve the damage, not just cut it
#define JUDGE_BUDGET      0.003   // seconds: a judgement dearer than this skips the next frame
#define SWITCH_GAIN       5.0f    // a dodge under way changes course only for this much less damage
#define SWITCH_HOLD       0.15    // and not within this long of its last change, unless it would be hit
#define MIN_DODGE         0.25    // a dodge runs at least this long before the player's own path takes over
#define CLEAR_FRAMES      3       // judged frames the player's own path must clear it before it does
#define VIEW_COS          0.5f    // cl_touch_autododge_rear 0: within 60 degrees of the view
#define TRACK_MAX         256
#define CUE_TIME          0.6

static char vabuf[64];

static struct
{
	int type_ofs, type_type;  // csqcprojectile_type, or .cnt
	int flags_ofs, flags_type; // .flags: the CSQC may declare it int or float
	int draw_ofs, draw_func;   // .draw and Projectile_Draw: the surest mark of a projectile
	int teamplay_ofs, teamplay_type; // the CSQC's teamplay global
	qbool type_is_cnt;
	// a dodge under way
	qbool active;
	td_plan_t plan;
	int proj;                 // the edict it is for
	double until, judged, started, switched;
	int clear_frames;          // judged frames in a row the player's own path has cleared it
	int buttons;
	qbool skip_next;          // the last judgement was dear: skip a frame
	// for the debug lines
	const char *said;
	const char *blocked_said;
	// per edict: velocities derived from positions, for projectiles that send
	// none; the frame it was first seen (reaction); counted as a threat
	struct { vec3_t org; double time, first_time; int ent; int first_frame, danger_frame; qbool threat, dodged, no_escape; } track[TRACK_MAX];
	int frame;
	double cost_sum;
	int cost_frames;
	double cost_report;
	// counts for touch_hook_status
	int n_threats, n_dodges, n_own_path, n_no_escape, n_blocked, n_switched;
	int reaction_sum, reaction_n, reaction_max;
	float lead_sum; int lead_n;
	// the cue
	double cue_time;
	float cue_side, cue_fwd;
	qbool cue_jump;
	// the query
	int unesc;
	float unesc_damage, unesc_when;
} td;

void TouchDodge_ProgsChanged(void)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *d;
	td.type_ofs = -1;
	td.flags_ofs = -1;
	td.teamplay_ofs = -1;
	td.active = false;
	if (!prog->loaded)
		return;
	// Every CSQC projectile is drawn by Projectile_Draw (0.8.6 and today's
	// code alike); FL_PROJECTILE is the fallback.
	td.draw_ofs = td.draw_func = -1;
	d = PRVM_ED_FindField(prog, "draw");
	if (d && (d->type & ~DEF_SAVEGLOBAL) == ev_function)
	{
		mfunction_t *f = PRVM_ED_FindFunction(prog, "Projectile_Draw");
		if (f)
		{
			td.draw_ofs = d->ofs;
			td.draw_func = (int)(f - prog->functions);
		}
	}
	d = PRVM_ED_FindField(prog, "flags");
	if (d)
	{
		td.flags_type = d->type & ~DEF_SAVEGLOBAL;
		td.flags_ofs = d->ofs;
	}
	d = PRVM_ED_FindField(prog, "csqcprojectile_type");
	td.type_is_cnt = false;
	if (!d)
	{
		d = PRVM_ED_FindField(prog, "cnt");
		td.type_is_cnt = true;
	}
	if (d)
	{
		td.type_type = d->type & ~DEF_SAVEGLOBAL;
		td.type_ofs = d->ofs;
	}
	// a float, or an int (gmqcc's own numeric types sit past ev_pointer)
	d = PRVM_ED_FindGlobal(prog, "teamplay");
	if (d)
	{
		int t = d->type & ~DEF_SAVEGLOBAL;
		if (t == ev_float || t > ev_pointer)
		{
			td.teamplay_type = t;
			td.teamplay_ofs = d->ofs;
		}
	}
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch dodge: projectiles by %s, type in .%s, teams from %s\n", td.draw_ofs >= 0 ? "Projectile_Draw" : td.flags_ofs >= 0 ? "FL_PROJECTILE" : "nothing",
			td.type_ofs < 0 ? "(none)" : td.type_is_cnt ? "cnt" : "csqcprojectile_type", td.teamplay_ofs >= 0 ? "the teamplay global" : "(unknown)");
}

int TouchDodge_Buttons(void)
{
	return td.buttons;
}

int TouchDodge_Lateral(float vel[2])
{
	float sp = cl.movevars_maxspeed > 0 ? cl.movevars_maxspeed : 360;
	if (!td.active)
	{
		vel[0] = vel[1] = 0;
		return 0;
	}
	vel[0] = td.plan.dir[0] * sp;
	vel[1] = td.plan.dir[1] * sp;
	return 1;
}

int TouchDodge_Unescapable(float *damage, float *seconds)
{
	*damage = td.unesc ? td.unesc_damage : 0;
	*seconds = td.unesc ? td.unesc_when : 0;
	return td.unesc;
}

// 1 team game, 0 free-for-all, -1 unknown.
static int teamplay_mode(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_eval_t *v;
	if (td.teamplay_ofs < 0 || !prog->loaded)
		return -1;
	v = PRVM_GLOBALFIELDVALUE(td.teamplay_ofs);
	if (td.teamplay_type == ev_float)
		return v->_float != 0;
	return v->_int != 0;
}

static int proj_type(prvm_prog_t *prog, prvm_edict_t *ed)
{
	prvm_eval_t *v;
	if (td.type_ofs < 0)
		return -1;
	v = PRVM_EDICTFIELDVALUE(ed, td.type_ofs);
	return td.type_type == ev_float ? (int)v->_float : v->_int;
}

static qbool is_projectile(prvm_prog_t *prog, prvm_edict_t *ed)
{
	prvm_eval_t *v;
	int f;
	if (td.draw_ofs >= 0)
		return PRVM_EDICTFIELDVALUE(ed, td.draw_ofs)->function == td.draw_func;
	if (td.flags_ofs < 0)
		return false;
	v = PRVM_EDICTFIELDVALUE(ed, td.flags_ofs);
	f = td.flags_type == ev_float ? (int)v->_float : v->_int;
	return (f & FL_PROJECTILE_BIT) != 0;
}

static void stop(const char *why)
{
	if (td.active && cl_touch_aim_debug.integer)
		Con_Printf("touch dodge: done (%s) after %.2f s\n", why, host.realtime - td.started);
	td.active = false;
	td.buttons = 0;
}

// The projectile's velocity: the networked one, else from its last position.
// Notes the frame it was first seen.
static void proj_velocity(prvm_prog_t *prog, prvm_edict_t *ed, int n, const vec3_t org, vec3_t vel)
{
	int k = n & (TRACK_MAX - 1);
	VectorCopy(PRVM_clientedictvector(ed, velocity), vel);
	if (td.track[k].ent != n || host.realtime - td.track[k].time > 0.5)
	{
		td.track[k].first_frame = td.frame;
		td.track[k].first_time = host.realtime;
		td.track[k].danger_frame = -1;
		td.track[k].threat = td.track[k].dodged = td.track[k].no_escape = false;
	}
	else if (VectorLength2(vel) < 1 && host.realtime > td.track[k].time)
	{
		VectorSubtract(org, td.track[k].org, vel);
		VectorScale(vel, 1.0f / (float)(host.realtime - td.track[k].time), vel);
	}
	VectorCopy(org, td.track[k].org);
	td.track[k].time = host.realtime;
	td.track[k].ent = n;
}

static qbool visible(const vec3_t eye, const vec3_t org)
{
	trace_t tr = CL_TraceLine(eye, org, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0,
		collision_extendmovelength.value, true, false, NULL, true, false);
	return tr.fraction >= 0.99f;
}

// Seconds between this client sending a move and the server acting on it,
// from the moves the server has not acknowledged yet: about a round trip.
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

// Fly the projectile `lead` seconds on: where the server has it when a move
// sent now arrives.
static void advance(tr_world_t *w, td_proj_t *pr, float lead)
{
	static td_flight_t f;
	int n = (int)(lead / TD_STEP + 0.5f);
	td_proj_t straight;
	if (n <= 0)
		return;
	straight = *pr;
	straight.homing = 0; // where it steers within the lead is not known: straight on
	TouchDodge_Fly(w, &straight, n * TD_STEP, &f);
	if (f.impact_step >= 0 && f.impact_step < n - 1)
	{
		// it has met the world already: its blast (or its bounce) is now
		VectorCopy(f.impact, pr->org);
		if (!pr->bounce && !pr->prox)
			VectorClear(pr->vel);
		return;
	}
	VectorCopy(f.pos[f.steps - 1], pr->org);
	pr->vel[2] -= pr->gravity * f.steps * TD_STEP;
}

// A Crylink volley before its pellets join (stock: 0.1 s after the shot)
// spreads out, then every pellet turns for the point the volley's middle
// reaches, and they pass it together. Until then the danger is that middle
// line, with all the pellets' damage: the pellets of a volley still spreading
// out are replaced by it.
static int crylink_join(td_proj_t *prs, int *ents, const char **names, float *eta, float *dists, int n, const vec3_t center)
{
	int idx[TD_MAX_PROJ], m = 0, i, k;
	vec3_t c = {0, 0, 0}, v = {0, 0, 0}, to;
	float spread = 0;
	td_proj_t mid;
	for (i = 0; i < n; i++)
		if (prs[i].radius == 80 && prs[i].damage <= 10)
			idx[m++] = i;
	if (m < 2)
		return n;
	for (i = 0; i < m; i++)
	{
		VectorAdd(c, prs[idx[i]].org, c);
		VectorAdd(v, prs[idx[i]].vel, v);
	}
	VectorScale(c, 1.0f / m, c);
	VectorScale(v, 1.0f / m, v);
	for (i = 0; i < m; i++)
	{
		vec3_t dp, dv;
		VectorSubtract(prs[idx[i]].org, c, dp);
		VectorSubtract(prs[idx[i]].vel, v, dv);
		if (VectorLength(dp) > 300)
			return n; // not one volley
		spread += DotProduct(dp, dv);
	}
	if (spread <= 0)
		return n; // already joining: each pellet's own line is right
	mid = prs[idx[0]];
	VectorCopy(c, mid.org);
	VectorCopy(v, mid.vel);
	mid.damage = prs[idx[0]].damage * m;
	mid.edge = prs[idx[0]].edge * m;
	mid.size = 8;
	// drop the pellets, keep the rest, add the middle line
	for (i = m - 1; i >= 0; i--)
	{
		int j = idx[i];
		if (i == 0)
		{
			prs[j] = mid;
			names[j] = "crylink volley";
			VectorSubtract(center, c, to);
			dists[j] = VectorLength(to);
			eta[j] = dists[j] / max(1.0f, VectorLength(v));
			continue;
		}
		for (k = j; k < n - 1; k++)
		{
			prs[k] = prs[k + 1]; ents[k] = ents[k + 1]; names[k] = names[k + 1]; eta[k] = eta[k + 1]; dists[k] = dists[k + 1];
		}
		n--;
	}
	return n;
}

static void gate_changed(const char *why)
{
	if (cl_touch_aim_debug.integer && why != td.blocked_said)
		Con_Printf("touch dodge: %s\n", why ? va(vabuf, sizeof(vabuf), "off: %s", why) : "on");
	td.blocked_said = why;
}

void TouchDodge_Frame(void)
{
	prvm_prog_t *prog = CLVM_prog;
	const char *blocked = TouchAim_BlockReason();
	double now = host.realtime, t0 = Sys_DirtyTime();
	float window = bound(WINDOW_MIN, cl_touch_autododge_window.value, WINDOW_MAX);
	float mindmg = bound(5.0f, cl_touch_autododge_mindamage.value, 100.0f);
	float lead, interp;
	vec3_t eye, fwd, right, ang, center;
	float own_f, own_s, own_mag, own_dir[2] = { 0, 0 };
	tr_world_t w;
	td_player_t p;
	td_move_t m;
	td_hazards_t hz;
	td_choice_t best;
	int i, mycolors, traces = 0, team_game;

	td.frame++;
	td.buttons = 0;
	td.unesc = 0;
	if (!blocked && !cl_touch_autododge.integer)
		blocked = "cl_touch_autododge is 0";
	if (!blocked && cl_touch_autododge_scope.integer == 1 && !sv.active)
		blocked = "cl_touch_autododge_scope 1: local matches only";
	if (!blocked && (TouchHook_InUse() || TouchHook_Rescuing()))
		blocked = "hooking, being rescued or turning";
	if (!blocked && (!prog->loaded || td.type_ofs < 0))
		blocked = "no projectiles to read in this CSQC";
	gate_changed(blocked);
	if (blocked)
	{
		if (td.active)
		{
			td.n_blocked++;
			stop(blocked);
		}
		return;
	}
	if (td.skip_next)
	{
		// the last judgement was dear: this frame keeps the course set
		td.skip_next = false;
		if (td.active)
			goto steer;
		return;
	}
	// the player's own move, before any dodge goes into it
	own_f = cl.cmd.forwardmove / max(1.0f, cl_forwardspeed.value);
	own_s = cl.cmd.sidemove / max(1.0f, cl_sidespeed.value);
	own_mag = sqrt(own_f * own_f + own_s * own_s);
	VectorSet(ang, 0, cl.viewangles[YAW], 0);
	AngleVectors(ang, fwd, right, NULL);
	if (own_mag > 0.1f)
	{
		own_dir[0] = (fwd[0] * own_f + right[0] * own_s) / own_mag;
		own_dir[1] = (fwd[1] * own_f + right[1] * own_s) / own_mag;
	}

	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	memset(&p, 0, sizeof(p));
	VectorCopy(eye, p.org);
	p.org[2] -= cl.stats[STAT_VIEWHEIGHT] ? cl.stats[STAT_VIEWHEIGHT] : 35;
	VectorCopy(cl.mvelocity[0], p.vel);
	VectorCopy(cl.playerstandmins, p.mins);
	VectorCopy(cl.playerstandmaxs, p.maxs);
	p.onground = cl.onground;
	VectorAdd(p.mins, p.maxs, center);
	VectorMA(p.org, 0.5f, center, center);
	memset(&m, 0, sizeof(m));
	m.maxspeed = cl.movevars_maxspeed > 0 ? cl.movevars_maxspeed : 360;
	m.accel = cl.movevars_accelerate > 0 ? cl.movevars_accelerate : 15;
	m.airaccel = cl.movevars_airaccelerate > 0 ? cl.movevars_airaccelerate : 2;
	m.friction = cl.movevars_friction > 0 ? cl.movevars_friction : 6;
	m.stopspeed = cl.movevars_stopspeed > 0 ? cl.movevars_stopspeed : 100;
	m.jumpvel = cl.movevars_jumpvelocity > 0 ? cl.movevars_jumpvelocity : 260;
	m.gravity = cl.movevars_gravity > 0 ? cl.movevars_gravity : 800;
	m.dodging = TouchHook_Mutator(MUT_DODGING_BIT);
	m.dodge_speed = 400;
	m.dodge_up = 200;
	m.fwd[0] = fwd[0];
	m.fwd[1] = fwd[1];
	m.airstrafeaccel = cl.movevars_airstrafeaccelerate;
	m.maxairstrafespeed = cl.movevars_maxairstrafespeed;
	m.maxairspeed = cl.movevars_maxairspeed;
	TouchHook_World(&w);
	mycolors = cl.realplayerentity >= 1 && cl.realplayerentity <= cl.maxclients ? cl.scores[cl.realplayerentity - 1].colors & 255 : -1;
	team_game = teamplay_mode();
	// what the server has done that this client has not drawn yet
	interp = (float)bound(0.0, cl.mtime[0] - cl.time, 0.2);
	lead = bound(0.0f, latency_estimate() + interp, LEAD_MAX);
	td.lead_sum += lead;
	td.lead_n++;

	// a dodge under way: on until the shot has passed (looked at again below)
	if (td.active && now >= td.until)
		stop("the shot has passed");

	// the shots coming, judged together (a Crylink's pellets, a Hagar burst,
	// two players' rockets): the soonest TD_MAX_PROJ of them
	memset(&best, 0, sizeof(best));
	{
		td_proj_t prs[TD_MAX_PROJ];
		float eta[TD_MAX_PROJ], dists[TD_MAX_PROJ];
		const char *names[TD_MAX_PROJ];
		int ents[TD_MAX_PROJ], nprs = 0, k;
		hz.n = 0;
		hz.radius = 125 + 32;
		for (i = 1; i < prog->num_edicts; i++)
		{
			prvm_edict_t *ed = PRVM_EDICT_NUM(i);
			const td_kind_t *kind;
			vec3_t org, vel, to;
			float dist, speed;
			td_proj_t pr;
			if (ed->free || !is_projectile(prog, ed))
				continue;
			kind = TouchDodge_Kind(proj_type(prog, ed));
			if (!kind || (td.type_is_cnt && td.draw_ofs < 0 && kind->kind != TD_KIND_ROCKET && kind->kind != TD_KIND_GRENADE
				&& kind->kind != TD_KIND_ELECTRO_BEAM && kind->kind != TD_KIND_HAGAR))
				continue; // .cnt is used by other entities too: with only FL_PROJECTILE to go on, the common ones
			VectorCopy(PRVM_clientedictvector(ed, origin), org);
			proj_velocity(prog, ed, i, org, vel);
			if (cl_touch_aim_debug.integer >= 3)
				Con_Printf("touch dodge: sees %s (edict %d) colours %d (mine %d, team game %d) speed %.0f at %.0f units\n", kind->name, i,
					(int)PRVM_clientedictfloat(ed, colormap) & 255, mycolors, team_game, VectorLength(vel), VectorDistance(org, center));
			// a teammate's (or the player's own) in a team game; in a
			// free-for-all colours say nothing, and the player's own shots
			// fly away from them (below)
			if (team_game != 0 && mycolors >= 0 && ((int)PRVM_clientedictfloat(ed, colormap) & 255) == mycolors)
				continue;
			VectorSubtract(center, org, to);
			dist = VectorLength(to);
			speed = VectorLength(vel);
			if (kind->prox > 0 && speed < 50)
			{
				// an enemy mine lying there: no escape goes near it
				if (hz.n < TD_MAX_HAZARDS && dist < 1500)
					VectorCopy(org, hz.org[hz.n++]);
				continue;
			}
			if (kind->nade)
			{
				// it blows where it is, flying, rolling or lying: near enough
				// to reach, not only coming this way
				if (dist > kind->radius + 100 + speed * (window + lead))
					continue;
			}
			else if (speed < 50 || DotProduct(vel, to) <= 0 || dist > speed * (window + lead + 0.2f))
				continue; // still, flying away, or not here within the window
			if (!visible(eye, org))
				continue; // a wall between: never through one
			if (!cl_touch_autododge_rear.integer)
			{
				vec3_t vf, d;
				AngleVectors(cl.viewangles, vf, NULL, NULL);
				VectorSubtract(org, eye, d);
				VectorNormalize(d);
				if (DotProduct(vf, d) < VIEW_COS)
					continue; // out of view
			}
			memset(&pr, 0, sizeof(pr));
			VectorCopy(org, pr.org);
			VectorCopy(vel, pr.vel);
			pr.gravity = PRVM_clientedictfloat(ed, gravity) * m.gravity;
			pr.damage = kind->damage;
			pr.edge = kind->edge;
			pr.radius = kind->radius;
			pr.size = kind->size;
			pr.world_blast = kind->world_blast;
			pr.bounce = kind->bounce;
			pr.fuse = kind->fuse;
			pr.homing = kind->homing;
			pr.accel = kind->accel;
			pr.max_speed = kind->max_speed;
			pr.prox = kind->prox;
			pr.life = -1;
			if (kind->nade)
			{
				// its fuse: at most TD_NADE_FUSE_SEEN from when it was first seen
				int tk = i & (TRACK_MAX - 1);
				double age = td.track[tk].ent == i ? now - td.track[tk].first_time : 0;
				pr.life = max(0.1f, TD_NADE_FUSE_SEEN - (float)age - lead);
			}
			// the later it is seen, the less sure where it and the player
			// will be: a wider berth
			pr.margin = lead * LEAD_MARGIN;
			advance(&w, &pr, lead);
			// keep the soonest TD_MAX_PROJ: when full, this one replaces the
			// latest if it is sooner
			k = nprs;
			if (nprs == TD_MAX_PROJ)
			{
				int latest = 0, q;
				for (q = 1; q < nprs; q++)
					if (eta[q] > eta[latest])
						latest = q;
				if (dist / speed >= eta[latest])
					continue;
				k = latest;
			}
			else
				nprs++;
			prs[k] = pr;
			eta[k] = dist / speed;
			ents[k] = i;
			names[k] = kind->name;
			dists[k] = dist;
		}
		nprs = crylink_join(prs, ents, names, eta, dists, nprs, center);
		// soonest first
		for (k = 1; k < nprs; k++)
		{
			int q;
			for (q = k; q > 0 && eta[q] < eta[q - 1]; q--)
			{
				td_proj_t tp = prs[q]; float te = eta[q]; int tn = ents[q]; const char *tname = names[q]; float tdist = dists[q];
				prs[q] = prs[q - 1]; eta[q] = eta[q - 1]; ents[q] = ents[q - 1]; names[q] = names[q - 1]; dists[q] = dists[q - 1];
				prs[q - 1] = tp; eta[q - 1] = te; ents[q - 1] = tn; names[q - 1] = tname; dists[q - 1] = tdist;
			}
		}
		if (!nprs)
		{
			if (td.active)
				stop("the shot is gone");
			goto done;
		}
		// judged against the player's own path: a dodge under way is
		// measured against the course it is on
		TouchDodge_ChooseAllHz(&w, prs, nprs, &p, &m, own_dir, window, mindmg * 0.5f, hz.n ? &hz : NULL, &best);
		traces = w.traces;
		td.judged = now;
		if (best.base_damage >= mindmg)
		{
			int tk = ents[0] & (TRACK_MAX - 1);
			if (td.track[tk].ent == ents[0] && !td.track[tk].threat)
			{
				td.track[tk].threat = true;
				td.track[tk].danger_frame = td.frame;
				td.n_threats++;
			}
			if (best.plan == 0 || best.damage >= best.base_damage * 0.5f)
			{
				td.unesc = 1;
				td.unesc_damage = best.plan ? best.damage : best.base_damage;
				td.unesc_when = best.when;
			}
		}
		if (td.active)
		{
			// the player's own path takes over once it has cleared the shot
			// for a few frames, and the dodge has run its minimum
			td.clear_frames = best.base_damage < mindmg * 0.5f ? td.clear_frames + 1 : 0;
			if (td.clear_frames >= CLEAR_FRAMES && now - td.started >= MIN_DODGE)
				stop("the player's own path clears it");
			else if (best.plan > 0 && (best.chosen.dir[0] != td.plan.dir[0] || best.chosen.dir[1] != td.plan.dir[1]))
			{
				// a better course: taken if it does clearly better than the
				// one under way (judged as if carried on)
				td_choice_t cur;
				TouchDodge_ChooseAllHz(&w, prs, nprs, &p, &m, td.plan.dir, window, 0, hz.n ? &hz : NULL, &cur);
				traces = w.traces;
				if (best.damage + SWITCH_GAIN < cur.base_damage && (now - td.switched >= SWITCH_HOLD || cur.base_damage >= mindmg))
				{
					td.plan = best.chosen;
					td.switched = now;
					td.n_switched++;
					if (cl_touch_aim_debug.integer)
						Con_Printf("touch dodge: changes course: %s -> %.0f (was %.0f)\n", best.chosen.name, best.damage, cur.base_damage);
				}
			}
			if (td.active)
				td.until = max(td.until, now + best.when + 0.1);
		}
		else if (best.base_damage >= mindmg)
		{
			const char *name = names[0];
			if (best.plan > 0 && best.base_damage - best.damage >= mindmg * 0.5f && (lead <= LEAD_STRICT || best.damage <= best.base_damage * 0.5f))
			{
				int tk = ents[0] & (TRACK_MAX - 1);
				int reaction = td.track[tk].ent == ents[0] && td.track[tk].danger_frame >= 0 ? td.frame - td.track[tk].danger_frame : -1;
				float ds, df;
				td.active = true;
				td.plan = best.chosen;
				td.proj = ents[0];
				td.started = td.switched = now;
				td.clear_frames = 0;
				if (td.track[tk].ent == ents[0])
					td.track[tk].dodged = true;
				td.until = now + best.when + 0.1;
				td.n_dodges++;
				if (reaction >= 0)
				{
					td.reaction_sum += reaction;
					td.reaction_n++;
					if (reaction > td.reaction_max)
						td.reaction_max = reaction;
				}
				ds = td.plan.dir[0] * right[0] + td.plan.dir[1] * right[1];
				df = td.plan.dir[0] * fwd[0] + td.plan.dir[1] * fwd[1];
				td.cue_time = now;
				td.cue_side = ds;
				td.cue_fwd = df;
				td.cue_jump = td.plan.jump || td.plan.dodge;
				if (cl_touch_aim_debug.integer)
					Con_Printf("touch dodge: %s%s at %.0f units, hits in %.2f s for %.0f (own path, lead %.0f ms); escape %s%s -> %.0f (%d ruled out); seen %d frames ago\n",
						name, nprs > 1 ? va(vabuf, sizeof(vabuf), " and %d more", nprs - 1) : "", dists[0], best.when, best.base_damage, lead * 1000,
						best.chosen.name, best.chosen.dodge ? " with a dodge" : best.chosen.jump ? " with a jump" : "", best.damage, best.rejected, reaction);
			}
			else
			{
				// once per shot
				int tk = ents[0] & (TRACK_MAX - 1);
				if (!td.track[tk].no_escape && !td.track[tk].dodged)
				{
					td.track[tk].no_escape = true;
					td.n_no_escape++;
					if (cl_touch_aim_debug.integer)
						Con_Printf("touch dodge: %s at %.0f units, hits in %.2f s for %.0f (lead %.0f ms); no escape does better: best %s -> %.0f (%d ruled out)\n",
							name, dists[0], best.when, best.base_damage, lead * 1000, best.chosen.name, best.damage, best.rejected);
				}
			}
		}
		else if (best.base_damage > 0 && own_mag > 0.1f && td.said != names[0])
		{
			// a shot the player's own path takes them clear of
			int tk = ents[0] & (TRACK_MAX - 1);
			td.said = names[0];
			if (td.track[tk].ent == ents[0] && !td.track[tk].threat)
				td.n_own_path++;
		}
	}

done:
	if (traces && Sys_DirtyTime() - t0 > JUDGE_BUDGET)
		td.skip_next = true;
steer:
	if (td.active)
	{
		// the move: the escape's direction, in the view's frame, at full speed
		float df, ds;
		VectorSet(ang, 0, cl.viewangles[YAW], 0);
		AngleVectors(ang, fwd, right, NULL);
		df = td.plan.dir[0] * fwd[0] + td.plan.dir[1] * fwd[1];
		ds = td.plan.dir[0] * right[0] + td.plan.dir[1] * right[1];
		cl.cmd.forwardmove = df * cl_forwardspeed.value;
		cl.cmd.sidemove = ds * cl_sidespeed.value;
		if (now - td.started < 0.1)
			td.buttons |= (td.plan.jump ? JUMP_BUTTON : 0) | (td.plan.dodge ? DODGE_BUTTON : 0);
	}

	if (traces)
	{
		td.cost_sum += Sys_DirtyTime() - t0;
		td.cost_frames++;
	}
	if (cl_touch_aim_debug.integer >= 2 && td.cost_frames && now - td.cost_report > 1)
	{
		Con_Printf("touch dodge: cost %.3f ms a judged frame over %d frames\n", td.cost_sum * 1000 / td.cost_frames, td.cost_frames);
		td.cost_sum = 0;
		td.cost_frames = 0;
		td.cost_report = now;
	}
}

// An arrow at the screen's edge the way the dodge went, for a moment.
void TouchDodge_DrawCue(void)
{
	double age = host.realtime - td.cue_time;
	float a, fs, x, y, w = vid_conwidth.value, h = vid_conheight.value;
	const char *s;
	if (!cl_touch_autododge_cue.integer || td.cue_time <= 0 || age < 0 || age > CUE_TIME || !vid_touchscreen.integer)
		return;
	a = (float)(1 - age / CUE_TIME);
	fs = h * 0.12f;
	if (fabs(td.cue_side) >= fabs(td.cue_fwd))
	{
		s = td.cue_side < 0 ? "<<" : ">>";
		x = td.cue_side < 0 ? w * 0.03f : w * 0.97f - DrawQ_TextWidth(s, 0, fs, fs, false, FONT_CENTERPRINT);
		y = h * 0.45f;
	}
	else
	{
		s = td.cue_fwd < 0 ? "vv" : "^^";
		x = w * 0.5f - DrawQ_TextWidth(s, 0, fs, fs, false, FONT_CENTERPRINT) * 0.5f;
		y = td.cue_fwd < 0 ? h * 0.80f : h * 0.12f;
	}
	// a dark copy under it: it shows over a bright wall as over a dark one
	DrawQ_String(x + fs * 0.05f, y + fs * 0.05f, s, 0, fs, fs, 0, 0, 0, a * 0.7f, 0, NULL, false, FONT_CENTERPRINT);
	DrawQ_String(x, y, s, 0, fs, fs, 0.506f, 0.816f, 1.0f, a, 0, NULL, false, FONT_CENTERPRINT);
	if (td.cue_jump)
	{
		DrawQ_String(x + fs * 0.05f, y - fs + fs * 0.05f, "^", 0, fs, fs, 0, 0, 0, a * 0.7f, 0, NULL, false, FONT_CENTERPRINT);
		DrawQ_String(x, y - fs, "^", 0, fs, fs, 0.506f, 0.816f, 1.0f, a, 0, NULL, false, FONT_CENTERPRINT);
	}
}

// For touch_hook_status.
void TouchDodge_PrintStatus(void)
{
	Con_Printf("touch dodge: %s; threats %d, dodges %d, left to the player's own path %d, no escape %d, stopped by the gate %d, course changes %d\n",
		td.blocked_said ? va(vabuf, sizeof(vabuf), "off: %s", td.blocked_said) : (td.active ? "dodging" : "on"),
		td.n_threats, td.n_dodges, td.n_own_path, td.n_no_escape, td.n_blocked, td.n_switched);
	Con_Printf("touch dodge: reaction %.1f frames from the first frame a shot was dangerous, on average (most %d) over %d dodges; delay judged %.0f ms on average\n",
		td.reaction_n ? (float)td.reaction_sum / td.reaction_n : 0.0f, td.reaction_max, td.reaction_n,
		td.lead_n ? td.lead_sum * 1000 / td.lead_n : 0.0f);
}

void TouchDodge_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_autododge);
	Cvar_RegisterVariable(&cl_touch_autododge_window);
	Cvar_RegisterVariable(&cl_touch_autododge_mindamage);
	Cvar_RegisterVariable(&cl_touch_autododge_cue);
	Cvar_RegisterVariable(&cl_touch_autododge_scope);
	Cvar_RegisterVariable(&cl_touch_autododge_rear);
	td.type_ofs = -1;
	td.teamplay_ofs = -1;
}
