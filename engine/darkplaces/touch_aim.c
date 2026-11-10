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
	"Touch controls: aim helpers -- the look drag slows down over an enemy (cl_touch_aimassist_friction) and the view is pulled gently toward one while a finger is on the aim control (cl_touch_aimassist_strength). "
	"Only in touch mode; never active with a keyboard attached"};
cvar_t cl_touch_aimassist_friction = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_friction", "0.45",
	"Aim slowdown: fraction of look speed removed while the crosshair is on an enemy, fading out to the edge of cl_touch_aimassist_cone (0 = off, at most 0.8)"};
cvar_t cl_touch_aimassist_strength = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_strength", "0.35",
	"Aim pull toward the nearest visible enemy inside the cone while a finger is on the aim control: follows strength x 2 of its motion across the view and closes the gap at strength x 10 per second, or strength x your own turn speed, whichever is faster (0 = off, at most 0.6)"};
cvar_t cl_touch_aimassist_maxrate = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_maxrate", "40",
	"Aim pull: hard cap on the pull, degrees per second (at most 60); it never snaps"};
cvar_t cl_touch_aimassist_cone = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_cone", "8",
	"Aim helpers: half-angle in degrees around the crosshair within which an enemy counts as near (at most 12)"};
cvar_t cl_touch_autoshoot_weapons = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_weapons", "0",
	"Auto-shoot weapons: 0 = every weapon, 1 = only instant-hit weapons (the game reports the weapon in _cl_touch_weapon_class; a server whose game does not leaves this unfiltered)"};
cvar_t cl_touch_autoshoot_aimonly = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_aimonly", "0",
	"Auto-shoot only while a finger is on the aim control (the look area or FIRE); 0 = also with no finger down"};
cvar_t cl_touch_aim_ring = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_ring", "1",
	"Touch HUD: draw a ring at the crosshair while an enemy is within the aim-assist cone, a red one while the crosshair is on an enemy (with aim assist or auto-shoot on)"};
cvar_t cl_touch_aim_range = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_range", "3000",
	"Auto-shoot and aim helpers ignore enemies farther away than this many units"};
cvar_t cl_touch_aim_debug = {CF_CLIENT, "cl_touch_aim_debug", "0",
	"Print auto-shoot and aim-assist target changes to the console (2 = also teammates under the crosshair)"};

// Engine -> CSQC: whether auto-shoot is firing (the port's FIRE button glows),
// and the look multiplier the port's touch_look.qc applies (aim friction).
cvar_t cl_touch_autoshoot_active = {CF_CLIENT, "_cl_touch_autoshoot_active", "0",
	"Set by the engine: 1 while touch auto-shoot is holding fire"};
cvar_t cl_touch_aim_lock = {CF_CLIENT, "_cl_touch_aim_lock", "0",
	"Set by the engine: 2 while the crosshair is on an enemy, 1 while one is within the aim-assist cone, else 0 (the touch HUD draws the crosshair ring from it)"};
// CSQC -> engine: class of the weapon in hand: 0 unknown, 1 instant-hit, 2 other.
cvar_t cl_touch_weapon_class = {CF_CLIENT, "_cl_touch_weapon_class", "0",
	"Set by the port's CSQC: 0 unknown, 1 instant-hit weapon in hand, 2 projectile or other"};
cvar_t cl_touch_aim_lookscale = {CF_CLIENT, "_cl_touch_aim_lookscale", "1",
	"Set by the engine: multiplier for touch look speed (aim friction near an enemy)"};
// CSQC -> engine: the port's touch HUD state. -1 = not live (menu, layout
// editor, chat sheet), 0 = live with no finger on the aim control, 1 = a
// finger is on the look zone or FIRE.
cvar_t cl_touch_hud_state = {CF_CLIENT, "_cl_touch_hud_state", "0",
	"Set by the port's CSQC: -1 touch HUD not live, 0 live, 1 live with a finger on the aim control"};

