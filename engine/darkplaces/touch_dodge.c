/*
Xonotic Touch: auto dodge. When an enemy projectile the player can see is
about to hit, the move steps aside for them; the view, auto-shoot and the aim
pull carry on untouched.

What the client sees of a projectile: the CSQC edict with FL_PROJECTILE, its
csqcprojectile_type (.cnt in 0.8.6 CSQCs), origin and velocity, its gravity
factor, and a team byte the CSQC turns into .colormap -- the owner's team
colours in a team game, the owner's own colours otherwise. No owner id: a
projectile in the player's own colours is theirs or a teammate's, and is left
alone. Hitscan shots give no warning, and the 6000 u/s Blaster and HLAC bolts
next to none: neither is dodged (touch_dodge.h's table).

The escape goes into the outgoing move (cl.cmd, after the player's own input
is in it), never into the key state, and a jump or a dodging-mutator burst
into the move's button bits (touch_hook.c ORs them in). The player's own
steering wins sideways: the touch move stick is digital (full speed once past
its dead zone), so how far it is pushed says nothing, but where it points
does. Moving mostly across the shot's path, either way, the player is
steering out of the line or deliberately across it, and the dodge stands
down; running toward or away from the shooter, the dodge takes them sideways
until the shot has passed (well under a second), at full speed: keeping part
of the run would cost most of the sideways acceleration (Xonotic accelerates
only up to the speed already going the wished way). Never with a keyboard or
a mouse aiming
(the aim helpers' rules), while hooking or being rescued, or during a quick
turn.
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
	"Only in touch mode; your own steering always wins"};
cvar_t cl_touch_autododge_window = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge_window", "0.8",
	"Auto dodge: how far ahead a shot is judged, seconds (0.3 - 1.5)"};
cvar_t cl_touch_autododge_mindamage = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autododge_mindamage", "15",
	"Auto dodge: only for a shot that would do at least this much damage, and an escape that saves at least half of it (5 - 100)"};

#define FL_PROJECTILE_BIT 32768   // FL_PROJECTILE, BIT(15) in common/constants.qh
#define MUT_DODGING_BIT   0
#define DODGE_BUTTON      8192    // button11: +dodge
#define JUMP_BUTTON       2
#define STEERING_ACROSS   0.7f    // the player's own move this much across the shot's path: theirs
#define KEEP_RUN          0.0f    // share of a run toward / away from the shooter kept while dodging
#define REJUDGE           0.1f    // seconds between second looks at a dodge under way
#define WINDOW_MIN        0.3f
#define WINDOW_MAX        1.5f
#define TRACK_MAX         256

static char vabuf[64];

static struct
{
	int type_ofs, type_type;  // csqcprojectile_type, or .cnt
	int flags_ofs, flags_type; // .flags: the CSQC may declare it int or float
	int draw_ofs, draw_func;   // .draw and Projectile_Draw: the surest mark of a projectile
	qbool type_is_cnt;
	// a dodge under way
	qbool active;
	td_plan_t plan;
	float across[2];          // unit, square to the shot's path
	int proj;                 // the edict it is for
	double until, judged, started;
	int buttons;
	// for the debug lines
	const char *said;
	// velocities derived from positions, for projectiles that send none
	struct { vec3_t org; double time; int ent; } track[TRACK_MAX];
	double cost_sum;
	int cost_frames;
	double cost_report;
} td;

void TouchDodge_ProgsChanged(void)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *d;
	td.type_ofs = -1;
	td.flags_ofs = -1;
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
}

int TouchDodge_Buttons(void)
{
	return td.buttons;
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
static void proj_velocity(prvm_prog_t *prog, prvm_edict_t *ed, int n, const vec3_t org, vec3_t vel)
{
	int k = n & (TRACK_MAX - 1);
	VectorCopy(PRVM_clientedictvector(ed, velocity), vel);
	if (VectorLength2(vel) < 1 && td.track[k].ent == n && host.realtime > td.track[k].time && host.realtime - td.track[k].time < 0.3)
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

void TouchDodge_Frame(void)
{
	prvm_prog_t *prog = CLVM_prog;
	const char *blocked = TouchAim_BlockReason();
	double now = host.realtime, t0 = Sys_DirtyTime();
	float window = bound(WINDOW_MIN, cl_touch_autododge_window.value, WINDOW_MAX);
	float mindmg = bound(5.0f, cl_touch_autododge_mindamage.value, 100.0f);
	vec3_t eye, fwd, right, ang, center;
	float own_f, own_s, own_mag, own_dir[2] = { 0, 0 };
	tr_world_t w;
	td_player_t p;
	td_move_t m;
	td_choice_t best, c;
	float best_vel[2] = { 0, 0 };
	int i, best_ent = 0, mycolors, traces = 0;
	const char *best_name = "";
	int best_count = 0;
	float best_dist = 0;

	td.buttons = 0;
	if (!cl_touch_autododge.integer || blocked || TouchHook_InUse() || TouchHook_Rescuing() || !prog->loaded || td.type_ofs < 0)
	{
		if (td.active)
			stop(blocked ? blocked : "not now (hook, rescue or turn)");
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
	if (td.active && own_mag > 0.1f && fabs(own_dir[0] * td.across[0] + own_dir[1] * td.across[1]) > STEERING_ACROSS)
		stop("the player is steering sideways");

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
	TouchHook_World(&w);
	mycolors = cl.realplayerentity >= 1 && cl.realplayerentity <= cl.maxclients ? cl.scores[cl.realplayerentity - 1].colors & 255 : -1;

	// a dodge under way: on until the shot has passed, looked at again now and then
	if (td.active)
	{
		prvm_edict_t *ed = td.proj > 0 && td.proj < prog->num_edicts ? PRVM_EDICT_NUM(td.proj) : NULL;
		if (!ed || ed->free || !is_projectile(prog, ed))
			stop("the shot is gone");
		else if (now >= td.until)
			stop("the shot has passed");
	}

	// the shots coming: judged together (a Crylink's pellets, a Hagar burst,
	// two players' rockets), the soonest TD_MAX_PROJ of them
	memset(&best, 0, sizeof(best));
	if (!td.active || now - td.judged >= REJUDGE)
	{
		td_proj_t prs[TD_MAX_PROJ];
		float eta[TD_MAX_PROJ], dists[TD_MAX_PROJ];
		const char *names[TD_MAX_PROJ];
		int ents[TD_MAX_PROJ], nprs = 0, k;
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
			if (!kind || (td.type_is_cnt && kind->kind != TD_KIND_ROCKET && kind->kind != TD_KIND_GRENADE
				&& kind->kind != TD_KIND_ELECTRO_BEAM && kind->kind != TD_KIND_HAGAR))
				continue; // .cnt is used by other entities too: only the common ones there
			VectorCopy(PRVM_clientedictvector(ed, origin), org);
			proj_velocity(prog, ed, i, org, vel);
			if (cl_touch_aim_debug.integer >= 3)
				Con_Printf("touch dodge: sees %s (edict %d) colours %d (mine %d) speed %.0f at %.0f units\n", kind->name, i,
					(int)PRVM_clientedictfloat(ed, colormap) & 255, mycolors, VectorLength(vel), VectorDistance(org, center));
			if (mycolors >= 0 && ((int)PRVM_clientedictfloat(ed, colormap) & 255) == mycolors)
				continue; // the player's own or a teammate's
			VectorSubtract(center, org, to);
			dist = VectorLength(to);
			speed = VectorLength(vel);
			if (speed < 50 || DotProduct(vel, to) <= 0 || dist > speed * (window + 0.2f))
				continue; // still, flying away, or not here within the window
			if (!visible(eye, org))
				continue; // the player could not see it
			memset(&pr, 0, sizeof(pr));
			VectorCopy(org, pr.org);
			VectorCopy(vel, pr.vel);
			pr.gravity = PRVM_clientedictfloat(ed, gravity) * m.gravity;
			pr.damage = kind->damage;
			pr.edge = kind->edge;
			pr.radius = kind->radius;
			pr.size = kind->size;
			pr.world_blast = kind->world_blast;
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
		if (nprs)
		{
			best_name = names[0];
			best_dist = dists[0];
		}
		if (nprs)
		{
			TouchDodge_ChooseAll(&w, prs, nprs, &p, &m, td.active ? td.plan.dir : own_dir, window, mindmg * 0.5f, &c);
			best = c;
			best_vel[0] = best_vel[1] = 0;
			for (k = 0; k < nprs; k++)
			{
				float l = sqrt(prs[k].vel[0] * prs[k].vel[0] + prs[k].vel[1] * prs[k].vel[1]);
				if (l > 1)
				{
					best_vel[0] += prs[k].vel[0] / l;
					best_vel[1] += prs[k].vel[1] / l;
				}
			}
			best_ent = ents[0];
			best_count = nprs;
		}
		traces = w.traces;
		td.judged = now;
		if (!td.active && best.base_damage >= mindmg && best.plan > 0 && best.base_damage - best.damage >= mindmg * 0.5f)
		{
			// square to the shot's path (from the escape: its part across the shot)
			float sx = best_vel[0], sy = best_vel[1], sl = sqrt(sx * sx + sy * sy);
			if (sl > 1)
			{
				td.across[0] = -sy / sl;
				td.across[1] = sx / sl;
			}
			else
			{
				td.across[0] = 1;
				td.across[1] = 0;
			}
			if (own_mag > 0.1f && fabs(own_dir[0] * td.across[0] + own_dir[1] * td.across[1]) > STEERING_ACROSS)
			{
				if (cl_touch_aim_debug.integer && td.said != best_name)
					Con_Printf("touch dodge: %s coming, the player is steering across it: left to them\n", best_name);
				td.said = best_name;
				goto done;
			}
			td.active = true;
			td.plan = best.chosen;
			td.proj = best_ent;
			td.started = now;
			td.until = now + best.when + 0.1;
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch dodge: %s%s at %.0f units, hits in %.2f s for %.0f; escape %s%s -> %.0f (%d ruled out by walls or ledges)\n",
					best_name, best_count > 1 ? va(vabuf, sizeof(vabuf), " and %d more", best_count - 1) : "", best_dist, best.when, best.base_damage, best.chosen.name,
					best.chosen.dodge ? " with a dodge" : best.chosen.jump ? " with a jump" : "", best.damage, best.rejected);
		}
		else if (!td.active && best.base_damage >= mindmg && cl_touch_aim_debug.integer && td.said != best_name)
		{
			Con_Printf("touch dodge: %s at %.0f units, hits in %.2f s for %.0f; no escape does better (%d ruled out)\n",
				best_name, best_dist, best.when, best.base_damage, best.rejected);
			td.said = best_name;
		}
	}

done:
	if (td.active)
	{
		// the move: the escape's direction (plus KEEP_RUN of the player's own
		// run along the shot's path), in the view's frame, at full speed
		float d[2], along[2], a, l;
		float df, ds;
		along[0] = own_dir[0] - (own_dir[0] * td.across[0] + own_dir[1] * td.across[1]) * td.across[0];
		along[1] = own_dir[1] - (own_dir[0] * td.across[0] + own_dir[1] * td.across[1]) * td.across[1];
		a = own_mag > 0.1f ? KEEP_RUN : 0;
		d[0] = td.plan.dir[0] + a * along[0];
		d[1] = td.plan.dir[1] + a * along[1];
		l = sqrt(d[0] * d[0] + d[1] * d[1]);
		if (l > 0.01f)
		{
			d[0] /= l;
			d[1] /= l;
		}
		df = d[0] * fwd[0] + d[1] * fwd[1];
		ds = d[0] * right[0] + d[1] * right[1];
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

void TouchDodge_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_autododge);
	Cvar_RegisterVariable(&cl_touch_autododge_window);
	Cvar_RegisterVariable(&cl_touch_autododge_mindamage);
	td.type_ofs = -1;
}
