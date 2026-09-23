/*
Xonotic Touch: auto-shoot and aim helpers (see touch_aim.h).

Where the other players are: Xonotic networks players as CSQC entities
(CSQCModel), so the engine's own cl.entities[1..maxclients] are empty and the
positions live in whatever CSQC is loaded -- the port's own in a local match,
the server's on a public server. The engine sets .entnum on every networked
CSQC entity, and a player's server entity number is its client slot + 1, so a
CSQC edict whose entnum is 1..maxclients is that player's model, with the
interpolated .origin and the networked .mins/.maxs the CSQC draws it with.
Those are engine-known fields, the same in any CSQC.

Who is an enemy: the engine's scoreboard (cl.scores) carries every player's
colours; in a team game Xonotic forces each player's colours to the team's, so
"same pants colour as me" is "same team". Whether the game is a team game at
all comes from the CSQC's own `teamplay` global. When that is missing the game
is treated as a team game, so a player who shares your colour is never shot.

Dead players: a dead player's entity stays around as the corpse until the
respawn, with solid SOLID_CORPSE (networked by CSQCModel; the server sets it
in PlayerDamage). Corpses copied at respawn, gibs and other bodies are not
player entities at all (their entnum is above maxclients or zero). The
networked death_time is no use here: it arrives as an approximate past time
and reads nonzero for living players too.
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "csprogs.h"
#include "touch_hud.h"
#include "touch_aim.h"

extern cvar_t vid_touchscreen;
extern qbool cl_touch_csqc_active;

cvar_t cl_touch_autoshoot = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot", "1",
	"Touch controls: fire automatically while the crosshair is on a visible enemy player. "
	"Only in touch mode (vid_touchscreen); never active with a keyboard attached. A manual FIRE press always works on top of it"};
cvar_t cl_touch_autoshoot_delay = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_delay", "0.1",
	"Auto-shoot reaction time: seconds the crosshair must stay on an enemy before firing starts (0.05 - 0.5)"};
cvar_t cl_touch_autoshoot_release = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_release", "0.15",
	"Seconds auto-shoot keeps firing after the crosshair leaves the enemy (0 - 0.5)"};
cvar_t cl_touch_aimassist = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist", "1",
	"Touch controls: aim helpers -- the look drag slows down over an enemy (cl_touch_aimassist_friction) and the view is pulled gently toward one while you turn or move (cl_touch_aimassist_strength). "
	"Only in touch mode; never active with a keyboard attached"};
cvar_t cl_touch_aimassist_friction = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_friction", "0.35",
	"Aim slowdown: fraction of look speed removed while the crosshair is on an enemy, fading out to the edge of cl_touch_aimassist_cone (0 = off, at most 0.8)"};
cvar_t cl_touch_aimassist_strength = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_strength", "0.15",
	"Aim pull: fraction of your own turn speed added toward the nearest visible enemy inside the cone while a finger is on the aim control and you turn or move (0 = off, at most 0.5)"};
cvar_t cl_touch_aimassist_maxrate = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_maxrate", "15",
	"Aim pull: hard cap on the pull, degrees per second (at most 30); it never snaps"};
cvar_t cl_touch_aimassist_cone = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_cone", "6",
	"Aim helpers: half-angle in degrees around the crosshair within which an enemy counts as near (at most 10)"};
cvar_t cl_touch_aim_range = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_range", "3000",
	"Auto-shoot and aim helpers ignore enemies farther away than this many units"};
cvar_t cl_touch_aim_debug = {CF_CLIENT, "cl_touch_aim_debug", "0",
	"Print auto-shoot and aim-assist target changes to the console (2 = also teammates under the crosshair)"};

// Engine -> CSQC: whether auto-shoot is firing (the port's FIRE button glows),
// and the look multiplier the port's touch_look.qc applies (aim friction).
cvar_t cl_touch_autoshoot_active = {CF_CLIENT, "_cl_touch_autoshoot_active", "0",
	"Set by the engine: 1 while touch auto-shoot is holding fire"};
cvar_t cl_touch_aim_lookscale = {CF_CLIENT, "_cl_touch_aim_lookscale", "1",
	"Set by the engine: multiplier for touch look speed (aim friction near an enemy)"};
// CSQC -> engine: the port's touch HUD state. -1 = not live (menu, layout
// editor, chat sheet), 0 = live with no finger on the aim control, 1 = a
// finger is on the look zone or FIRE.
cvar_t cl_touch_hud_state = {CF_CLIENT, "_cl_touch_hud_state", "0",
	"Set by the port's CSQC: -1 touch HUD not live, 0 live, 1 live with a finger on the aim control"};

// Hard ceilings, whatever the cvars say: this runs on public servers.
#define FRICTION_MAX   0.8f
#define STRENGTH_MAX   0.5f
#define MAXRATE_MAX    30.0f
#define CONE_MAX       10.0f
// Turn rate (deg/s) the pull scales with while the player strafes without
// turning: at the default strength that is 6 deg/s, a slow drift.
#define MOVE_REF_RATE  40.0f
// A per-frame view change larger than this is a teleport or respawn, not a turn.
#define TURN_SPIKE_DEG 60.0f

static struct
{
	qbool was_allowed;
	qbool prev_valid;
	vec3_t prev_angles;
	double on_since;     // crosshair entered an enemy (0 = not on one)
	double last_on;      // last frame the crosshair was on an enemy
	qbool firing;
	float lookscale;
	qbool hud_aim_touch;
	int debug_target, debug_friend;
	// from the loaded CSQC
	int teamplay_ofs, teamplay_type;
} ta;

void TouchAim_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_autoshoot);
	Cvar_RegisterVariable(&cl_touch_autoshoot_delay);
	Cvar_RegisterVariable(&cl_touch_autoshoot_release);
	Cvar_RegisterVariable(&cl_touch_aimassist);
	Cvar_RegisterVariable(&cl_touch_aimassist_friction);
	Cvar_RegisterVariable(&cl_touch_aimassist_strength);
	Cvar_RegisterVariable(&cl_touch_aimassist_maxrate);
	Cvar_RegisterVariable(&cl_touch_aimassist_cone);
	Cvar_RegisterVariable(&cl_touch_aim_range);
	Cvar_RegisterVariable(&cl_touch_aim_debug);
	Cvar_RegisterVariable(&cl_touch_autoshoot_active);
	Cvar_RegisterVariable(&cl_touch_aim_lookscale);
	Cvar_RegisterVariable(&cl_touch_hud_state);
	ta.lookscale = 1;
	ta.teamplay_ofs = -1;
}

void TouchAim_ProgsChanged(void)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *d;
	ta.teamplay_ofs = -1;
	if (!prog->loaded)
		return;
	d = PRVM_ED_FindGlobal(prog, "teamplay");
	if (d)
	{
		// a float in most builds; gmqcc can also emit bool/int globals as ints
		ta.teamplay_type = d->type & ~DEF_SAVEGLOBAL;
		if (ta.teamplay_type != ev_string && ta.teamplay_type != ev_vector && ta.teamplay_type != ev_entity
			&& ta.teamplay_type != ev_field && ta.teamplay_type != ev_function && ta.teamplay_type != ev_void)
			ta.teamplay_ofs = d->ofs;
	}
}

qbool TouchAim_AutoFiring(void)
{
	return ta.firing;
}

float TouchAim_LookScale(void)
{
	return ta.lookscale;
}

void TouchAim_SetHudAimTouch(qbool touching)
{
	ta.hud_aim_touch = touching;
}

static void publish(void)
{
	if ((cl_touch_autoshoot_active.integer != 0) != ta.firing)
		Cvar_SetValueQuick(&cl_touch_autoshoot_active, ta.firing ? 1 : 0);
	if (fabs(cl_touch_aim_lookscale.value - ta.lookscale) > 0.004f)
		Cvar_SetValueQuick(&cl_touch_aim_lookscale, ta.lookscale);
}

static void set_firing(qbool on, int target)
{
	if (ta.firing == on)
		return;
	ta.firing = on;
	if (cl_touch_aim_debug.integer)
	{
		if (on && target > 0 && target <= cl.maxclients)
			Con_Printf("touch aim: auto-shoot ON at player %d (%s)\n", target, cl.scores[target - 1].name);
		else
			Con_Printf("touch aim: auto-shoot OFF\n");
	}
}

static void reset(void)
{
	set_firing(false, 0);
	ta.on_since = 0;
	ta.prev_valid = false;
	ta.lookscale = 1;
	ta.debug_target = 0;
	ta.debug_friend = 0;
}

static qbool is_spectator_slot(int n)
{
	int frags = cl.scores[n - 1].frags;
	// Nexuiz-derived games mark spectators and out-of-game players this way
	// (the engine's own trace code relies on it, cl_collision.c).
	return frags == -666 || frags == -616;
}

// Whether this frame may assist at all.
static qbool aim_allowed(void)
{
	int me = cl.realplayerentity;
	// Touch mode only: the engine clears vid_touchscreen when a physical
	// keyboard is attached (vid_touchdetect.c), so a keyboard and mouse player
	// never gets any of this.
	if (!vid_touchscreen.integer)
		return false;
	if (cls.state != ca_connected || cls.signon != SIGNONS || cls.demoplayback)
		return false;
	if (key_dest != key_game || key_consoleactive)
		return false;
	if (cl.intermission || !CLVM_prog->loaded || !cl.scores)
		return false;
	if (me < 1 || me > cl.maxclients || cl.viewentity != me)
		return false; // spectating someone, or in a vehicle / camera view
	if (is_spectator_slot(me) || cl.stats[STAT_HEALTH] <= 0)
		return false;
	// The port's CSQC HUD is not live (menu, layout editor, chat sheet).
	if (cl_touch_csqc_active && cl_touch_hud_state.integer < 0)
		return false;
	return true;
}

// 1 team game, 0 free-for-all, -1 unknown.
static int teamplay_mode(void)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_eval_t *v;
	if (ta.teamplay_ofs < 0 || !prog->loaded)
		return -1;
	v = PRVM_GLOBALFIELDVALUE(ta.teamplay_ofs);
	if (ta.teamplay_type == ev_float)
		return v->_float != 0;
	return v->_int != 0;
}

static qbool is_enemy(int n, int team_game)
{
	const scoreboard_t *them = &cl.scores[n - 1];
	const scoreboard_t *me = &cl.scores[cl.realplayerentity - 1];
	if (!them->name[0] || is_spectator_slot(n))
		return false;
	if (team_game == 0)
		return true;
	// Team game, or not known: the same pants colour is the same team.
	return (them->colors & 15) != (me->colors & 15);
}

// Clear line through world geometry (and CSQC brush models such as doors).
static qbool line_clear(const vec3_t from, const vec3_t to)
{
	trace_t tr = CL_TraceLine(from, to, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0,
		collision_extendmovelength.value, true, false, NULL, true, false);
	return !tr.startsolid && tr.fraction >= 0.999f;
}

// Ray from o along unit d against an axis-aligned box: entry distance.
static qbool ray_box(const vec3_t o, const vec3_t d, const vec3_t bmin, const vec3_t bmax, float *tnear)
{
	float t0 = 0, t1 = 1e9f;
	int k;
	for (k = 0; k < 3; k++)
	{
		if (fabs(d[k]) < 1e-6f)
		{
			if (o[k] < bmin[k] || o[k] > bmax[k])
				return false;
		}
		else
		{
			float inv = 1.0f / d[k];
			float a = (bmin[k] - o[k]) * inv, b = (bmax[k] - o[k]) * inv;
			if (a > b)
			{
				float t = a;
				a = b;
				b = t;
			}
			t0 = max(t0, a);
			t1 = min(t1, b);
			if (t0 > t1)
				return false;
		}
	}
	*tnear = t0;
	return true;
}

static float wrap180(float a)
{
	a = fmod(a + 180.0f, 360.0f);
	if (a < 0)
		a += 360.0f;
	return a - 180.0f;
}

// View angles (Quake convention: positive pitch looks down) that point at p.
static void angles_to(const vec3_t eye, const vec3_t p, float *yaw, float *pitch)
{
	vec3_t d;
	VectorSubtract(p, eye, d);
	*yaw = atan2(d[1], d[0]) * (180.0f / M_PI);
	*pitch = -atan2(d[2], sqrt(d[0] * d[0] + d[1] * d[1])) * (180.0f / M_PI);
}

typedef struct aimtarget_s
{
	int entnum;
	float err;          // degrees between the crosshair and the aim point
	float radius;       // angular half-width of the body, degrees
	float yaw, pitch;   // view angles that point at the aim point
} aimtarget_t;

// One pass over the players the CSQC is drawing: the one under the crosshair
// (for auto-shoot) and the nearest one inside the cone (for the helpers). Both
// must be enemies, alive, drawn, in range and in line of sight.
static void scan(const vec3_t eye, const vec3_t fwd, float cone, int *on_target, int *friend_on, aimtarget_t *near_out)
{
	prvm_prog_t *prog = CLVM_prog;
	int team_game = teamplay_mode();
	float range = max(64.0f, cl_touch_aim_range.value);
	float best_on = 1e9f;
	int i;
	*on_target = 0;
	*friend_on = 0;
	near_out->entnum = 0;
	near_out->err = 1e9f;
	for (i = 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		vec3_t org, mins, maxs, bmin, bmax, center, head, dir;
		float dist, err, t, alpha, hw;
		int n;
		qbool enemy;
		if (ed->free)
			continue;
		n = (int)PRVM_clientedictfloat(ed, entnum);
		if (n < 1 || n > cl.maxclients || n == cl.realplayerentity || n == cl.viewentity)
			continue;
		if (is_spectator_slot(n))
			continue;
		enemy = is_enemy(n, team_game);
		// alive, and drawn visibly
		if (PRVM_clientedictfloat(ed, solid) == SOLID_CORPSE)
			continue;
		if (!PRVM_clientedictfloat(ed, modelindex) || !PRVM_clientedictfloat(ed, drawmask))
			continue;
		if ((int)PRVM_clientedictfloat(ed, effects) & EF_NODRAW)
			continue;
		alpha = PRVM_clientedictfloat(ed, alpha);
		if (alpha < 0 || (alpha > 0 && alpha < 0.4f))
			continue; // invisibility: never reveal a hidden player
		VectorCopy(PRVM_clientedictvector(ed, origin), org);
		VectorCopy(PRVM_clientedictvector(ed, mins), mins);
		VectorCopy(PRVM_clientedictvector(ed, maxs), maxs);
		if (maxs[0] - mins[0] < 1 || maxs[2] - mins[2] < 1)
		{
			VectorCopy(cl.playerstandmins, mins);
			VectorCopy(cl.playerstandmaxs, maxs);
		}
		VectorAdd(org, mins, bmin);
		VectorAdd(org, maxs, bmax);
		VectorMAM(0.5f, bmin, 0.5f, bmax, center);
		VectorSubtract(center, eye, dir);
		dist = VectorLength(dir);
		if (dist < 1 || dist > range)
			continue;
		VectorScale(dir, 1.0f / dist, dir);
		if (!enemy)
		{
			// only noted for cl_touch_aim_debug 2: a teammate under the crosshair
			if (cl_touch_aim_debug.integer >= 2 && ray_box(eye, fwd, bmin, bmax, &t))
				*friend_on = n;
			continue;
		}
		hw = 0.5f * max(maxs[0] - mins[0], maxs[1] - mins[1]);

		// under the crosshair: the view ray enters the body, and nothing solid
		// stands between the eye and that point
		if (ray_box(eye, fwd, bmin, bmax, &t) && t < best_on)
		{
			vec3_t hit;
			VectorMA(eye, max(0.0f, t - 1.0f), fwd, hit);
			if (line_clear(eye, hit))
			{
				best_on = t;
				*on_target = n;
			}
		}

		// near the crosshair: angle to the chest, or the head if the chest is
		// hidden behind cover
		err = acos(bound(-1.0f, DotProduct(fwd, dir), 1.0f)) * (180.0f / M_PI);
		if (err > cone || err >= near_out->err)
			continue;
		VectorCopy(center, head);
		head[2] = bmax[2] - 6;
		if (!line_clear(eye, center))
		{
			if (!line_clear(eye, head))
				continue;
			VectorCopy(head, center);
		}
		near_out->entnum = n;
		near_out->err = err;
		near_out->radius = atan(hw / dist) * (180.0f / M_PI);
		angles_to(eye, center, &near_out->yaw, &near_out->pitch);
	}
}

void TouchAim_Frame(void)
{
	double now = host.realtime;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	qbool autoshoot, assist, aim_touch, moving, turning;
	float friction, strength, cone, turn_yaw, turn_pitch, turn_mag;
	vec3_t eye, fwd;
	int on_target, friend_on;
	aimtarget_t near;

	if (!aim_allowed())
	{
		if (ta.was_allowed && cl_touch_aim_debug.integer)
			Con_Printf("touch aim: inactive\n");
		ta.was_allowed = false;
		reset();
		publish();
		return;
	}
	if (!ta.was_allowed && cl_touch_aim_debug.integer)
		Con_Printf("touch aim: active (teamplay %d, my colours %d, %s HUD)\n", teamplay_mode(),
			cl.scores[cl.realplayerentity - 1].colors, TouchHUD_Active() ? "engine" : "CSQC");
	ta.was_allowed = true;

	autoshoot = cl_touch_autoshoot.integer != 0;
	assist = cl_touch_aimassist.integer != 0;
	friction = assist ? bound(0.0f, cl_touch_aimassist_friction.value, FRICTION_MAX) : 0;
	strength = assist ? bound(0.0f, cl_touch_aimassist_strength.value, STRENGTH_MAX) : 0;
	cone = bound(1.0f, cl_touch_aimassist_cone.value, CONE_MAX);
	if (!autoshoot && friction <= 0 && strength <= 0)
	{
		reset();
		publish();
		return;
	}

	// How far the player turned since the last frame: the touch drag (engine
	// HUD this frame, port CSQC last frame) is the only thing that turns the
	// view in touch mode, so this is the player's own input.
	turn_yaw = turn_pitch = 0;
	if (ta.prev_valid && !cl.fixangle[0])
	{
		turn_yaw = wrap180(cl.viewangles[YAW] - ta.prev_angles[YAW]);
		turn_pitch = cl.viewangles[PITCH] - ta.prev_angles[PITCH];
		if (fabs(turn_yaw) > TURN_SPIKE_DEG || fabs(turn_pitch) > TURN_SPIKE_DEG)
			turn_yaw = turn_pitch = 0;
	}
	turn_mag = sqrt(turn_yaw * turn_yaw + turn_pitch * turn_pitch);
	turning = turn_mag > 0.01f;
	moving = cl.cmd.forwardmove != 0 || cl.cmd.sidemove != 0;
	if (TouchHUD_Active())
		aim_touch = ta.hud_aim_touch;
	else if (cl_touch_csqc_active)
		aim_touch = cl_touch_hud_state.integer > 0;
	else
		aim_touch = false;

	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	AngleVectors(cl.viewangles, fwd, NULL, NULL);
	scan(eye, fwd, cone, &on_target, &friend_on, &near);

	if (cl_touch_aim_debug.integer && on_target != ta.debug_target)
	{
		if (on_target)
			Con_Printf("touch aim: crosshair on player %d (%s)\n", on_target, cl.scores[on_target - 1].name);
		ta.debug_target = on_target;
	}
	if (cl_touch_aim_debug.integer >= 2 && friend_on != ta.debug_friend)
	{
		if (friend_on)
			Con_Printf("touch aim: crosshair on teammate %d (%s), not firing\n", friend_on, cl.scores[friend_on - 1].name);
		ta.debug_friend = friend_on;
	}

	// auto-shoot: fire after the reaction delay, keep firing a moment after
	// the crosshair slips off so a jittery track does not stutter
	if (autoshoot && on_target)
	{
		if (!ta.on_since)
			ta.on_since = now;
		ta.last_on = now;
		if (now - ta.on_since >= bound(0.05f, cl_touch_autoshoot_delay.value, 0.5f))
			set_firing(true, on_target);
	}
	else
	{
		ta.on_since = 0;
		if (!autoshoot || now - ta.last_on >= bound(0.0f, cl_touch_autoshoot_release.value, 0.5f))
			set_firing(false, 0);
	}

	// friction: full over the body, fading to nothing at the cone's edge
	ta.lookscale = 1;
	if (friction > 0 && (on_target || near.entnum))
	{
		float f = 1;
		if (!on_target && near.err > near.radius)
			f = 1.0f - (near.err - near.radius) / max(0.5f, cone - near.radius);
		ta.lookscale = 1.0f - friction * bound(0.0f, f, 1.0f);
	}

	// magnetism: only with a finger on the aim control, only while turning or
	// moving, never against a turn that is heading away from the target, and
	// capped per second so it can never snap
	if (strength > 0 && near.entnum && aim_touch && (turning || moving))
	{
		float ey = wrap180(near.yaw - cl.viewangles[YAW]);
		float ep = near.pitch - cl.viewangles[PITCH];
		float elen = sqrt(ey * ey + ep * ep);
		qbool away = turning && (turn_yaw * ey + turn_pitch * ep) < 0;
		if (elen > 0.05f && !away)
		{
			float rate = strength * (turning ? turn_mag / dt : MOVE_REF_RATE);
			float step = min(rate, bound(0.0f, cl_touch_aimassist_maxrate.value, MAXRATE_MAX)) * dt;
			step = min(step, elen);
			cl.viewangles[YAW] += ey / elen * step;
			cl.viewangles[PITCH] = bound(-89.0f, cl.viewangles[PITCH] + ep / elen * step, 89.0f);
		}
	}

	VectorCopy(cl.viewangles, ta.prev_angles);
	ta.prev_valid = true;
	publish();
}