// Hard ceilings, whatever the cvars say: this runs on public servers.
#define FRICTION_MAX   0.8f
#define STRENGTH_MAX   0.6f
#define MAXRATE_MAX    60.0f
#define CONE_MAX       12.0f
// Pull toward the target with a finger down: the gap (degrees) closes at
// strength * SETTLE_GAIN per second, 3.5/s at the default 0.35, so a 5 degree
// miss is mostly gone in a third of a second; and strength * FOLLOW_SCALE of
// the target's own motion across the view is followed (0.7 at the default).
#define SETTLE_GAIN    10.0f
#define FOLLOW_SCALE   2.0f
// A target that jumps further than this in one frame (teleport, aim point
// switching between chest and head) is not followed that frame.
#define FOLLOW_SPIKE_DEG 5.0f
// The player is aiming while turning faster than INPUT_MIN_RATE (deg/s) or
// moving, and for INPUT_HOLD seconds after: long enough to settle a thumb that
// has just stopped, too short to track anything for a finger left resting.
#define INPUT_MIN_RATE 2.0f
#define INPUT_HOLD     0.5f
// The player's own turn counts as heading away from the target only above
// this rate (deg/s), so the look filter's decaying tail does not cancel the pull.
#define AWAY_MIN_RATE  5.0f
// A per-frame view change larger than this is a teleport or respawn, not a turn.
#define TURN_SPIKE_DEG 60.0f

static struct
{
	qbool was_allowed;
	const char *blocked_said;
	qbool prev_valid;
	vec3_t prev_angles;
	double on_since;     // crosshair entered an enemy (0 = not on one)
	double last_on;      // last frame the crosshair was on an enemy
	double last_input;   // last frame the player turned or moved
	qbool firing;
	float lookscale;
	int lock;
	qbool hud_aim_touch;
	int debug_target, debug_friend;
	int track_ent;               // target followed last frame, and its view angles
	float track_yaw, track_pitch;
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
	Cvar_RegisterVariable(&cl_touch_autoshoot_weapons);
	Cvar_RegisterVariable(&cl_touch_autoshoot_aimonly);
	Cvar_RegisterVariable(&cl_touch_aim_lock);
	Cvar_RegisterVariable(&cl_touch_weapon_class);
	Cvar_RegisterVariable(&cl_touch_aim_ring);
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
	if (cl_touch_aim_lock.integer != ta.lock)
		Cvar_SetValueQuick(&cl_touch_aim_lock, ta.lock);
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
	ta.lock = 0;
	ta.debug_target = 0;
	ta.debug_friend = 0;
	ta.track_ent = 0;
}

static qbool is_spectator_slot(int n)
{
	int frags = cl.scores[n - 1].frags;
	// Nexuiz-derived games mark spectators and out-of-game players this way
	// (the engine's own trace code relies on it, cl_collision.c).
	return frags == -666 || frags == -616;
}

// Why this frame may not assist, or NULL when it may. The reason is printed
// under cl_touch_aim_debug so "it does nothing" can be told apart from "it is
// switched off for this reason".
static const char *aim_block_reason(void)
{
	int me = cl.realplayerentity;
	// Touch mode only: the engine clears vid_touchscreen when a physical
	// keyboard is being played on (vid_touchdetect.c), so a keyboard and
	// mouse player never gets any of this. Nor does a mouse aiming next to
	// the touch controls: help is for a thumb, not a mouse.
	if (!vid_touchscreen.integer)
		return "vid_touchscreen is 0 (keyboard or mouse in use)";
	if (vid_touchscreen_mouselook.integer)
		return "a mouse is aiming next to the touch controls";
	if (cls.state != ca_connected || cls.signon != SIGNONS || cls.demoplayback)
		return "not in a live game";
	if (key_dest != key_game || key_consoleactive)
		return "menu or console is open";
	if (cl.intermission || !CLVM_prog->loaded || !cl.scores)
		return "intermission or no CSQC";
	if (me < 1 || me > cl.maxclients || cl.viewentity != me)
		return "spectating or in another view";
	if (is_spectator_slot(me) || cl.stats[STAT_HEALTH] <= 0)
		return "dead or spectator";
	// The port's CSQC HUD is not live (menu, layout editor, chat sheet).
	if (cl_touch_csqc_active && cl_touch_hud_state.integer < 0)
		return "touch HUD not live (menu, layout editor or chat open)";
	return NULL;
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
	float err;          // degrees from the crosshair to the nearest edge of the body (0 on it)
	float rh, rv;       // angular half-width and half-height of the body, degrees
	float yaw, pitch;   // view angles that point at the aim point (chest, or head behind cover)
} aimtarget_t;

