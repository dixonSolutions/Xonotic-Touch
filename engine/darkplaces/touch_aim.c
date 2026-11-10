/*
Xonotic Touch: auto-shoot and aim helpers (see touch_aim.h).

Where the other players are: Xonotic networks players as CSQC entities
(CSQCModel), so the engine's own cl.entities[1..maxclients] are empty and the
positions live in whatever CSQC is loaded -- the port's own in a local match,
the server's on a public server. The engine sets .entnum on every networked
CSQC entity, and a player's server entity number is its client slot + 1, so a
CSQC edict whose entnum is 1..maxclients is that player's model, with the
interpolated .origin and the networked .mins/.maxs the CSQC draws it with.
Those are engine-known fields, the same in any CSQC. So is .velocity, which
CSQCModel derives from successive network updates.

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

The weapon in hand, what is owned and the ammo come from touch_hud.c's
binding of the same CSQC; what each weapon does from touch_weapons.h, by
netname. An unknown weapon (a mod's, or no binding) is fired as before:
primary, no lead, no switching.
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "csprogs.h"
#include "touch_hud.h"
#include "touch_aim.h"
#include "touch_weapons.h"

extern cvar_t vid_touchscreen;
extern qbool cl_touch_csqc_active;
extern kbutton_t in_attack, in_button3;

cvar_t cl_touch_autoshoot = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot", "1",
	"Touch controls: fire automatically while the crosshair is on a visible enemy player. "
	"Only in touch mode (vid_touchscreen); never active with a keyboard attached. A manual FIRE press always works on top of it"};
cvar_t cl_touch_autoshoot_delay = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_delay", "0",
	"Auto-shoot reaction time: seconds the crosshair must stay on an enemy before firing starts (0 = on the first frame, at most 0.5)"};
cvar_t cl_touch_autoshoot_release = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_release", "0.15",
	"Seconds auto-shoot keeps firing after the crosshair leaves the enemy (0 - 0.5)"};
cvar_t cl_touch_autoshoot_radius = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_radius", "1.5",
	"Auto-shoot fires while the crosshair is within this many degrees of the enemy's body (of where a projectile will meet it), "
	"narrowed to what the weapon's spread or splash can cover at that distance; 0 = only on the body (at most 6)"};
cvar_t cl_touch_autoshoot_alt = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_alt", "1",
	"Auto-shoot uses secondary fire when it is the better shot: Shotgun melee point-blank, Crylink, MachineGun burst far away, "
	"Arc bolts beyond the beam, the blaster shot of an empty Vaporizer or Overkill weapon"};
cvar_t cl_touch_autoshoot_lead = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_lead", "1",
	"Auto-shoot and the aim pull lead moving targets with projectile weapons (aim where the shot will meet them) and loft grenades and orbs for the distance"};
cvar_t cl_touch_autoshoot_steer = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_steer", "1",
	"Devastator: auto-shoot keeps fire held so the rocket follows the crosshair, keeps the crosshair on the enemy and detonates the rocket next to them"};
cvar_t cl_touch_autoshoot_safe = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_safe", "1",
	"Auto-shoot holds fire when the shot would catch you in its own blast, the enemy is beyond the weapon's reach, "
	"or the weapon is not one to fire by itself (Port-O-Launch, Grappling Hook, Tuba)"};
cvar_t cl_touch_autoswitch = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoswitch", "1",
	"Touch controls: switch to the best weapon you own for the distance to the enemy you are fighting. "
	"Never within 4 seconds of choosing a weapon yourself, nor while you hold FIRE or ALT"};
cvar_t cl_touch_aimassist = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist", "1",
	"Touch controls: aim helpers -- the look drag slows down over an enemy (cl_touch_aimassist_friction) and the view is pulled gently toward one while a finger is on the aim control (cl_touch_aimassist_strength). "
	"Only in touch mode; never active with a keyboard attached"};
cvar_t cl_touch_aimassist_friction = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_friction", "0.45",
	"Aim slowdown: fraction of look speed removed while the crosshair is on an enemy, fading out to the edge of cl_touch_aimassist_cone (0 = off, at most 0.8)"};
cvar_t cl_touch_aimassist_strength = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_strength", "0.6",
	"Aim pull toward the nearest visible enemy inside the cone (where a projectile will meet it) while a finger is on the aim control, while auto-shoot fires at it and while a Devastator rocket flies: "
	"follows strength x 2 of its motion across the view and closes the gap at strength x 10 per second, or strength x your own turn speed, whichever is faster (0 = off, at most 1)"};
cvar_t cl_touch_aimassist_track = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_track", "1",
	"Aim pull: keep pulling toward the enemy auto-shoot is firing at even while the thumb rests (otherwise only while a finger is on the aim control and aiming)"};
cvar_t cl_touch_aimassist_maxrate = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_maxrate", "90",
	"Aim pull: hard cap on the pull, degrees per second (at most 180); it never snaps"};
cvar_t cl_touch_aimassist_cone = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_cone", "8",
	"Aim helpers: half-angle in degrees around the crosshair within which an enemy counts as near (1 - 15)"};
cvar_t cl_touch_autoshoot_weapons = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_weapons", "0",
	"Auto-shoot weapons: 0 = every weapon, 1 = only instant-hit weapons (judged from the weapon in hand; for a weapon the engine does not know, from _cl_touch_weapon_class)"};
cvar_t cl_touch_autoshoot_aimonly = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_aimonly", "0",
	"Auto-shoot only while a finger is on the aim control (the look area or FIRE); 0 = also with no finger down"};
cvar_t cl_touch_aim_ring = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_ring", "1",
	"Touch HUD: draw a ring at the crosshair while an enemy is within the aim-assist cone, a red one while the crosshair is on an enemy (with aim assist or auto-shoot on)"};
cvar_t cl_touch_aim_range = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_range", "3000",
	"Auto-shoot and aim helpers ignore enemies farther away than this many units"};
cvar_t cl_touch_aim_debug = {CF_CLIENT, "cl_touch_aim_debug", "0",
	"Print auto-shoot and aim-assist decisions to the console: target, weapon, fire mode and why, lead, switches, rockets (2 = also teammates under the crosshair)"};

// Engine -> CSQC: whether auto-shoot is firing (the port's FIRE button glows),
// and the look multiplier the port's touch_look.qc applies (aim friction).
cvar_t cl_touch_autoshoot_active = {CF_CLIENT, "_cl_touch_autoshoot_active", "0",
	"Set by the engine: what touch auto-shoot is holding, 1 primary fire, 2 secondary, 3 both, 0 nothing"};
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
#define STRENGTH_MAX   1.0f
#define MAXRATE_MAX    180.0f
#define CONE_MAX       15.0f
#define RADIUS_MAX     6.0f
// Pull toward the target: the gap (degrees) closes at strength * SETTLE_GAIN
// per second, 6/s at the default 0.6, so a 5 degree miss is mostly gone in a
// fifth of a second; and strength * FOLLOW_SCALE of the target's own motion
// across the view is followed (all of it from strength 0.5).
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
// Target velocity: smoothed over about this many seconds; forgotten when a
// player was not seen for TRACK_STALE seconds or jumps TRACK_JUMP units.
#define TRACK_SMOOTH   0.08f
#define TRACK_STALE    0.5f
#define TRACK_JUMP     300.0f
#define TRACK_MAX      256
// Auto-switch guards (seconds): the enemy must be the target for FIGHT_MIN
// before a switch; none for MANUAL_GUARD after the player picked a weapon,
// nor within OWN_GUARD of the last automatic one; a request that has not
// landed after SWITCH_WAIT is given up.
#define FIGHT_MIN      0.25f
#define MANUAL_GUARD   4.0f
#define OWN_GUARD      2.0f
#define SWITCH_WAIT    1.5f
// Devastator: how far from the eye a fresh rocket may be to be ours, how
// long to wait for it to appear (refire 1.1 s plus the trip to the server),
// the longest flight followed, and how long fire stays released after a
// rocket so the next press launches one (and attack2 is up again).
#define ROCKET_FIND_DIST  300.0f
#define ROCKET_FIND_TIME  1.6f
#define ROCKET_MAX_FLIGHT 4.0f
#define ROCKET_RELEASE    0.06f
#define PROJECTILE_ROCKET 2 // csqcprojectile_type of a Devastator rocket
#define ALTFIRE_READY     2 // TOUCH_ALTFIRE_READY (touch_altfire.h)

// Devastator states.
enum { RK_IDLE, RK_LAUNCH, RK_FLY, RK_DETONATE, RK_RELEASE };
static const char *const rk_state_name[] = { "idle", "launch", "fly", "detonate", "release" };

static char vabuf[256];

typedef struct track_s
{
	vec3_t org, vel;
	double time;
} track_t;

static struct
{
	qbool was_allowed;
	const char *blocked_said;
	qbool prev_valid;
	vec3_t prev_angles;
	double on_since;     // crosshair entered an enemy (0 = not on one)
	double last_on;      // last frame the crosshair was on an enemy
	double last_input;   // last frame the player turned or moved
	qbool firing, firing2;
	int fire_target;     // who auto-shoot is firing at (0 none)
	int last_mode;       // fire mode of the last shot decision (TW_MODE_*)
	float lookscale;
	int lock;
	qbool hud_aim_touch;
	int debug_target, debug_friend;
	int said_target, said_weapon, said_mode, said_why;
	int track_ent;               // target followed last frame, and its view angles
	float track_yaw, track_pitch;
	track_t tracks[TRACK_MAX];   // per player slot: smoothed velocity
	// auto-switch
	int held_last, switch_to;
	double switch_asked, manual_switch, own_switch;
	int fight_ent;
	double fight_since;
	// Devastator steering
	int rk_state, rk_ent, rk_target;
	double rk_time, rk_seen;
	float rk_closest;    // closest the rocket came to the target (debug)
	vec3_t rk_org, rk_vel, rk_target_org, rk_target_vel;
	int rk_before[16], rk_before_count;
	// from the loaded CSQC
	int teamplay_ofs, teamplay_type;
	int projtype_ofs, projtype_type;
} ta;

void TouchAim_Init(void)
{
	Cvar_RegisterVariable(&cl_touch_autoshoot);
	Cvar_RegisterVariable(&cl_touch_autoshoot_delay);
	Cvar_RegisterVariable(&cl_touch_autoshoot_release);
	Cvar_RegisterVariable(&cl_touch_autoshoot_radius);
	Cvar_RegisterVariable(&cl_touch_autoshoot_alt);
	Cvar_RegisterVariable(&cl_touch_autoshoot_lead);
	Cvar_RegisterVariable(&cl_touch_autoshoot_steer);
	Cvar_RegisterVariable(&cl_touch_autoshoot_safe);
	Cvar_RegisterVariable(&cl_touch_autoswitch);
	Cvar_RegisterVariable(&cl_touch_aimassist);
	Cvar_RegisterVariable(&cl_touch_aimassist_friction);
	Cvar_RegisterVariable(&cl_touch_aimassist_strength);
	Cvar_RegisterVariable(&cl_touch_aimassist_track);
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
	ta.projtype_ofs = -1;
	ta.held_last = -1;
	ta.switch_to = -1;
	ta.manual_switch = ta.own_switch = -100;
}

// A float or int global/field the CSQC may declare either way.
static qbool numeric_type(int type)
{
	return type != ev_string && type != ev_vector && type != ev_entity
		&& type != ev_field && type != ev_function && type != ev_void;
}

void TouchAim_ProgsChanged(void)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *d;
	ta.teamplay_ofs = -1;
	ta.projtype_ofs = -1;
	ta.rk_state = RK_IDLE;
	ta.rk_ent = 0;
	ta.held_last = -1;
	ta.switch_to = -1;
	if (!prog->loaded)
		return;
	d = PRVM_ED_FindGlobal(prog, "teamplay");
	if (d)
	{
		// a float in most builds; gmqcc can also emit bool/int globals as ints
		ta.teamplay_type = d->type & ~DEF_SAVEGLOBAL;
		if (numeric_type(ta.teamplay_type))
			ta.teamplay_ofs = d->ofs;
	}
	// what kind of projectile a CSQC projectile entity is (Ent_Projectile)
	d = PRVM_ED_FindField(prog, "csqcprojectile_type");
	if (d)
	{
		ta.projtype_type = d->type & ~DEF_SAVEGLOBAL;
		if (numeric_type(ta.projtype_type))
			ta.projtype_ofs = d->ofs;
	}
}

qbool TouchAim_AutoFiring(void)
{
	return ta.firing;
}

qbool TouchAim_AutoFire2(void)
{
	return ta.firing2;
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
	int active = (ta.firing ? 1 : 0) | (ta.firing2 ? 2 : 0);
	if (cl_touch_autoshoot_active.integer != active)
		Cvar_SetValueQuick(&cl_touch_autoshoot_active, active);
	if (cl_touch_aim_lock.integer != ta.lock)
		Cvar_SetValueQuick(&cl_touch_aim_lock, ta.lock);
	if (fabs(cl_touch_aim_lookscale.value - ta.lookscale) > 0.004f)
		Cvar_SetValueQuick(&cl_touch_aim_lookscale, ta.lookscale);
}

static const char *player_name(int n)
{
	return n > 0 && n <= cl.maxclients ? cl.scores[n - 1].name : "?";
}

// What auto-shoot holds: primary and/or secondary fire, at target.
static void set_firing(qbool on, qbool on2, int target)
{
	if (ta.firing == on && ta.firing2 == on2)
		return;
	ta.firing = on;
	ta.firing2 = on2;
	if (cl_touch_aim_debug.integer)
	{
		if ((on || on2) && target > 0 && target <= cl.maxclients)
			Con_Printf("touch aim: auto-shoot ON%s at player %d (%s)\n", on && on2 ? " (primary+secondary)" : on2 ? " (secondary)" : "", target, player_name(target));
		else
			Con_Printf("touch aim: auto-shoot OFF\n");
	}
}

static void rk_set(int state, const char *why)
{
	if (ta.rk_state == state)
		return;
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch aim: rocket %s -> %s (%s)\n", rk_state_name[ta.rk_state], rk_state_name[state], why);
	ta.rk_state = state;
	ta.rk_time = host.realtime;
}

static void reset(void)
{
	set_firing(false, false, 0);
	ta.on_since = 0;
	ta.prev_valid = false;
	ta.lookscale = 1;
	ta.lock = 0;
	ta.debug_target = 0;
	ta.debug_friend = 0;
	ta.said_target = ta.said_weapon = ta.said_mode = ta.said_why = -1;
	ta.track_ent = 0;
	ta.fire_target = 0;
	ta.fight_ent = 0;
	ta.rk_state = RK_IDLE;
	ta.rk_ent = 0;
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
	float err;          // degrees from the crosshair to the nearest edge of the (lead-adjusted) body (0 on it)
	float rh, rv;       // angular half-width and half-height of the body, degrees
	float yaw, pitch;   // view angles that point at the aim point (chest, or head behind cover; led)
	float dist;         // eye to the body's centre
	vec3_t center;      // body centre
	vec3_t vel;         // smoothed velocity
	vec3_t lead;        // offset from the body to where the shot should go (0 = no lead)
	float tof;          // flight time of the shot to the aim point
	qbool has_lead;     // lead applied
	qbool on_lead;      // the view ray goes through the led body
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

// The player's velocity, smoothed over a few frames. The CSQC's .velocity is
// derived from network updates (CSQCModel); when it reads zero while the body
// moves (a CSQC that does not derive it), the origin's own motion stands in.
static void track_velocity(int n, prvm_edict_t *ed, const vec3_t org, double now, vec3_t vel_out)
{
	prvm_prog_t *prog = CLVM_prog;
	track_t *t = &ta.tracks[n & (TRACK_MAX - 1)];
	vec3_t v, d;
	float dt = (float)(now - t->time), f;
	VectorCopy(PRVM_clientedictvector(ed, velocity), v);
	VectorSubtract(org, t->org, d);
	if (t->time <= 0 || dt > TRACK_STALE || VectorLength(d) > TRACK_JUMP)
	{
		VectorClear(t->vel);
		if (VectorLength2(v) > 1)
			VectorCopy(v, t->vel);
		VectorCopy(org, t->org);
		t->time = now;
		VectorCopy(t->vel, vel_out);
		return;
	}
	if (dt <= 0)
	{
		VectorCopy(t->vel, vel_out);
		return;
	}
	if (VectorLength2(v) < 1 && VectorLength2(d) > 0.01f)
		VectorScale(d, 1.0f / dt, v);
	f = bound(0.0f, dt / TRACK_SMOOTH, 1.0f);
	VectorLerp(t->vel, f, v, t->vel);
	VectorCopy(org, t->org);
	t->time = now;
	VectorCopy(t->vel, vel_out);
}

// One pass over the players the CSQC is drawing: the one under the crosshair
// (for the old auto-shoot test and the ring) and the nearest one inside the
// cone (for auto-shoot and the helpers), measured to where the shot should
// go when `fire` is a projectile and lead is on. Both must be enemies, alive,
// drawn, in range and in line of sight.
static void scan(const vec3_t eye, const vec3_t fwd, float cone, const touch_wep_fire_t *fire, float latency, int *on_target, int *friend_on, aimtarget_t *near_out)
{
	prvm_prog_t *prog = CLVM_prog;
	int team_game = teamplay_mode();
	float range = max(64.0f, cl_touch_aim_range.value);
	float best_on = 1e9f, gravity = cl.movevars_gravity;
	double now = host.realtime;
	int i;
	*on_target = 0;
	*friend_on = 0;
	memset(near_out, 0, sizeof(*near_out));
	near_out->err = 1e9f;
	for (i = 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		vec3_t org, mins, maxs, bmin, bmax, center, head, dir, vel, lead, lbmin, lbmax;
		float dist, err, t, alpha, hw, tof = 0;
		int n;
		qbool enemy, has_lead = false, on_lead = false;
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

		// where a projectile meets this player, as an offset from the body
		track_velocity(n, ed, org, now, vel);
		VectorClear(lead);
		if (fire)
		{
			vec3_t rel, aim;
			VectorSubtract(center, eye, rel);
			if (TouchWep_AimPoint(fire, rel, vel, latency, gravity, aim, &tof))
			{
				VectorSubtract(aim, rel, lead);
				has_lead = tof > 0;
			}
		}

		// near the crosshair: how far the crosshair is from the body's edge,
		// so a big close body counts as soon as the crosshair nears it, not
		// only once its centre is inside the cone. The aim point is the chest,
		// or the head if the chest is hidden behind cover; led when it flies.
		{
			float yaw, pitch, rh, rv, ey, ep;
			qbool on = n == *on_target;
			vec3_t aimp;
			rh = atan(hw / dist) * (180.0f / M_PI);
			rv = atan(0.5f * (maxs[2] - mins[2]) / dist) * (180.0f / M_PI);
			VectorAdd(center, lead, aimp);
			angles_to(eye, aimp, &yaw, &pitch);
			ey = wrap180(yaw - cl.viewangles[YAW]);
			ep = pitch - cl.viewangles[PITCH];
			if (has_lead)
			{
				VectorAdd(bmin, lead, lbmin);
				VectorAdd(bmax, lead, lbmax);
				on_lead = ray_box(eye, fwd, lbmin, lbmax, &t);
				err = on_lead ? 0 : edge_err(ey, ep, rh, rv);
			}
			else
			{
				on_lead = on;
				err = on ? 0 : edge_err(ey, ep, rh, rv);
			}
			if (err > cone || err > near_out->err)
				continue;
			if (!on && !line_clear(eye, center))
			{
				VectorCopy(center, head);
				head[2] = bmax[2] - 6;
				if (!line_clear(eye, head))
					continue;
				// only the head shows: aim at it, and count only it as the body
				VectorAdd(head, lead, aimp);
				angles_to(eye, aimp, &yaw, &pitch);
				rv = atan(6.0f / dist) * (180.0f / M_PI);
				err = on_lead ? 0 : edge_err(wrap180(yaw - cl.viewangles[YAW]), pitch - cl.viewangles[PITCH], rh, rv);
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
			near_out->dist = dist;
			VectorCopy(center, near_out->center);
			VectorCopy(vel, near_out->vel);
			VectorCopy(lead, near_out->lead);
			near_out->tof = tof;
			near_out->has_lead = has_lead;
			near_out->on_lead = on_lead;
		}
	}
}

// Seconds between this client sending a move and the server acting on it,
// from the moves the server has not acknowledged yet (the scoreboard ping is
// stale under Xonotic's CSQC scoreboard).
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

// Whether weapon `index` has ammo for one primary shot.
static qbool weapon_ready(int index, const touch_wep_t *w, int held)
{
	int ammo = TouchHUD_WeaponAmmo(index), cost = w ? w->cost : 1;
	if (ammo < 0 || cost <= 0)
		return true;
	if (index == held)
	{
		int clip = TouchHUD_HeldClip();
		if (clip >= 0 && clip >= cost)
			return true;
	}
	return ammo >= cost;
}

// The held weapon changed: by our own request, or the player's choice (a
// strip tap, a wheel, a pickup switching by itself).
static void note_held(int held, double now)
{
	if (held == ta.held_last)
		return;
	if (held >= 0 && held == ta.switch_to)
	{
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: switch landed: %s\n", TouchHUD_WeaponName(held));
	}
	else if (ta.held_last >= 0 && held >= 0 && !weapon_ready(ta.held_last, TouchWep_Find(TouchHUD_WeaponName(ta.held_last)), -1))
	{
		// the last one ran dry: the server switched by itself, not the player
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: weapon now %s (%s ran out of ammo)\n", TouchHUD_WeaponName(held), TouchHUD_WeaponName(ta.held_last));
	}
	else
	{
		ta.manual_switch = now;
		if (cl_touch_aim_debug.integer && held >= 0)
			Con_Printf("touch aim: weapon now %s (not asked by auto-switch: no switch for %.0f s)\n", TouchHUD_WeaponName(held), MANUAL_GUARD);
	}
	ta.switch_to = -1;
	ta.held_last = held;
}

// Switch to the best owned weapon for the fight, under the guards.
static void auto_switch(const aimtarget_t *near, int held, const touch_wep_t *wep, double now)
{
	const char *names[72];
	qbool ready[72];
	int index[72], n = 0, i, best, rank, cur_rank;
	qbool cur_ok;
	if (!cl_touch_autoswitch.integer || held < 0 || !wep)
		return;
	if (near->entnum != ta.fight_ent)
	{
		ta.fight_ent = near->entnum;
		ta.fight_since = now;
	}
	if (!near->entnum || now - ta.fight_since < FIGHT_MIN)
		return;
	if (now - ta.manual_switch < MANUAL_GUARD || now - ta.own_switch < OWN_GUARD)
		return;
	if (ta.switch_to >= 0)
	{
		if (now - ta.switch_asked < SWITCH_WAIT)
			return; // wait for the held weapon to change
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: switch to %s did not land, giving up\n", TouchHUD_WeaponName(ta.switch_to));
		ta.switch_to = -1;
	}
	if ((ta.rk_state != RK_IDLE && ta.rk_state != RK_RELEASE) || (in_attack.state & 3) || (in_button3.state & 3))
		return;
	for (i = 0; i < TouchHUD_NumWeapons() && n < 72; i++)
	{
		if (!TouchHUD_WeaponOwned(i))
			continue;
		index[n] = i;
		names[n] = TouchHUD_WeaponName(i);
		ready[n] = weapon_ready(i, TouchWep_Find(names[n]), held);
		n++;
	}
	best = TouchWep_Choose(names, ready, n, near->dist, &rank);
	if (best < 0 || index[best] == held)
		return;
	cur_rank = TouchWep_Rank(wep->netname, near->dist);
	cur_ok = weapon_ready(held, wep, held) && TouchWep_Usable(wep, near->dist);
	if (!TouchWep_ShouldSwitch(cur_rank, cur_ok, rank))
		return;
	Cbuf_AddText(cmd_local, va(vabuf, sizeof(vabuf), "weapon_%s\n", names[best]));
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch aim: switch %s -> %s at %.0f units (%s; rank %d -> %d; %.1f s after the player's last pick)\n", wep->netname, names[best], near->dist,
			!cur_ok ? "current cannot shoot here" : cur_rank < 0 ? "current not on the list" : "clearly better", cur_rank, rank, now - ta.manual_switch);
	ta.switch_to = index[best];
	ta.switch_asked = now;
	ta.own_switch = now;
}

static int projectile_type(prvm_edict_t *ed)
{
	prvm_eval_t *v;
	if (ta.projtype_ofs < 0)
		return -1;
	v = PRVM_EDICTFIELDVALUE(ed, ta.projtype_ofs);
	return ta.projtype_type == ev_float ? (int)v->_float : v->_int;
}

// Rocket projectiles the CSQC has now, so one that appears after a shot
// can be told apart from the ones already flying.
static void rockets_before(void)
{
	prvm_prog_t *prog = CLVM_prog;
	int i;
	ta.rk_before_count = 0;
	if (ta.projtype_ofs < 0)
		return;
	for (i = 1; i < prog->num_edicts && ta.rk_before_count < 16; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		if (!ed->free && projectile_type(ed) == PROJECTILE_ROCKET)
			ta.rk_before[ta.rk_before_count++] = i;
	}
}

// Our rocket: projectiles carry no owner id, so take a rocket that was not
// there at the press and is near the muzzle now.
static int find_own_rocket(const vec3_t eye)
{
	prvm_prog_t *prog = CLVM_prog;
	int i, k, best = 0;
	float best_d = ROCKET_FIND_DIST;
	if (ta.projtype_ofs < 0)
		return 0;
	for (i = 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		float d;
		if (ed->free || projectile_type(ed) != PROJECTILE_ROCKET)
			continue;
		for (k = 0; k < ta.rk_before_count; k++)
			if (ta.rk_before[k] == i)
				break;
		if (k < ta.rk_before_count)
			continue;
		d = VectorDistance(PRVM_clientedictvector(ed, origin), eye);
		if (d < best_d)
		{
			best_d = d;
			best = i;
		}
	}
	return best;
}

// Devastator with steering: press fire once lined up, keep it held while the
// rocket flies (the server guides it toward the view ray), detonate it next
// to the enemy with a one-frame attack2, then release fire long enough for
// the next press to launch again. Sets the fire bits and, while a rocket
// flies, where the view should be pulled.
static void steer_devastator(const vec3_t eye, const aimtarget_t *near, qbool want_fire, qbool manual, float *pull_yaw, float *pull_pitch, qbool *pull)
{
	prvm_prog_t *prog = CLVM_prog;
	double now = host.realtime;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	const touch_wep_t *dev = TouchWep_Find("devastator");
	*pull = false;
	switch (ta.rk_state)
	{
	case RK_IDLE:
		if (want_fire && !manual && near->entnum)
		{
			rockets_before();
			ta.rk_ent = 0;
			ta.rk_target = near->entnum;
			VectorCopy(near->center, ta.rk_target_org);
			VectorCopy(near->vel, ta.rk_target_vel);
			rk_set(RK_LAUNCH, va(vabuf, sizeof(vabuf), "lined up on player %d at %.0f units", near->entnum, near->dist));
			set_firing(true, false, near->entnum);
		}
		else
			set_firing(false, false, 0);
		return;
	case RK_LAUNCH:
		set_firing(true, false, ta.rk_target);
		ta.rk_ent = find_own_rocket(eye);
		if (ta.rk_ent)
		{
			VectorCopy(PRVM_clientedictvector(PRVM_EDICT_NUM(ta.rk_ent), origin), ta.rk_org);
			VectorClear(ta.rk_vel);
			ta.rk_seen = now;
			ta.rk_closest = 1e9f;
			rk_set(RK_FLY, va(vabuf, sizeof(vabuf), "tracking own rocket, edict %d", ta.rk_ent));
		}
		else if (now - ta.rk_time > ROCKET_FIND_TIME)
			rk_set(RK_RELEASE, "no rocket of ours appeared");
		return;
	case RK_FLY:
	{
		prvm_edict_t *ed = ta.rk_ent < prog->num_edicts ? PRVM_EDICT_NUM(ta.rk_ent) : NULL;
		vec3_t org, d, rel_v, me;
		float sp;
		if (!ed || ed->free || projectile_type(ed) != PROJECTILE_ROCKET)
		{
			rk_set(RK_RELEASE, va(vabuf, sizeof(vabuf), "rocket gone: hit something; it came within %.0f units of the target", ta.rk_closest));
			set_firing(false, false, 0);
			return;
		}
		VectorCopy(PRVM_clientedictvector(ed, origin), org);
		VectorSubtract(org, ta.rk_org, d);
		if (VectorLength2(d) > 0.01f && now > ta.rk_seen)
		{
			float f = bound(0.0f, (float)(now - ta.rk_seen) / 0.05f, 1.0f);
			vec3_t v;
			VectorScale(d, 1.0f / (float)(now - ta.rk_seen), v);
			VectorLerp(ta.rk_vel, f, v, ta.rk_vel);
			VectorCopy(org, ta.rk_org);
			ta.rk_seen = now;
		}
		if (now - ta.rk_time > ROCKET_MAX_FLIGHT)
		{
			rk_set(RK_RELEASE, "flight too long");
			set_firing(false, false, 0);
			return;
		}
		// follow the target while it is still the one in view
		if (near->entnum == ta.rk_target)
		{
			VectorCopy(near->center, ta.rk_target_org);
			VectorCopy(near->vel, ta.rk_target_vel);
		}
		else
			VectorMA(ta.rk_target_org, dt, ta.rk_target_vel, ta.rk_target_org);
		VectorSubtract(ta.rk_vel, ta.rk_target_vel, rel_v);
		ta.rk_closest = min(ta.rk_closest, VectorDistance(ta.rk_org, ta.rk_target_org));
		Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, me);
		if (dev && TouchWep_Detonate(ta.rk_org, rel_v, ta.rk_target_org, me, dev->pri.splash))
		{
			rk_set(RK_DETONATE, va(vabuf, sizeof(vabuf), "rocket %.0f units from player %d", VectorDistance(ta.rk_org, ta.rk_target_org), ta.rk_target));
			// attack stays held this frame: with the rocket already out it
			// launches nothing, and attack2 sets off every rocket of ours
			set_firing(true, true, ta.rk_target);
			return;
		}
		set_firing(true, false, ta.rk_target);
		// keep the view on where the rocket will meet the target: the server
		// steers it toward a point on the view ray ahead of it
		sp = VectorLength(ta.rk_vel);
		if (sp > 100)
		{
			vec3_t rel, aim;
			float t = 0;
			VectorSubtract(ta.rk_target_org, ta.rk_org, rel);
			if (!TouchWep_InterceptTime(rel, ta.rk_target_vel, max(sp, dev ? dev->pri.speed : 1000), TW_LEAD_MAX_TIME, &t))
				t = 0;
			VectorMA(ta.rk_target_org, t, ta.rk_target_vel, aim);
			angles_to(eye, aim, pull_yaw, pull_pitch);
			*pull = true;
		}
		return;
	}
	case RK_DETONATE:
		set_firing(false, false, 0);
		rk_set(RK_RELEASE, "attack2 sent");
		return;
	case RK_RELEASE:
	default:
		set_firing(false, false, 0);
		if (now - ta.rk_time >= ROCKET_RELEASE)
			rk_set(RK_IDLE, "fire released");
		return;
	}
}

void TouchAim_Frame(void)
{
	double now = host.realtime;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	qbool autoshoot, assist, aim_touch, manual, pull_target_set = false;
	float friction, strength, cone, turn_yaw, turn_pitch, turn_mag, radius, pull_yaw = 0, pull_pitch = 0;
	vec3_t eye, fwd;
	int on_target, friend_on, on_shoot, held, mode = TW_MODE_PRIMARY, why = TW_WHY_DEFAULT;
	const touch_wep_t *wep;
	const touch_wep_fire_t *fire = NULL;
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
		Con_Printf("touch aim: active (teamplay %d, my colours %d, %s HUD, %d weapons bound)\n", teamplay_mode(),
			cl.scores[cl.realplayerentity - 1].colors, TouchHUD_Active() ? "engine" : "CSQC", TouchHUD_NumWeapons());
	ta.was_allowed = true;
	ta.blocked_said = NULL;

	autoshoot = cl_touch_autoshoot.integer != 0;
	assist = cl_touch_aimassist.integer != 0;
	friction = assist ? bound(0.0f, cl_touch_aimassist_friction.value, FRICTION_MAX) : 0;
	strength = assist ? bound(0.0f, cl_touch_aimassist_strength.value, STRENGTH_MAX) : 0;
	cone = bound(1.0f, cl_touch_aimassist_cone.value, CONE_MAX);
	radius = bound(0.0f, cl_touch_autoshoot_radius.value, RADIUS_MAX);

	// the weapon in hand, and whether the player picked it
	held = TouchHUD_HeldWeapon();
	wep = held >= 0 ? TouchWep_Find(TouchHUD_WeaponName(held)) : NULL;
	note_held(held, now);

	if (!autoshoot && friction <= 0 && strength <= 0 && !cl_touch_autoswitch.integer)
	{
		reset();
		publish();
		return;
	}
	// the player's own FIRE or ALT: auto-shoot adds nothing on top of it
	manual = (in_attack.state & 3) || (in_button3.state & 3);

	// How far the player turned since the last frame: the touch drag (engine
	// HUD this frame, port CSQC last frame) is the only thing that turns the
	// view in touch mode, so this is the player's own input. The pull's own
	// turn last frame is not in it: prev_angles is taken after the pull.
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

	// the fire whose flight the aim leads: last decision's mode
	if (wep && cl_touch_autoshoot_lead.integer)
	{
		fire = ta.last_mode == TW_MODE_SECONDARY && wep->sec.kind != TW_KIND_NONE ? &wep->sec : &wep->pri;
		if (fire->kind != TW_KIND_PROJ && fire->kind != TW_KIND_BALLISTIC)
			fire = NULL;
	}

	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	AngleVectors(cl.viewangles, fwd, NULL, NULL);
	scan(eye, fwd, cone, fire, fire ? latency_estimate() : 0, &on_target, &friend_on, &near);

	if (cl_touch_aim_debug.integer && on_target != ta.debug_target)
	{
		if (on_target)
			Con_Printf("touch aim: crosshair on player %d (%s)\n", on_target, player_name(on_target));
		ta.debug_target = on_target;
	}
	if (cl_touch_aim_debug.integer >= 2 && friend_on != ta.debug_friend)
	{
		if (friend_on)
			Con_Printf("touch aim: crosshair on teammate %d (%s), not firing\n", friend_on, player_name(friend_on));
		ta.debug_friend = friend_on;
	}

	// Who to shoot. With no radius and no lead: the crosshair on the body,
	// as it always was. Otherwise the nearest enemy, when the crosshair is
	// within the radius of where the shot meets it, narrowed to what this
	// weapon's spread or splash covers; a led shot also needs a clear path
	// to the lead point.
	on_shoot = 0;
	if (autoshoot)
	{
		if (radius <= 0 && !near.has_lead)
			on_shoot = on_target;
		else if (near.entnum)
		{
			const touch_wep_fire_t *f = wep ? (ta.last_mode == TW_MODE_SECONDARY && wep->sec.kind != TW_KIND_NONE ? &wep->sec : &wep->pri) : NULL;
			float window = f ? TouchWep_FireWindow(f, near.dist, radius) : radius;
			if (near.on_lead || near.err <= window)
			{
				vec3_t lp;
				VectorAdd(near.center, near.lead, lp);
				if (!near.has_lead || (f && f->kind == TW_KIND_BALLISTIC) || line_clear(eye, lp))
					on_shoot = near.entnum;
			}
		}
	}

	// primary or secondary for this weapon, distance and ammo
	if (on_shoot && wep)
	{
		touch_wep_situation_t s;
		float dist = near.entnum == on_shoot ? near.dist : 0;
		if (!dist)
		{
			const track_t *t = &ta.tracks[on_shoot & (TRACK_MAX - 1)];
			dist = VectorDistance(t->org, eye);
		}
		s.dist = dist;
		s.impact_dist = dist;
		if (near.entnum == on_shoot && near.has_lead)
		{
			// where the shot meets the target: the lead point, or for a lob
			// (whose lead point only gives the launch direction) the target
			// where it will be
			vec3_t ip;
			const touch_wep_fire_t *f = ta.last_mode == TW_MODE_SECONDARY && wep->sec.kind != TW_KIND_NONE ? &wep->sec : &wep->pri;
			if (f->kind == TW_KIND_BALLISTIC)
				VectorMA(near.center, near.tof, near.vel, ip);
			else
				VectorAdd(near.center, near.lead, ip);
			s.impact_dist = VectorDistance(ip, eye);
		}
		s.pri_ready = weapon_ready(held, wep, held);
		s.alt_ready = TouchHUD_AltFireState() == ALTFIRE_READY;
		s.use_alt = cl_touch_autoshoot_alt.integer != 0;
		mode = TouchWep_FireMode(wep, &s, &why);
		if (mode == TW_MODE_NONE && !cl_touch_autoshoot_safe.integer && why != TW_WHY_NO_AMMO)
			mode = TW_MODE_PRIMARY; // the player turned the safety rules off
		if (mode == TW_MODE_NONE && why == TW_WHY_NO_AMMO && !cl_touch_autoswitch.integer)
			mode = TW_MODE_PRIMARY; // fire on empty: the server switches to another weapon
		if (cl_touch_autoshoot_weapons.integer == 1)
		{
			const touch_wep_fire_t *f = mode == TW_MODE_SECONDARY ? &wep->sec : &wep->pri;
			if (f->kind != TW_KIND_HITSCAN && f->kind != TW_KIND_BEAM && f->kind != TW_KIND_MELEE)
				mode = TW_MODE_NONE; // the player asked for instant-hit only
		}
		if (mode != TW_MODE_NONE)
			ta.last_mode = mode;
	}
	else if (on_shoot && cl_touch_autoshoot_weapons.integer == 1 && cl_touch_weapon_class.integer == 2)
		mode = TW_MODE_NONE; // unknown weapon: the port's CSQC says it is a projectile weapon
	if (on_shoot && cl_touch_autoshoot_aimonly.integer && !aim_touch)
		on_shoot = 0;
	if (mode == TW_MODE_NONE)
		on_shoot = 0;

	if (cl_touch_aim_debug.integer && (near.entnum != ta.said_target || held != ta.said_weapon || (on_shoot ? mode : -1) != ta.said_mode || why != ta.said_why))
	{
		if (near.entnum)
			Con_Printf("touch aim: target %d (%s) at %.0f units, %.1f deg off; weapon %s: %s (%s)%s\n", near.entnum, player_name(near.entnum), near.dist, near.err,
				held >= 0 ? TouchHUD_WeaponName(held) : "?", !on_shoot ? "hold" : mode == TW_MODE_SECONDARY ? "secondary" : "primary",
				wep ? touch_wep_why[why] : "unknown weapon",
				near.has_lead ? va(vabuf, sizeof(vabuf), ", lead %.2f s (%.0f units)", near.tof, VectorLength(near.lead)) : "");
		ta.said_target = near.entnum;
		ta.said_weapon = held;
		ta.said_mode = on_shoot ? mode : -1;
		ta.said_why = why;
	}

	ta.lock = !assist && !autoshoot ? 0 : ((on_target || (near.entnum && near.on_lead)) ? 2 : (near.entnum ? 1 : 0));

	// switch first: no rocket is launched while a switch is on its way
	auto_switch(&near, held, wep, now);

	if (autoshoot && wep && (wep->flags & TW_GUIDED) && cl_touch_autoshoot_steer.integer)
	{
		// Devastator: reaction delay as for any weapon, then the rocket cycle
		qbool want = false;
		if (on_shoot)
		{
			if (!ta.on_since)
				ta.on_since = now;
			ta.last_on = now;
			want = now - ta.on_since >= bound(0.0f, cl_touch_autoshoot_delay.value, 0.5f) && ta.switch_to < 0;
		}
		else
			ta.on_since = 0;
		steer_devastator(eye, &near, want, manual, &pull_yaw, &pull_pitch, &pull_target_set);
		ta.fire_target = ta.rk_state != RK_IDLE ? ta.rk_target : 0;
	}
	else
	{
		// auto-shoot: fire after the reaction delay, keep firing a moment
		// after the crosshair slips off so a jittery track does not stutter.
		// The player's own press decides alone (attack2 together with a fresh
		// attack would, for one, detonate a rocket at the muzzle).
		qbool on = ta.firing, on2 = ta.firing2;
		int target = ta.fire_target;
		if (ta.rk_state != RK_IDLE)
			rk_set(RK_IDLE, "not steering");
		if (autoshoot && on_shoot)
		{
			if (!ta.on_since)
				ta.on_since = now;
			ta.last_on = now;
			if (now - ta.on_since >= bound(0.0f, cl_touch_autoshoot_delay.value, 0.5f))
			{
				on = mode != TW_MODE_SECONDARY;
				on2 = mode == TW_MODE_SECONDARY;
				target = on_shoot;
			}
		}
		else
		{
			ta.on_since = 0;
			if (!autoshoot || now - ta.last_on >= bound(0.0f, cl_touch_autoshoot_release.value, 0.5f))
			{
				on = on2 = false;
				target = 0;
			}
		}
		if (manual)
			on = on2 = false;
		set_firing(on, on2, target);
		ta.fire_target = on || on2 ? target : 0;
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

	// magnetism: with a finger on the aim control (the look area or FIRE)
	// while the player aims -- turning, moving, or within INPUT_HOLD of
	// either, so a finger resting on the screen never aims by itself -- and
	// also while a guided rocket flies, or auto-shoot is firing at that
	// target (cl_touch_aimassist_track). Never against a turn that is heading away from the target, and
	// capped per second so it can never snap. Two parts: follow a share of
	// the target's own motion across the view (it runs, or the player
	// strafes), and close the remaining gap at a rate that grows with it, so
	// a thumb that stops a few degrees short or long still lands on the body.
	if (turn_mag / dt > INPUT_MIN_RATE || cl.cmd.forwardmove != 0 || cl.cmd.sidemove != 0)
		ta.last_input = now;
	if (pull_target_set && near.entnum != ta.rk_target)
	{
		// a guided rocket's target out of the cone: pull toward its meeting point anyway
		near.entnum = ta.rk_target;
		near.err = 1;
		near.rh = near.rv = 0.5f;
	}
	if (pull_target_set)
	{
		near.yaw = pull_yaw;
		near.pitch = pull_pitch;
	}
	if (strength > 0 && near.entnum
		&& ((aim_touch && now - ta.last_input < INPUT_HOLD)
			|| ta.rk_state != RK_IDLE
			|| (cl_touch_aimassist_track.integer && ta.fire_target && ta.fire_target == near.entnum && (ta.firing || ta.firing2))))
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