static float signf(float v)
{
	return v < 0 ? -1.0f : 1.0f;
}

// Degrees from the view to the body's edge, given the view's offset from the
// aim point and the body's angular half-extents: 0 while the crosshair is on it.
static float edge_err(float ey, float ep, float rh, float rv)
{
	float x = max(0.0f, fabs(ey) - rh), z = max(0.0f, fabs(ep) - rv);
	return sqrt(x * x + z * z);
}

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

		// near the crosshair: how far the crosshair is from the body's edge,
		// so a big close body counts as soon as the crosshair nears it, not
		// only once its centre is inside the cone. The aim point is the chest,
		// or the head if the chest is hidden behind cover.
		{
			float yaw, pitch, rh, rv, ey, ep;
			qbool on = n == *on_target;
			rh = atan(hw / dist) * (180.0f / M_PI);
			rv = atan(0.5f * (maxs[2] - mins[2]) / dist) * (180.0f / M_PI);
			angles_to(eye, center, &yaw, &pitch);
			ey = wrap180(yaw - cl.viewangles[YAW]);
			ep = pitch - cl.viewangles[PITCH];
			err = on ? 0 : edge_err(ey, ep, rh, rv);
			if (err > cone || err > near_out->err)
				continue;
			if (!on && !line_clear(eye, center))
			{
				VectorCopy(center, head);
				head[2] = bmax[2] - 6;
				if (!line_clear(eye, head))
					continue;
				// only the head shows: aim at it, and count only it as the body
				angles_to(eye, head, &yaw, &pitch);
				rv = atan(6.0f / dist) * (180.0f / M_PI);
				err = edge_err(wrap180(yaw - cl.viewangles[YAW]), pitch - cl.viewangles[PITCH], rh, rv);
				if (err > cone || err > near_out->err)
					continue;
			}
			if (err == near_out->err && near_out->entnum && n != *on_target)
				continue;
			near_out->entnum = n;
			near_out->err = err;
			near_out->rh = rh;
			near_out->rv = rv;
			near_out->yaw = yaw;
			near_out->pitch = pitch;
		}
	}
}

void TouchAim_Frame(void)
{
	double now = host.realtime;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	qbool autoshoot, assist, aim_touch;
	float friction, strength, cone, turn_yaw, turn_pitch, turn_mag;
	vec3_t eye, fwd;
	int on_target, friend_on, on_shoot;
	aimtarget_t near;

	const char *blocked = aim_block_reason();
	if (blocked)
	{
		if (cl_touch_aim_debug.integer && (ta.was_allowed || !ta.blocked_said || strcmp(ta.blocked_said, blocked)))
			Con_Printf("touch aim: inactive: %s\n", blocked);
		ta.blocked_said = blocked;
		ta.was_allowed = false;
		reset();
		publish();
		return;
	}
	if (!ta.was_allowed && cl_touch_aim_debug.integer)
		Con_Printf("touch aim: active (teamplay %d, my colours %d, %s HUD)\n", teamplay_mode(),
			cl.scores[cl.realplayerentity - 1].colors, TouchHUD_Active() ? "engine" : "CSQC");
	ta.was_allowed = true;
	ta.blocked_said = NULL;

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
	if (autoshoot && on_target && cl_touch_autoshoot_weapons.integer == 1 && cl_touch_weapon_class.integer == 2)
		on_shoot = 0; // a projectile weapon in hand and the player asked for instant-hit only
	else if (autoshoot && on_target && cl_touch_autoshoot_aimonly.integer && !aim_touch)
		on_shoot = 0;
	else
		on_shoot = on_target;
	ta.lock = !assist && !autoshoot ? 0 : (on_target ? 2 : (near.entnum ? 1 : 0));
	if (autoshoot && on_shoot)
	{
		if (!ta.on_since)
			ta.on_since = now;
		ta.last_on = now;
		if (now - ta.on_since >= bound(0.05f, cl_touch_autoshoot_delay.value, 0.5f))
			set_firing(true, on_shoot);
	}
	else
	{
		ta.on_since = 0;
		if (!autoshoot || now - ta.last_on >= bound(0.0f, cl_touch_autoshoot_release.value, 0.5f))
			set_firing(false, 0);
	}

	// friction: full over the body, fading to nothing at the cone's edge.
	// Not while the thumb is turning toward a body it has not reached yet:
	// slowing that down only fights the pull and makes the target harder to
	// reach. It is there to stop a turn on the body, or that runs past it.
	ta.lookscale = 1;
	if (friction > 0 && near.entnum)
	{
		float ey = wrap180(near.yaw - cl.viewangles[YAW]), ep = near.pitch - cl.viewangles[PITCH];
		qbool approaching = !on_target && turn_mag / dt > AWAY_MIN_RATE && turn_yaw * ey + turn_pitch * ep > 0;
		if (!approaching)
			ta.lookscale = 1.0f - friction * bound(0.0f, 1.0f - near.err / cone, 1.0f);
	}
	else if (friction > 0 && on_target)
		ta.lookscale = 1.0f - friction;

	// magnetism: only with a finger on the aim control (the look area or
	// FIRE) while the player aims -- turning, moving, or within INPUT_HOLD of
	// either, so a finger resting on the screen never aims by itself -- never
	// against a turn that is heading away from the target, and capped per
	// second so it can never snap. Two parts: follow a share of the target's
	// own motion across the view (it runs, or the player strafes), and close
	// the remaining gap at a rate that grows with it, so a thumb that stops a
	// few degrees short or long still lands on the body.
	if (turn_mag / dt > INPUT_MIN_RATE || cl.cmd.forwardmove != 0 || cl.cmd.sidemove != 0)
		ta.last_input = now;
	if (strength > 0 && near.entnum && aim_touch && now - ta.last_input < INPUT_HOLD)
	{
		float ey = wrap180(near.yaw - cl.viewangles[YAW]);
		float ep = near.pitch - cl.viewangles[PITCH];
		float turn_rate = turn_mag / dt;
		qbool away = turn_rate > AWAY_MIN_RATE && (turn_yaw * ey + turn_pitch * ep) < 0;
		if (!away)
		{
			float py = 0, pp = 0, len, cap;
			if (ta.track_ent == near.entnum)
			{
				float dy = wrap180(near.yaw - ta.track_yaw), dp = near.pitch - ta.track_pitch;
				if (fabs(dy) < FOLLOW_SPIKE_DEG && fabs(dp) < FOLLOW_SPIKE_DEG)
				{
					float follow = min(1.0f, strength * FOLLOW_SCALE);
					py = follow * dy;
					pp = follow * dp;
				}
			}
			// the gap to the inner half of the body: a crosshair on the body's
			// edge is still drawn in, one well inside it is left alone
			{
				float gy = signf(ey) * max(0.0f, fabs(ey) - 0.5f * near.rh);
				float gp = signf(ep) * max(0.0f, fabs(ep) - 0.5f * near.rv);
				float gap = sqrt(gy * gy + gp * gp);
				if (gap > 0.01f)
				{
					float rate = strength * max(turn_rate, SETTLE_GAIN * gap);
					float step = min(rate * dt, gap);
					py += gy / gap * step;
					pp += gp / gap * step;
				}
			}
			len = sqrt(py * py + pp * pp);
			cap = bound(0.0f, cl_touch_aimassist_maxrate.value, MAXRATE_MAX) * dt;
			if (len > cap)
			{
				py *= cap / len;
				pp *= cap / len;
			}
			cl.viewangles[YAW] += py;
			cl.viewangles[PITCH] = bound(-89.0f, cl.viewangles[PITCH] + pp, 89.0f);
		}
	}
	ta.track_ent = near.entnum;
	ta.track_yaw = near.yaw;
	ta.track_pitch = near.pitch;

	VectorCopy(cl.viewangles, ta.prev_angles);
	ta.prev_valid = true;
	publish();
}
