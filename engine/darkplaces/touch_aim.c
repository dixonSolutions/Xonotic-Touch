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
#include "touch_hook.h"
#include "touch_tactics.h"
#include "touch_assist.h"
extern cvar_t cl_touch_esp_enabled;

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
	"narrowed to what the weapon's spread or splash can cover at that distance; 0 = only on the body (at most 15)"};
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
cvar_t cl_touch_autoshoot_air = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_air", "1",
	"Auto-shoot leads an enemy in the air along its fall under gravity, and aims where it lands when the shot would arrive after the landing"};
cvar_t cl_touch_autoshoot_predict = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_predict", "1",
	"Auto-shoot adds the time the screen lags the server to the lead, stops a lead at a wall the enemy will run into, and leads instant-hit weapons by the part of a high ping the server's antilag does not cover (beyond 0.4 s)"};
cvar_t cl_touch_autoshoot_feet = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_feet", "1",
	"Splash weapons (Devastator, Mortar, Electro, Crylink, RPC, Fireball) aim at the floor at a grounded enemy's feet, so a near miss still explodes beside them"};
cvar_t cl_touch_autoshoot_group = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_group", "1",
	"Splash weapons aim where the blast catches the most visible enemies (never teammates), and a Devastator rocket is set off where it does the most damage to all of them"};
cvar_t cl_touch_autoswitch_smart = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoswitch_smart", "1",
	"Auto-switch picks by expected damage for the situation -- distance, how the enemy moves (standing, strafing, dodging, in the air), enemies grouped for splash, the time a switch costs -- instead of a fixed list per distance"};
cvar_t cl_touch_autoswitch_combo = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoswitch_combo", "1",
	"Auto-switch: once your blast has thrown the enemy into the air, switch at once to the instant-hit weapon that finishes them"};
cvar_t cl_touch_autoshoot_clear = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_clear", "1",
	"A steered Devastator rocket is flown clear of walls, floors, ledges and pillars on its way (its path is checked ahead the way the server turns it), "
	"aimed at the body until close and at the floor beside a grounded enemy only for the final approach; an enemy that goes out of sight is steered to where it was last seen"};
cvar_t cl_touch_autoshoot_volley = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_volley", "1",
	"Devastator: launch the next rocket while one is still flying, once that one is on a line that hits by itself (it then flies straight); "
	"the one remote detonation, which sets off every rocket of yours, is timed for the most damage, and no weapon switch happens while a rocket that still needs steering is out"};
cvar_t cl_touch_autoshoot_head = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_head", "1",
	"Aim at the head with a weapon that pays a headshot bonus, where the smaller target still does more damage on average (distance, spread, how the enemy moves). "
	"In stock Xonotic no weapon does: only the Rifle can have a bonus and the stock balance sets it to 0 (see cl_touch_autoshoot_head_rifle); the Vortex has none"};
cvar_t cl_touch_autoshoot_head_rifle = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_head_rifle", "0",
	"The server's Rifle headshot multiplier (g_balance_rifle_primary_headshot_multiplier), which the client cannot read: 0 in stock Xonotic, 1.25 with the Nexuiz 2.5 balance"};
cvar_t cl_touch_autoswitch_lastresort = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoswitch_lastresort", "1",
	"Auto-switch never picks the Blaster (or another last-resort weapon) while another owned weapon can shoot, and leaves it at once when the server switched to it on an empty weapon"};
cvar_t cl_touch_aimassist_hysteresis = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_hysteresis", "2",
	"Aim helpers: the enemy already targeted stays the target unless another is this many degrees nearer the crosshair (0 = always the nearest)"};
cvar_t cl_touch_aim_mode = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_mode", "1",
	"Aim helpers, who may be the target: 0 = Radius (within cl_touch_aimassist_cone of the crosshair), 1 = Field of view (anyone on screen), "
	"2 = Full 360 (offline matches only; elsewhere it works as 1). Always only enemies in sight"};
cvar_t cl_touch_aimassist_priority = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_priority", "1",
	"Aim helpers, several enemies in the cone: nearest the crosshair first, then the one the weapon in hand does most damage to, one facing you up close, "
	"one you have just hurt, a group for a splash weapon; one running out of reach of a projectile counts less. A new target is kept at least half a second (0 = nearest the crosshair only)"};
cvar_t cl_touch_practice_seethrough = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_seethrough", "1",
	"Offline practice, only in a match against bots on this machine with nobody else connected: mark enemies behind walls, pre-aim where they will appear, "
	"fire splash weapons that reach them round cover and steer rockets to them. Never anywhere else, whatever this says"};
cvar_t cl_touch_practice_360 = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_360", "1",
	"Offline practice (as above): with Targets set to Full 360 (cl_touch_aim_mode 2), enemies all around may be the target, with a marker at the screen edge. Anywhere else Full 360 works as Field of view"};
cvar_t cl_touch_practice_instant = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_instant", "0",
	"Offline practice (as above): the aim pull moves onto the target at once instead of at cl_touch_aimassist_maxrate. Never anywhere else"};
cvar_t cl_touch_practice_burst = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_burst", "0",
	"Offline practice (as above): a splash shot at an enemy behind cover aims where its simulated flight bursts next to the enemy (over the cover, round it, on the floor by it), "
	"and fires only while the shot from the view as it is now would burst there (0, the default: the straight line to the enemy must end near it). Never anywhere else"};
cvar_t cl_touch_aimassist_lastseen = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_lastseen", "1",
	"Aim pull, during a fight: when the enemy goes out of sight, keep the crosshair where it was last seen, moving on its last seen course for a second, for up to 3 seconds, while that point is in clear view -- never where it really is behind cover, and never on a wall it went behind"};
cvar_t cl_touch_autoshoot_combo = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_combo", "1",
	"Auto-shoot uses two-step fire sequences that do more damage: Electro orbs lobbed at an enemy standing or coming at you, then the bolt that sets them off (with cl_touch_autoshoot_alt)"};
cvar_t cl_touch_autoshoot_teamsafe = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_teamsafe", "1",
	"Team games: auto-shoot holds fire while a teammate is on the line of the shot or would be inside its blast, and a rocket is not set off next to one"};
cvar_t cl_touch_aimassist = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist", "1",
	"Touch controls: aim helpers -- the look drag slows down over an enemy (cl_touch_aimassist_friction) and the view is pulled gently toward one while a finger is on the aim control (cl_touch_aimassist_strength). "
	"Only in touch mode; never active with a keyboard attached"};
cvar_t cl_touch_aimassist_friction = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_friction", "0.45",
	"Aim slowdown: fraction of look speed removed while the crosshair is on an enemy, fading out to the edge of cl_touch_aimassist_cone (0 = off, at most 0.8)"};
cvar_t cl_touch_aimassist_strength = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_strength", "0.6",
	"Aim pull toward the nearest visible enemy inside the cone (where a projectile will meet it) while a finger is on the aim control, while auto-shoot fires at it and while a Devastator rocket flies: "
	"follows strength x 2 of its motion across the view and closes the gap at strength x 10 per second, or strength x your own turn speed, whichever is faster (0 = off, at most 1)"};
cvar_t cl_touch_aimassist_track = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_track", "1",
	"Aim pull: follow a fight with the thumb at rest -- keep pulling toward the enemy auto-shoot fired at for a moment after each shot, and let a projectile's lead slide rather than jump when the enemy reverses (0 = pull only while a finger is on the aim control and aiming)"};
cvar_t cl_touch_aimassist_maxrate = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_maxrate", "90",
	"Aim pull: hard cap on the pull, degrees per second (at most 180); it never snaps"};
cvar_t cl_touch_aimassist_cone = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aimassist_cone", "8",
	"Aim helpers: half-angle in degrees around the crosshair within which an enemy counts as near (1 - 60: up to the whole view)"};
cvar_t cl_touch_autoshoot_weapons = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_weapons", "0",
	"Auto-shoot weapons: 0 = every weapon, 1 = only instant-hit weapons (judged from the weapon in hand; for a weapon the engine does not know, from _cl_touch_weapon_class)"};
cvar_t cl_touch_autoshoot_aimonly = {CF_CLIENT | CF_ARCHIVE, "cl_touch_autoshoot_aimonly", "0",
	"Auto-shoot only while a finger is on the aim control (the look area or FIRE); 0 = also with no finger down"};
cvar_t cl_touch_aim_ring = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_ring", "1",
	"Touch HUD: draw a ring at the crosshair while an enemy is within the aim-assist cone, a red one while the crosshair is on an enemy (with aim assist or auto-shoot on)"};
cvar_t cl_touch_aim_range = {CF_CLIENT | CF_ARCHIVE, "cl_touch_aim_range", "3000",
	"Auto-shoot and aim helpers ignore enemies farther away than this many units; 0 = no limit (still only enemies in sight)"};
cvar_t cl_touch_aim_dumptracks = {CF_CLIENT, "cl_touch_aim_dumptracks", "0",
	"Testing: print every drawn player's position and velocity each frame (XTRK lines), also in demo playback, for the lead evaluator"};
cvar_t cl_touch_aim_debug = {CF_CLIENT, "cl_touch_aim_debug", "0",
	"Print auto-shoot and aim-assist decisions to the console: target, weapon, fire mode and why, lead, switches, rockets (2 = also teammates under the crosshair)"};
cvar_t cl_touch_loser = {CF_CLIENT | CF_ARCHIVE, "cl_touch_loser", "0",
	"Development: enable all touch auto-shoot, aim assist, ESP and wallhack helpers on a desktop machine (no touch screen). Off by default"};

// Engine -> CSQC: whether auto-shoot is firing (the port's FIRE button glows),
// and the look multiplier the port's touch_look.qc applies (aim friction).
cvar_t cl_touch_autoshoot_active = {CF_CLIENT, "_cl_touch_autoshoot_active", "0",
	"Set by the engine: what touch auto-shoot is holding, 1 primary fire, 2 secondary, 3 both, 0 nothing"};
cvar_t cl_touch_aim_lock = {CF_CLIENT, "_cl_touch_aim_lock", "0",
	"Set by the engine: 2 while the crosshair is on an enemy, 1 while one is within the aim-assist cone, else 0 (the touch HUD draws the crosshair ring from it)"};
// CSQC -> engine: class of the weapon in hand: 0 unknown, 1 instant-hit, 2 other.
cvar_t cl_touch_weapon_class = {CF_CLIENT, "_cl_touch_weapon_class", "0",
	"Set by the port's CSQC: 0 unknown, 1 instant-hit weapon in hand, 2 projectile or other"};
cvar_t cl_touch_aim_disengage = {CF_CLIENT, "_cl_touch_aim_disengage", "0",
	"Set by the engine: 1 while the weapons left are not worth a fight (the one in hand is empty and only the Blaster or nothing has ammo): keep away and dodge"};
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
#define CONE_MAX       60.0f  // the whole view
#define RADIUS_MAX     15.0f
// Pull toward the target: the gap (degrees) closes at strength * SETTLE_GAIN
// per second, 6/s at the default 0.6, so a 5 degree miss is mostly gone in a
// fifth of a second; and strength * FOLLOW_SCALE of the target's own motion
// across the view is followed (all of it from strength 0.5).
#define SETTLE_GAIN    10.0f
#define SETTLE_GAIN_ROCKET 25.0f
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
#define AWAY_NOISE_RATE 15.0f // slower than this, or sideways, a turn does not stop the pull
#define LEAVE_RATE     30.0f  // a drag leaving a target this fast is not slowed down
#define FLICK_RATE     120.0f // a drag this fast is a new look: the old target loses its hold
#define FLICK_KEEP     1.0f   // seconds after one in which the fight follows the new view
// Combat lock (cl_touch_aimassist_track): after auto-shoot has fired at a
// target, the pull keeps tracking that same target for LOCK_HOLD seconds
// with the thumb at rest, out to LOCK_CONE_EXTRA degrees beyond the cone,
// so a reversing strafe that takes the crosshair off the shot for a moment
// does not end the fight. It never starts without auto-shoot firing.
#define LOCK_HOLD       1.0f
#define LOCK_CONE_EXTRA 6.0f
#define ENGAGE_HOLD     3.0f
// A per-frame view change larger than this is a teleport or respawn, not a turn.
#define TURN_SPIKE_DEG 60.0f
#define ANTILAG_MAX    0.4f   // g_antilag rewinds at most this much ping (ANTILAG_LATENCY)
#define ORB_HOLD       0.55f  // attack2 held this long lobs the Electro's three orbs
// Target velocity: smoothed over about this many seconds; forgotten when a
// player was not seen for TRACK_STALE seconds or jumps TRACK_JUMP units.
#define TRACK_SMOOTH   0.08f
#define TRACK_STALE    0.5f
#define TRACK_JUMP     300.0f
#define TRACK_MAX      256
#define TRACK_MAX_SPEED 2000.0f
// Auto-switch guards (seconds): the enemy must be the target for FIGHT_MIN
// before a switch; none for MANUAL_GUARD after the player picked a weapon,
// nor within OWN_GUARD of the last automatic one; a request that has not
// landed after SWITCH_WAIT is given up.
#define FIGHT_MIN      0.25f
#define MANUAL_GUARD   4.0f
#define OWN_GUARD      2.0f
#define OWN_GUARD_SMART 0.6f // cl_touch_autoswitch_smart: switch cost is in the choice itself
#define CLASS_SETTLE   0.3f  // a motion class must hold this long before a switch on it
#define SWITCH_WAIT    1.5f
// Devastator: how far from the eye a fresh rocket may be to be ours, how
// long to wait for it to appear (refire 1.1 s plus the trip to the server),
// the longest flight followed, and how long fire stays released after a
// rocket so the next press launches one (and attack2 is up again).
#define ROCKET_FIND_DIST  300.0f
#define ROCKET_FIND_TIME  1.6f
#define ROCKET_DEAD_RECKON 0.25f // no rocket seen this long after the press: assume it flies
#define ROCKET_MAX_FLIGHT 4.0f
#define ROCKET_RELEASE    0.06f
#define DETONATE_HOLD     0.05f // attack2 held at least this long for a detonation
#define ROCKET_REFIRE     1.1f  // g_balance_devastator_refire
#define REFIRE_EARLY      0.1f  // the next press goes this much before the refire is up, as seen here
#define ROCKET_SPEED      1300.0f // g_balance_devastator_speed, and _speedaccel
#define ROCKET_GUIDE_RATE 90.0f // g_balance_devastator_guiderate (deg/s)
#define ROCKET_GUIDE_GOAL 512.0f // g_balance_devastator_guidegoal
// path check (cl_touch_autoshoot_clear): simulated in steps of PLAN_DT for
// at most PLAN_STEPS, redone every PLAN_EVERY seconds
#define PLAN_DT           0.05f
#define PLAN_STEPS        40
#define PLAN_EVERY        0.1
#define PLAN_SETTLE       0.08  // seconds of tracking before the rocket's path is simulated
#define ROCKET_FLOOR_FINAL 400.0f // the floor beside a grounded target is aimed at only this close
#define LAST_SEEN_EXTRAP  0.5f
#define HEAD_WAIT         0.3f  // longest a head-aimed shot waits for the crosshair to reach the head  // a target out of sight is followed on its last velocity this long
#define FREE_MAX          3     // own rockets flying straight (cl_touch_autoshoot_volley)
#define VOLLEY_SURE       0.35f // a moving target: only a rocket this close to a direct hit is let go
#define PROJECTILE_ROCKET 2 // csqcprojectile_type of a Devastator rocket
#define ROCKET_MODEL      "models/rocket.md3"

#define ALTFIRE_READY     2 // TOUCH_ALTFIRE_READY (touch_altfire.h)

// Devastator states.
enum { RK_IDLE, RK_LAUNCH, RK_FLY, RK_DETONATE, RK_RELEASE, RK_VOLLEY };
static const char *const rk_state_name[] = { "idle", "launch", "fly", "detonate", "release", "volley" };

// an own rocket no longer guided (the next one took over): it flies straight
typedef struct freerk_s
{
	int ent;
	vec3_t org, vel;
	double seen;
	int target;
	vec3_t tgt_org, tgt_vel; // the target as last seen
	qbool good;      // its straight line hits the target, or the floor beside it
	qbool certain;   // a direct hit within a moment
	float t_hit;     // seconds until it gets there
} freerk_t;

static char vabuf[256];

typedef struct track_s
{
	vec3_t org, vel;
	double time;
	vec3_t bmin, bmax;   // last seen box (cl_touch_aim_debug 3: miss distance)
	vec3_t lead;         // motion part of the lead, slewed (cl_touch_aimassist_track)
	double lead_time;    // 0 = start from the unslewed lead
	// motion class (TW_CLASS_*), and what it is made of
	int cls;
	qbool airborne;
	float t_land;        // seconds until it lands (airborne)
	float floor_z;       // floor under it
	int lat_sign;        // side of its last clear sideways motion
	double rev[4];       // times of its last strafe reversals, newest first
	double cls_since;    // when the class last changed
	double air_checked;  // when the ground under it was last traced
	// for choosing among several (cl_touch_aimassist_priority)
	double seen_since;   // in sight without a break since then
	float range_rate;    // units/s away from the player (< 0 closing in)
	qbool facing;        // looking toward the player
	float dealt;         // damage auto-shoot's hits did to it lately (the hit-sound stat)
	double dealt_time;
	touch_wep_ghost_t ghost; // last seen (cl_touch_aimassist_lastseen)
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
	int lock_ent;        // combat lock: last target fired at, and until when
	double lock_until;
	double lock_fired;   // last time auto-shoot fired at lock_ent
	double flick_time;   // last fast drag of the player's own
	double orbs_time, orbs_until;
	int ghost_said;      // the enemy held by its last seen point (debug)
	qbool said_hidden;
	qbool rk_over_valid; // offline practice: the point over cover a rocket goes by
	vec3_t rk_over; // Electro combo: when orbs were last lobbed, and the hold for them
	double target_since; // when the current target (track_ent) was taken
	int target_switches; // for cl_touch_aim_debug
	int mate_said;       // teammate that last held fire (debug)
	double shootable_since; // first frame of the current chance to shoot (fire latency, debug)
	int shootable_frames;
	int stat_dealt;      // index of STAT_HITSOUND_DAMAGE_DEALT_TOTAL, -1 none
	int dealt_last;
	qbool dropped;       // the server switched to a last-resort weapon off an empty one
	float fight_dist;    // distance of the last target, for a switch without one
	double cost_time;    // frame cost (cl_touch_aim_debug 4)
	int cost_frames, cost_players;
	double cost_since;
	// groups: the blast point chosen last frame (relative to nothing: world)
	qbool group_valid;
	vec3_t group_point;
	int group_count, group_said;
	double launcher_fired; // last time a splash launcher fired at the target (combo)

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
	double rk_time, rk_seen, rk_launch;
	float rk_closest;    // closest the rocket came to the target (debug)
	vec3_t rk_org, rk_vel, rk_target_org, rk_target_vel;
	int rk_before[16], rk_before_count;
	float head_mult;     // headshot bonus of the fire in hand (cl_touch_autoshoot_head), 0 none
	float head_spread;   // and its spread (radians)
	double head_wait;    // since when a head-aimed shot waits for the crosshair
	double rk_tseen;     // last time the target was in sight
	int rk_lift_ent;     // launch path blocked: aim at this target's head until rk_lift_until
	double rk_lift_until;
	double rk_plan_next; // path check (cl_touch_autoshoot_clear)
	qbool rk_way_on;     // steering via a waypoint around something in the way
	vec3_t rk_way;
	int rk_way_said;
	double rk_fired;     // when the press that launched the guided rocket went out
	double rk_launch_seen; // when the guided rocket was first tracked
	freerk_t freerk[FREE_MAX];
	int num_free;
	// from the loaded CSQC
	int teamplay_ofs, teamplay_type;
	int projtype_ofs, projtype_type;
	qbool projtype_is_cnt;
} ta;

#define SHOT_MAX 32
#define SHOT_SEEN_MAX 16384
typedef struct shot_s { int ent, type, target; qbool hidden; float closest; double born; vec3_t last; } shot_t;
static qbool shot_hidden; // the shots now going out were fired at a target behind cover (debug)
static double shot_hidden_fired; // last frame auto-shoot decided to fire at a target behind cover
static shot_t shots[SHOT_MAX];
static int num_shots;
static unsigned char shot_seen[SHOT_SEEN_MAX];

void TouchAim_Init(void)
{
	TouchAssist_Init();
	Cvar_RegisterVariable(&cl_touch_autoshoot);
	Cvar_RegisterVariable(&cl_touch_autoshoot_delay);
	Cvar_RegisterVariable(&cl_touch_autoshoot_release);
	Cvar_RegisterVariable(&cl_touch_autoshoot_radius);
	Cvar_RegisterVariable(&cl_touch_autoshoot_alt);
	Cvar_RegisterVariable(&cl_touch_autoshoot_lead);
	Cvar_RegisterVariable(&cl_touch_autoshoot_steer);
	Cvar_RegisterVariable(&cl_touch_autoshoot_safe);
	Cvar_RegisterVariable(&cl_touch_autoswitch);
	Cvar_RegisterVariable(&cl_touch_autoshoot_air);
	Cvar_RegisterVariable(&cl_touch_autoshoot_predict);
	Cvar_RegisterVariable(&cl_touch_autoshoot_feet);
	Cvar_RegisterVariable(&cl_touch_autoshoot_group);
	Cvar_RegisterVariable(&cl_touch_autoswitch_smart);
	Cvar_RegisterVariable(&cl_touch_autoswitch_combo);
	Cvar_RegisterVariable(&cl_touch_autoswitch_lastresort);
	Cvar_RegisterVariable(&cl_touch_autoshoot_clear);
	Cvar_RegisterVariable(&cl_touch_autoshoot_head);
	Cvar_RegisterVariable(&cl_touch_autoshoot_head_rifle);
	Cvar_RegisterVariable(&cl_touch_autoshoot_volley);
	Cvar_RegisterVariable(&cl_touch_aimassist_hysteresis);
	Cvar_RegisterVariable(&cl_touch_aimassist_priority);
	Cvar_RegisterVariable(&cl_touch_aim_mode);
	Cvar_RegisterVariable(&cl_touch_autoshoot_teamsafe);
	Cvar_RegisterVariable(&cl_touch_autoshoot_combo);
	Cvar_RegisterVariable(&cl_touch_aimassist_lastseen);
	Cvar_RegisterVariable(&cl_touch_practice_burst);
	Cvar_RegisterVariable(&cl_touch_practice_seethrough);
	Cvar_RegisterVariable(&cl_touch_practice_360);
	Cvar_RegisterVariable(&cl_touch_practice_instant);
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
	Cvar_RegisterVariable(&cl_touch_loser);
	Cvar_RegisterVariable(&cl_touch_aim_dumptracks);
	Cvar_RegisterVariable(&cl_touch_autoshoot_active);
	Cvar_RegisterVariable(&cl_touch_aim_lookscale);
	Cvar_RegisterVariable(&cl_touch_aim_disengage);
	Cvar_RegisterVariable(&cl_touch_hud_state);
	TouchHook_Init();
	ta.lookscale = 1;
	ta.teamplay_ofs = -1;
	ta.projtype_ofs = -1;
	ta.held_last = -1;
	ta.switch_to = -1;
	ta.manual_switch = ta.own_switch = -100;
	ta.stat_dealt = -1;
}

// A float or int global/field the CSQC may declare either way.
static qbool numeric_type(int type)
{
	return type != ev_string && type != ev_vector && type != ev_entity
		&& type != ev_field && type != ev_function && type != ev_void;
}

// The damage-dealt stat: an int in current builds, a float in 0.8.6's
// (whose bits read as a huge or negative int).
static int dealt_stat(void)
{
	int v;
	if (ta.stat_dealt < 0)
		return 0;
	v = cl.stats[ta.stat_dealt];
	if (v < 0 || v > 100000000)
		v = (int)cl.statsf[ta.stat_dealt];
	return v;
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
	num_shots = 0;
	memset(shot_seen, 0, sizeof(shot_seen));
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
	// what kind of projectile a CSQC projectile entity is (Ent_Projectile):
	// .csqcprojectile_type in current builds; 0.8.6 and older keep it in
	// .cnt, which other entities use too, so there the model must also be
	// the rocket's
	ta.projtype_is_cnt = false;
	d = PRVM_ED_FindField(prog, "csqcprojectile_type");
	if (!d)
	{
		d = PRVM_ED_FindField(prog, "cnt");
		ta.projtype_is_cnt = true;
	}
	if (d)
	{
		ta.projtype_type = d->type & ~DEF_SAVEGLOBAL;
		if (numeric_type(ta.projtype_type))
			ta.projtype_ofs = d->ofs;
	}
	// the damage the player has dealt, which every CSQC keeps for the hit
	// sound: STAT_HITSOUND_DAMAGE_DEALT_TOTAL.m_id is its stat number
	ta.stat_dealt = -1;
	d = PRVM_ED_FindGlobal(prog, "STAT_HITSOUND_DAMAGE_DEALT_TOTAL");
	{
		mdef_t *f_id = PRVM_ED_FindField(prog, "m_id");
		if (d && f_id && (d->type & ~DEF_SAVEGLOBAL) == ev_entity)
		{
			int e = PRVM_GLOBALFIELDEDICT(d->ofs);
			if (e > 0 && e < prog->num_edicts)
			{
				prvm_eval_t *v = PRVM_EDICTFIELDVALUE(PRVM_EDICT_NUM(e), f_id->ofs);
				int id = (f_id->type & ~DEF_SAVEGLOBAL) == ev_float ? (int)v->_float : v->_int;
				if (id > 0 && id < MAX_CL_STATS)
					ta.stat_dealt = id;
			}
		}
	}
	ta.dealt_last = dealt_stat();
	TouchHook_ProgsChanged();
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
	// the hook is held, or its weapon is coming or going, or a fall rescue or
	// a TURN spin owns the view: no shot on top
	if (TouchHook_InUse() || TouchHook_Rescuing())
		on = on2 = false;
	if (ta.firing == on && ta.firing2 == on2)
		return;
	ta.firing = on;
	ta.firing2 = on2;
	if (cl_touch_aim_debug.integer)
	{
		if ((on || on2) && target > 0 && target <= cl.maxclients)
			Con_Printf("touch aim: auto-shoot ON%s at player %d (%s)%s\n", on && on2 ? " (primary+secondary)" : on2 ? " (secondary)" : "", target, player_name(target),
				cl_touch_aim_debug.integer >= 2 ? va(vabuf, sizeof(vabuf), " at %.3f", cl.time) : "");
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
	ta.lock_ent = 0;
	ta.fight_ent = 0;
	ta.rk_state = RK_IDLE;
	ta.rk_ent = 0;
	ta.rk_way_on = false;
	ta.num_free = 0;
}

static qbool is_spectator_slot(int n)
{
	int frags = cl.scores[n - 1].frags;
	// Nexuiz-derived games mark spectators and out-of-game players this way
	// (the engine's own trace code relies on it, cl_collision.c); out of the
	// round is -669 since Xonotic 0.8.6.
	return frags == -666 || frags == -616 || frags == -669;
}

// Why this frame may not assist, or NULL when it may. The reason is printed
// under cl_touch_aim_debug so "it does nothing" can be told apart from "it is
// switched off for this reason".
const char *TouchAim_BlockReason(void)
{
	int me = cl.realplayerentity;
	// Touch mode only, unless the development "loser" toggle is on, which lets
	// the same helpers run on a desktop machine for testing.
	if (!cl_touch_loser.integer)
	{
		if (!vid_touchscreen.integer)
			return "vid_touchscreen is 0 (keyboard or mouse in use)";
		if (vid_touchscreen_mouselook.integer)
			return "a mouse is aiming next to the touch controls";
	}
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

// Automatic weapon switching: the player's setting, and the server allows it
// (touch_assist.c).
static qbool autoswitch_on(void)
{
	return cl_touch_autoswitch.integer != 0 && TouchAssist_Allowed(TOUCH_ASSIST_AUTOSWITCH);
}

// Clear line through world geometry (and CSQC brush models such as doors).
static qbool line_clear(const vec3_t from, const vec3_t to)
{
	trace_t tr = CL_TraceLine(from, to, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0,
		collision_extendmovelength.value, true, false, NULL, true, false);
	return !tr.startsolid && tr.fraction >= 0.999f;
}

// Offline practice (touch_weapons.h TouchWep_PracticeGate): the one check,
// made every frame, that decides whether anything offline-only may run.
static touch_practice_t practice;
static int practice_gate_now, practice_gate_said = -1;
// enemies to mark this frame (behind a wall, or off the screen in Full 360)
#define MARK_MAX 64
typedef struct mark_s { vec3_t org; float dist; qbool hidden; } mark_t;
static mark_t marks[MARK_MAX];
static int num_marks;

static int practice_gate(void)
{
	touch_practice_env_t e;
	int i;
	memset(&e, 0, sizeof(e));
	e.demo = cls.demoplayback;
	e.server_active = sv.active;
	e.local_connection = cls.state == ca_connected && cls.netcon && LHNETADDRESS_GetAddressType(&cls.netcon->peeraddress) == LHNETADDRESSTYPE_LOOP;
	if (sv.active && svs.clients)
		for (i = 0; i < svs.maxclients; i++)
		{
			const client_t *c = &svs.clients[i];
			// bots have no connection; this client's own comes over the loopback
			if (c->active && c->netconnection && LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) != LHNETADDRESSTYPE_LOOP)
				e.other_humans++;
		}
	return TouchWep_PracticeGate(&e);
}

// Whether a straight shot toward p gets there: a clear line, or for a splash
// weapon one that ends close enough to p for the blast to reach it (a wall
// beside the enemy is as good a place for a rocket to burst).
static qbool shot_path_ok(const vec3_t eye, const vec3_t p, const touch_wep_fire_t *f)
{
	trace_t tr = CL_TraceLine(eye, p, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0,
		collision_extendmovelength.value, true, false, NULL, true, false);
	if (!tr.startsolid && tr.fraction >= 0.999f)
		return true;
	return f && f->splash >= 60 && !tr.startsolid && VectorDistance(tr.endpos, p) < 0.8f * f->splash;
}

// A rocket launched toward p (cl_touch_autoshoot_clear): it leaves from the
// eye with its own size (6 units across), so a ledge lip or a low wall the
// eye just sees past can still stop it. Clear when the box trace reaches p,
// or ends within reach of the blast there.
static qbool launch_clear(const vec3_t eye, const vec3_t p, float splash)
{
	static const vec3_t mins = { -3, -3, -3 }, maxs = { 3, 3, 3 };
	trace_t tr = CL_TraceBox(eye, mins, maxs, p, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false);
	if (tr.startsolid)
		return line_clear(eye, p);
	return tr.fraction >= 0.999f || VectorDistance(tr.endpos, p) < 0.6f * splash;
}

// The first stretch of a rocket's flight, before the server starts guiding
// it (g_balance_devastator_guidedelay 0.2 s, some 220 units): straight along
// the view as it is now, whatever the aim point.
#define ROCKET_UNGUIDED 260.0f
static qbool launch_clear_view(const vec3_t eye, float dist, float splash)
{
	vec3_t fwd, p;
	AngleVectors(cl.viewangles, fwd, NULL, NULL);
	VectorMA(eye, min(dist, ROCKET_UNGUIDED), fwd, p);
	return launch_clear(eye, p, dist <= ROCKET_UNGUIDED ? splash : 0);
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
	qbool lead_failed;  // a projectile in hand and no meeting point with this moving enemy
	qbool head_aim;     // aimed at the head (a headshot bonus pays there)
	qbool hidden;       // behind a wall: only ever a target in offline practice
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
		t->lead_time = 0;
		VectorClear(t->vel);
		if (VectorLength2(v) > 1 && VectorLength(v) <= TRACK_MAX_SPEED)
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
	// a teleport or a respawn reads as one absurd sample: no player runs that fast
	if (VectorLength(v) > TRACK_MAX_SPEED)
		VectorScale(v, TRACK_MAX_SPEED / VectorLength(v), v);
	f = bound(0.0f, dt / TRACK_SMOOTH, 1.0f);
	VectorLerp(t->vel, f, v, t->vel);
	VectorCopy(org, t->org);
	t->time = now;
	VectorCopy(t->vel, vel_out);
}

// Visible players (enemies and teammates) for splash scoring: their boxes
// and velocities, collected by scan when a splash decision may need them --
// every drawn player in range (32 a side fit), traced only later and only
// near the target.
#define GROUP_MAX 64
typedef struct gbody_s { int n; qbool enemy; vec3_t bmin, bmax, vel; float d; } gbody_t;
static gbody_t gbodies[GROUP_MAX];
static int num_gbodies;

// Keep only the candidates near the target (within GROUP_NEAR of its centre)
// that are in sight -- the chest or at least the head: the only ones a
// splash decision may count, the GROUP_AIM_MAX nearest at most (the blast
// point search is quadratic in them). The target itself is in sight already.
#define GROUP_NEAR 300.0f
#define GROUP_AIM_MAX 8
static void gbodies_in_sight(const vec3_t eye, const vec3_t center, int target)
{
	int i, j, k = 0, n = 0;
	for (i = 0; i < num_gbodies; i++)
	{
		gbody_t g = gbodies[i];
		vec3_t c;
		VectorMAM(0.5f, g.bmin, 0.5f, g.bmax, c);
		g.d = g.n == target ? -1 : VectorDistance(c, center);
		if (g.d > GROUP_NEAR)
			continue;
		// nearest first (the target leads)
		for (j = n; j > 0 && gbodies[j - 1].d > g.d; j--)
			gbodies[j] = gbodies[j - 1];
		gbodies[j] = g;
		n++;
	}
	for (i = 0; i < n && k < GROUP_AIM_MAX; i++)
	{
		gbody_t *g = &gbodies[i];
		vec3_t c, hd;
		VectorMAM(0.5f, g->bmin, 0.5f, g->bmax, c);
		if (g->n != target)
		{
			VectorCopy(c, hd);
			hd[2] = g->bmax[2] - 6;
			if (!line_clear(eye, c) && !line_clear(eye, hd))
				continue;
		}
		gbodies[k++] = *g;
	}
	num_gbodies = k;
}

// Motion of a player for the lead and the weapon choice: on the ground or
// in the air (a short trace under its feet; the floor below and the time to
// land when airborne), and its strafe reversals across the line of sight.
#define AIR_TRACE      24.0f   // the game's own IsFlying test
#define FLOOR_TRACE    1024.0f
#define REV_MIN_SPEED  120.0f  // a reversal: clear sideways speed on both sides
#define REV_WINDOW     1.5f    // reversals counted over this many seconds
#define STAND_SPEED    60.0f
#define AIR_RECHECK    0.1f
static void track_motion(int n, const vec3_t org, const vec3_t mins, const vec3_t eye, const vec3_t vel, qbool want_air, double now)
{
	track_t *t = &ta.tracks[n & (TRACK_MAX - 1)];
	vec3_t a, b, d;
	float lat, vh;
	int mclass, k, nrev = 0;
	trace_t tr;
	// sideways speed across the line of sight, signed
	VectorSubtract(org, eye, d);
	d[2] = 0;
	VectorNormalize(d);
	lat = d[0] * vel[1] - d[1] * vel[0];
	if (fabs(lat) > REV_MIN_SPEED)
	{
		int sign = lat > 0 ? 1 : -1;
		if (t->lat_sign && sign != t->lat_sign)
		{
			for (k = 3; k > 0; k--)
				t->rev[k] = t->rev[k - 1];
			t->rev[0] = now;
		}
		t->lat_sign = sign;
	}
	for (k = 0; k < 4; k++)
		if (t->rev[k] > 0 && now - t->rev[k] < REV_WINDOW)
			nrev++;
	// in the air: nothing solid within AIR_TRACE under the feet (traced at
	// most every AIR_RECHECK seconds; the landing time follows every frame)
	if (want_air && now - t->air_checked < AIR_RECHECK)
	{
		if (t->airborne)
			t->t_land = TouchWep_LandTime(org[2] + mins[2] - t->floor_z, vel[2], cl.movevars_gravity);
	}
	else if (want_air)
	{
		t->air_checked = now;
		t->airborne = false;
		t->t_land = -1;
		VectorCopy(org, a);
		a[2] += mins[2] + 1;
		VectorCopy(a, b);
		b[2] -= AIR_TRACE;
		tr = CL_TraceLine(a, b, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, true, false);
		if (tr.fraction >= 1)
		{
			b[2] = a[2] - FLOOR_TRACE;
			tr = CL_TraceLine(a, b, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, true, false);
			t->floor_z = tr.endpos[2];
			t->airborne = true;
			t->t_land = TouchWep_LandTime(a[2] - 1 - t->floor_z, vel[2], cl.movevars_gravity);
		}
	}
	else
	{
		t->airborne = false;
		t->t_land = -1;
	}
	vh = sqrt(vel[0] * vel[0] + vel[1] * vel[1]);
	mclass = t->airborne ? TW_CLASS_AIR : nrev >= 2 ? TW_CLASS_ERRATIC : vh < STAND_SPEED ? TW_CLASS_STAND : TW_CLASS_STEADY;
	if (mclass != t->cls)
	{
		t->cls = mclass;
		t->cls_since = now;
	}
}

// Mean time between the target's recent strafe reversals (0: fewer than 2),
// and the time since its last one.
static float reversal_period(const track_t *t, double now, float *since)
{
	int k, m = 0;
	*since = t->rev[0] > 0 ? (float)(now - t->rev[0]) : 0;
	for (k = 1; k < 4; k++)
		if (t->rev[k] > 0 && now - t->rev[k] < 2 * REV_WINDOW)
			m = k;
	if (m < 1)
		return 0;
	return (float)((t->rev[0] - t->rev[m]) / m);
}

// A player runs into a wall and stops; a lead that carries it on through
// the wall sends the shot (or a steered rocket) into the wall. The sideways
// part of a lead from the body centre is cut where a box the width of a
// player, clear of the floor, would meet the world (cl_touch_autoshoot_predict).
static void clip_lead(const vec3_t center, vec3_t lead)
{
	static const vec3_t mins = { -14, -14, -8 }, maxs = { 14, 14, 8 };
	vec3_t to;
	trace_t tr;
	if (lead[0] * lead[0] + lead[1] * lead[1] < 16 * 16)
		return;
	VectorSet(to, center[0] + lead[0], center[1] + lead[1], center[2]);
	tr = CL_TraceBox(center, mins, maxs, to, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false);
	if (tr.startsolid || tr.fraction >= 1)
		return;
	lead[0] = tr.endpos[0] - center[0];
	lead[1] = tr.endpos[1] - center[1];
}

// Where to aim at player n for this fire, as an offset from the body centre
// (lead) with its flight time. Lead and latency as before; then, each under
// its cvar: the lag of the screen behind the server and a lead shrunk toward
// the middle of an erratic strafe (_predict), the fall of an enemy in the air
// (_air), and the floor at the feet for splash weapons (_feet: the target
// point moves from the chest to the floor under the led feet, unless the
// feet are above the eye or the floor point cannot be seen).
static qbool compute_lead(int n, const touch_wep_fire_t *fire, const vec3_t eye, const vec3_t center, const vec3_t bmin, const vec3_t vel, float latency, qbool slew, qbool guided, qbool candidate, vec3_t lead, float *tof)
{
	track_t *t = &ta.tracks[n & (TRACK_MAX - 1)];
	double now = host.realtime;
	float gravity = cl.movevars_gravity, t_land = -1, k = 1;
	vec3_t rel, aim, v;
	qbool ok, feet = false, lifted;
	VectorSubtract(center, eye, rel);
	VectorCopy(vel, v);
	// a rocket held because its launch path hit a lip: aim it at the head
	// for a moment, the part of the body that clears the lip
	lifted = guided && cl_touch_autoshoot_clear.integer && ta.rk_lift_ent == n && now < ta.rk_lift_until;
	if (lifted)
		rel[2] += center[2] - bmin[2] - 8;
	// (a steered rocket is re-aimed in flight by its own straight-line intercept)
	if (cl_touch_autoshoot_air.integer && t->airborne && t->t_land >= 0 && !guided)
		t_land = t->t_land;
	if (cl_touch_autoshoot_predict.integer && t->cls == TW_CLASS_ERRATIC)
	{
		// the share of the sideways motion to lead (TouchWep_LeadShare:
		// measured on bot movement, all of it)
		float since, period = reversal_period(t, now, &since), along;
		vec3_t d;
		ok = TouchWep_AimPointAir(fire, rel, vel, latency, gravity, t_land, aim, tof);
		k = TouchWep_LeadShare(t->cls, (ok ? *tof : 0.5f) + latency, period);
		(void)since;
		VectorCopy(rel, d);
		d[2] = 0;
		VectorNormalize(d);
		along = DotProduct(vel, d);
		v[0] = d[0] * along + k * (vel[0] - d[0] * along);
		v[1] = d[1] * along + k * (vel[1] - d[1] * along);
	}
	// (only for a candidate near the crosshair: the floor point is traced)
	if (cl_touch_autoshoot_feet.integer && candidate && t_land < 0 && fire->splash >= 100 && !t->airborne && !lifted
		&& (fire->kind == TW_KIND_PROJ || fire->kind == TW_KIND_BALLISTIC) && bmin[2] < eye[2] - 16)
	{
		// the floor under the feet, a little on the shooter's side
		vec3_t fp, side;
		VectorSubtract(eye, center, side);
		side[2] = 0;
		VectorNormalize(side);
		VectorMA(center, 12, side, fp);
		fp[2] = bmin[2] + 4;
		VectorSubtract(fp, eye, rel);
		feet = true;
	}
	ok = TouchWep_AimPointAir(fire, rel, v, latency, gravity, t_land, aim, tof);
	if (!ok)
		return false;
	if (feet)
	{
		// the floor point must be in sight, or the shot hits whatever is between
		vec3_t fpw;
		if (fire->kind == TW_KIND_PROJ)
		{
			VectorAdd(eye, aim, fpw);
			if (!line_clear(eye, fpw) || (guided && cl_touch_autoshoot_clear.integer && !launch_clear(eye, fpw, fire->splash)))
			{
				VectorSubtract(center, eye, rel);
				ok = TouchWep_AimPointAir(fire, rel, v, latency, gravity, t_land, aim, tof);
				if (!ok)
					return false;
				feet = false;
			}
		}
	}
	if (slew && *tof > 0)
	{
		// slide only the part of the aim that comes from the motion: the
		// aim at the same point standing still is the fixed part
		vec3_t still, aim0, motion;
		float tof0;
		VectorClear(still);
		if (!TouchWep_AimPointAir(fire, rel, still, latency, gravity, -1, aim0, &tof0))
			VectorCopy(rel, aim0);
		VectorSubtract(aim, aim0, motion);
		if (t->lead_time <= 0 || now - t->lead_time > TRACK_STALE)
			VectorCopy(motion, t->lead);
		else
			TouchWep_LeadSlew(t->lead, motion, VectorLength(vel), (float)(now - t->lead_time));
		t->lead_time = now;
		VectorAdd(aim0, t->lead, aim);
	}
	// offset from the body centre
	VectorSubtract(center, eye, rel);
	VectorSubtract(aim, rel, lead);
	if (cl_touch_autoshoot_predict.integer && fire->kind == TW_KIND_PROJ && candidate)
		clip_lead(center, lead);
	return *tof > 0 || feet;
}

// Teammates seen this frame (cl_touch_autoshoot_teamsafe): a shot is held
// while one is on its line or would be in its blast.
#define MATE_MAX 64
typedef struct mate_s { int n; vec3_t bmin, bmax; } mate_t;
static mate_t mates[MATE_MAX];
static int num_mates;

// Enemies inside the cone this frame, before one is chosen.
#define CAND_MAX 64
typedef struct cand_s { aimtarget_t a; float eff; qbool on; qbool tested; } cand_t;
static cand_t tcands[CAND_MAX];
static int num_cands;

// A new target is kept at least this long while it stays a candidate
// (cl_touch_aimassist_priority), so two enemies of about the same worth do
// not take turns.
#define TARGET_HOLD 0.5
// Damage dealt counts toward "already hurt" this long.
#define DEALT_KEEP 3.0
// Facing the player: within this many degrees of looking straight at them.
#define FACING_DEG 35.0f

// Degrees by which an enemy counts nearer the crosshair (TouchWep_PriorityBonus).
static float priority_bonus(const aimtarget_t *a, const touch_wep_t *wep, const touch_wep_fire_t *fire, double now)
{
	const track_t *t = &ta.tracks[a->entnum & (TRACK_MAX - 1)];
	touch_wep_prio_t p;
	int i;
	memset(&p, 0, sizeof(p));
	p.dist = a->dist;
	p.dps_share = wep ? TouchWep_DPSShare(wep->netname, a->dist, t->cls) : -1;
	p.facing = t->facing;
	p.range_rate = t->range_rate;
	p.instant = !fire || fire->kind == TW_KIND_HITSCAN || fire->kind == TW_KIND_BEAM || fire->kind == TW_KIND_MELEE;
	p.dealt = now - t->dealt_time < DEALT_KEEP ? t->dealt : 0;
	if (fire && fire->splash >= 100)
		for (i = 0; i < num_gbodies; i++)
		{
			const gbody_t *g = &gbodies[i];
			float dx = 0.5f * (g->bmin[0] + g->bmax[0]) - a->center[0], dy = 0.5f * (g->bmin[1] + g->bmax[1]) - a->center[1];
			if (g->enemy && g->n != a->entnum && sqrt(dx * dx + dy * dy) < 2 * fire->splash + 32)
				p.group++;
		}
	return TouchWep_PriorityBonus(&p);
}

// One pass over the players the CSQC is drawing: the one under the crosshair
// (for the old auto-shoot test and the ring), the teammates, and the enemies
// inside the cone, measured to where the shot should go when `fire` is a
// projectile and lead is on. Then the target among them: the nearest to the
// crosshair (cl_touch_aimassist_priority: shifted by what each is worth),
// the one held last frame preferred. All must be enemies, alive, drawn, in
// range and in line of sight.
static void scan(const vec3_t eye, const vec3_t fwd, float cone, float lock_cone, const touch_wep_t *wep, const touch_wep_fire_t *fire, float latency, qbool slew, qbool guided, qbool collect, int *on_target, int *friend_on, aimtarget_t *near_out)
{
	prvm_prog_t *prog = CLVM_prog;
	int team_game = teamplay_mode();
	float range = cl_touch_aim_range.value <= 0 ? 1e9f : max(64.0f, cl_touch_aim_range.value); // 0: no limit
	extern cvar_t cl_touch_practice_exact;
	float best_on = 1e9f;
	float hyst = bound(0.0f, cl_touch_aimassist_hysteresis.value, 15.0f); // degrees the current target is preferred by
	qbool prio = cl_touch_aimassist_priority.integer != 0;
	double now = host.realtime;
	int i, players = 0;
	if (TouchTactics_Open() && cl_touch_practice_exact.integer)
		range = 1e9f; // offline only: any distance
	*on_target = 0;
	*friend_on = 0;
	num_gbodies = 0;
	num_mates = 0;
	num_cands = 0;
	memset(near_out, 0, sizeof(*near_out));
	near_out->err = 1e9f;
	for (i = 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		vec3_t org, mins, maxs, bmin, bmax, center, dir, vel, lead, lbmin, lbmax;
		float dist, err, t, alpha, hw, tof = 0;
		int n;
		qbool enemy, has_lead = false, on_lead = false;
		float e0;
		track_t *tr;
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
		players++;
		if (collect && num_gbodies < GROUP_MAX)
		{
			// a candidate: whether it is in sight is traced later, and only
			// if it stands near the target (gbodies_in_sight)
			gbody_t *g = &gbodies[num_gbodies++];
			g->n = n;
			g->enemy = enemy;
			VectorCopy(bmin, g->bmin);
			VectorCopy(bmax, g->bmax);
			VectorCopy(PRVM_clientedictvector(ed, velocity), g->vel);
			if (VectorLength(g->vel) > TRACK_MAX_SPEED)
				VectorClear(g->vel);
		}
		if (!enemy)
		{
			if (num_mates < MATE_MAX)
			{
				mate_t *m = &mates[num_mates++];
				m->n = n;
				VectorCopy(bmin, m->bmin);
				VectorCopy(bmax, m->bmax);
			}
			// only noted for cl_touch_aim_debug 2: a teammate under the crosshair
			if (cl_touch_aim_debug.integer >= 2 && ray_box(eye, fwd, bmin, bmax, &t))
				*friend_on = n;
			continue;
		}
		hw = 0.5f * max(maxs[0] - mins[0], maxs[1] - mins[1]);

		// offline practice: mark an enemy behind a wall, or off the screen in Full 360
		if ((practice.seethrough || practice.full360) && num_marks < MARK_MAX)
		{
			vec3_t hd;
			qbool hid;
			VectorCopy(center, hd);
			hd[2] = bmax[2] - 6;
			hid = !line_clear(eye, center) && !line_clear(eye, hd);
			if ((hid && practice.seethrough) || practice.full360)
			{
				mark_t *mk = &marks[num_marks++];
				VectorCopy(center, mk->org);
				mk->dist = dist;
				mk->hidden = hid;
			}
		}

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
		TouchTactics_ExactVelocity(n, vel); // offline only: the server's own velocity
		tr = &ta.tracks[n & (TRACK_MAX - 1)];
		VectorCopy(bmin, tr->bmin);
		VectorCopy(bmax, tr->bmax);
		// coming or going, and whether it looks this way
		tr->range_rate = DotProduct(vel, dir);
		{
			float ay = PRVM_clientedictvector(ed, angles)[YAW] * (float)(M_PI / 180.0);
			tr->facing = -(cos(ay) * dir[0] + sin(ay) * dir[1]) > cos(FACING_DEG * (float)(M_PI / 180.0)) * sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
		}
		// ground or air, reversals: only for those near enough the crosshair
		// to matter (a short trace each, at most ten times a second)
		{
			float yaw0, pitch0;
			angles_to(eye, center, &yaw0, &pitch0);
			e0 = sqrt(wrap180(yaw0 - cl.viewangles[YAW]) * wrap180(yaw0 - cl.viewangles[YAW]) + (pitch0 - cl.viewangles[PITCH]) * (pitch0 - cl.viewangles[PITCH]));
			track_motion(n, org, mins, eye, vel, e0 < max(cone, lock_cone) + 5 && (cl_touch_autoshoot_air.integer || cl_touch_autoswitch_smart.integer), now);
		}
		VectorClear(lead);
		if (fire)
		{
			if (compute_lead(n, fire, eye, center, bmin, vel, latency, slew, guided, e0 < max(cone, lock_cone) + 5, lead, &tof))
				has_lead = true;
		}

		// near the crosshair: how far the crosshair is from the body's edge,
		// so a big close body counts as soon as the crosshair nears it, not
		// only once its centre is inside the cone. The aim point is the chest
		// (the head where a headshot bonus pays); led when it flies.
		{
			float yaw, pitch, rh, rv, ey, ep, near_err;
			qbool head_aim;
			vec3_t aimp;
			cand_t *c;
			rh = atan(hw / dist) * (180.0f / M_PI);
			rv = atan(0.5f * (maxs[2] - mins[2]) / dist) * (180.0f / M_PI);
			VectorAdd(center, lead, aimp);
			// a weapon with a headshot bonus: the head, where it pays more
			head_aim = ta.head_mult > 1 && TouchWep_HeadBetter(ta.head_mult, hw, bmin[2], bmax[2],
				TouchWep_AimSigma(dist, tr->cls, ta.head_spread));
			if (head_aim)
				aimp[2] += bmax[2] - 0.5f * TW_HEAD_ZONE_H - center[2];
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
				on_lead = n == *on_target;
				err = on_lead ? 0 : edge_err(ey, ep, rh, rv);
			}
			// in the cone: the led body or the body itself (a thumb on the
			// body of a fast target is near it, however long the lead); the
			// enemy of the combat lock may be followed out to lock_cone
			near_err = err;
			if (has_lead)
			{
				float y0, p0;
				angles_to(eye, center, &y0, &p0);
				near_err = min(err, n == *on_target ? 0 : edge_err(wrap180(y0 - cl.viewangles[YAW]), p0 - cl.viewangles[PITCH], rh, rv));
			}
			if (near_err > (n == ta.lock_ent && lock_cone > cone ? lock_cone : cone) || num_cands >= CAND_MAX)
				continue;
			c = &tcands[num_cands++];
			memset(c, 0, sizeof(*c));
			c->on = n == *on_target;
			c->a.entnum = n;
			c->a.err = err;
			c->a.rh = rh;
			c->a.rv = rv;
			c->a.yaw = yaw;
			c->a.pitch = pitch;
			c->a.dist = dist;
			VectorCopy(center, c->a.center);
			VectorCopy(vel, c->a.vel);
			VectorCopy(lead, c->a.lead);
			c->a.tof = tof;
			c->a.has_lead = has_lead;
			c->a.lead_failed = fire && !has_lead && VectorLength(vel) > STAND_SPEED;
			c->a.head_aim = head_aim;
			c->a.on_lead = on_lead;
			// the target of last frame keeps its place unless another is
			// clearly nearer the crosshair (a wide cone must not flip)
			c->eff = near_err - (n == ta.track_ent ? hyst : 0);
		}
	}
	ta.cost_players = players;

	// what each is worth, then the best one that is in sight (traced in
	// order of preference, so mostly once)
	if (prio)
		for (i = 0; i < num_cands; i++)
		{
			cand_t *c = &tcands[i];
			c->eff -= priority_bonus(&c->a, wep, fire, now);
			c->eff -= TouchTactics_TargetBonus(c->a.entnum); // offline only: nearly dead, aiming at the player
			if (c->a.entnum == ta.track_ent && ta.track_ent && now - ta.target_since < TARGET_HOLD)
				c->eff -= 1000; // just taken: kept a moment while it stays in sight
		}
	for (;;)
	{
		cand_t *best = NULL;
		vec3_t head, aimp;
		for (i = 0; i < num_cands; i++)
		{
			cand_t *c = &tcands[i];
			if (c->tested)
				continue;
			if (!best || c->eff < best->eff || (c->eff == best->eff && c->on && !best->on))
				best = c;
		}
		if (!best)
			break;
		best->tested = true;
		if (!best->on && !line_clear(eye, best->a.center))
		{
			float dist = best->a.dist, yaw, pitch;
			VectorCopy(best->a.center, head);
			head[2] = ta.tracks[best->a.entnum & (TRACK_MAX - 1)].bmax[2] - 6;
			if (!line_clear(eye, head))
			{
				// behind a wall: a target only in offline practice
				if (!practice.seethrough)
					continue;
				best->a.hidden = true;
				*near_out = best->a;
				break;
			}
			// only the head shows: aim at it, and count only it as the body
			VectorAdd(head, best->a.lead, aimp);
			angles_to(eye, aimp, &yaw, &pitch);
			best->a.yaw = yaw;
			best->a.pitch = pitch;
			best->a.rv = atan(6.0f / dist) * (180.0f / M_PI);
			best->a.err = best->a.on_lead ? 0 : edge_err(wrap180(yaw - cl.viewangles[YAW]), pitch - cl.viewangles[PITCH], best->a.rh, best->a.rv);
			if (best->a.err > (best->a.entnum == ta.lock_ent && lock_cone > cone ? lock_cone : cone))
				continue;
		}
		*near_out = best->a;
		break;
	}
	if (near_out->entnum != ta.track_ent)
	{
		if (near_out->entnum && ta.track_ent)
		{
			ta.target_switches++;
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch aim: target switch %d -> %d (%d candidates in the cone; switch %d)\n", ta.track_ent, near_out->entnum, num_cands, ta.target_switches);
		}
		ta.target_since = now;
	}
}

// A teammate the shot about to go would hit or catch (cl_touch_autoshoot_teamsafe),
// or 0. impact: where it meets the enemy (world).
static int mate_blocks(const vec3_t eye, const vec3_t fwd, float target_dist, const touch_wep_t *wep, const touch_wep_fire_t *f, const vec3_t impact, int *how)
{
	int i;
	for (i = 0; i < num_mates; i++)
	{
		int r = TouchWep_MateBlocks(f, wep && (wep->flags & TW_PIERCE) && f == &wep->pri, eye, fwd, target_dist, impact, mates[i].bmin, mates[i].bmax);
		if (r != TW_MATE_NONE)
		{
			*how = r;
			return mates[i].n;
		}
	}
	return 0;
}

// A teammate within reach of a blast at p (a rocket set off there), under
// cl_touch_autoshoot_teamsafe: the detonation waits.
static qbool mate_at_blast(const vec3_t p, float radius)
{
	int i;
	if (!cl_touch_autoshoot_teamsafe.integer)
		return false;
	for (i = 0; i < num_mates; i++)
		if (TouchWep_BoxGap(p, mates[i].bmin, mates[i].bmax) < radius + TW_MATE_MARGIN)
		{
			if (cl_touch_aim_debug.integer && ta.mate_said != mates[i].n)
				Con_Printf("touch aim: rocket not set off: teammate %d (%s) inside the blast\n", mates[i].n, player_name(mates[i].n));
			ta.mate_said = mates[i].n;
			return true;
		}
	return false;
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

// Latency for a projectile's lead: the moves the server has not taken yet,
// plus (cl_touch_autoshoot_predict) how far the drawn players trail the
// newest snapshot -- about one server frame of interpolation.
static float shot_latency(void)
{
	float l = latency_estimate();
	if (cl_touch_autoshoot_predict.integer)
		l += bound(0.0f, (float)(cl.mtime[0] - cl.mtime[1]), 0.1f);
	return l;
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
	if (TouchHook_InUse())
	{
		// the hook's weapon switch, there and back: not the player's choice
	}
	else if (held >= 0 && held == ta.switch_to)
	{
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: switch landed: %s\n", TouchHUD_WeaponName(held));
	}
	else if (ta.held_last >= 0 && held >= 0 && !weapon_ready(ta.held_last, TouchWep_Find(TouchHUD_WeaponName(ta.held_last)), -1))
	{
		// the last one ran dry: the server switched by itself, not the player
		ta.dropped = TouchWep_IsFallback(TouchHUD_WeaponName(held));
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: weapon now %s (%s ran out of ammo)\n", TouchHUD_WeaponName(held), TouchHUD_WeaponName(ta.held_last));
		ta.switch_to = -1;
		ta.held_last = held;
		return;
	}
	else
	{
		ta.manual_switch = now;
		if (cl_touch_aim_debug.integer && held >= 0)
			Con_Printf("touch aim: weapon now %s (not asked by auto-switch: no switch for %.0f s)\n", TouchHUD_WeaponName(held), MANUAL_GUARD);
	}
	ta.switch_to = -1;
	ta.held_last = held;
	ta.dropped = false;
}

// How many enemies besides the target stand close enough to share a blast
// with it (for the weapon choice).
static int group_near(const aimtarget_t *near)
{
	int i, c = 0;
	for (i = 0; i < num_gbodies; i++)
	{
		const gbody_t *g = &gbodies[i];
		float dx, dy;
		if (!g->enemy || g->n == near->entnum)
			continue;
		dx = 0.5f * (g->bmin[0] + g->bmax[0]) - near->center[0];
		dy = 0.5f * (g->bmin[1] + g->bmax[1]) - near->center[1];
		if (sqrt(dx * dx + dy * dy) < 2 * 110 + 32)
			c++;
	}
	return c;
}

static qbool free_all_certain(void);

// The weapon in hand running low (TouchWep_DryPlan): fire it to the last shot
// it can pay for, then switch at once to the best weapon with ammo -- before
// the server's own empty switch, which may pick the Blaster -- or sooner when
// the other weapon does more over the next moments. With nothing worth having
// left, publish _cl_touch_aim_disengage for the dodge to keep away. True
// when it asked for a switch.
static qbool dry_check(const aimtarget_t *near, int held, const touch_wep_t *wep, double now)
{
	const char *names[72];
	qbool ready[72];
	int index[72], n = 0, i, best, nshots, plan, mclass;
	float bu = 0, hu = 0, dist, held_dps, alt_dps = 0, refire;
	qbool fight = near->entnum || (ta.lock_ent && now < ta.lock_until), engage = 0;
	static int said_plan = -1;
	if (held < 0 || !wep)
	{
		if (cl_touch_aim_disengage.integer)
			Cvar_SetValueQuick(&cl_touch_aim_disengage, 0);
		return false;
	}
	dist = near->entnum ? near->dist : ta.fight_dist > 0 ? ta.fight_dist : 800;
	mclass = near->entnum ? ta.tracks[near->entnum & (TRACK_MAX - 1)].cls : TW_CLASS_STEADY;
	nshots = TouchWep_ShotsLeft(TouchHUD_WeaponAmmo(held), TouchHUD_HeldClip(), wep->cost);
	refire = TouchWep_Refire(wep->netname, ta.last_mode == TW_MODE_SECONDARY);
	for (i = 0; i < TouchHUD_NumWeapons() && n < 72; i++)
	{
		if (!TouchHUD_WeaponOwned(i) || i == held)
			continue;
		index[n] = i;
		names[n] = TouchHUD_WeaponName(i);
		ready[n] = weapon_ready(i, TouchWep_Find(names[n]), held);
		n++;
	}
	best = TouchWep_ChooseSmart(names, ready, n, -1, dist, mclass, 0, cl_touch_autoswitch_lastresort.integer != 0, &bu, &hu);
	if (best >= 0)
		alt_dps = max(0.01f, TouchWep_ExpectedDPS(names[best], dist, mclass));
	held_dps = TouchWep_Usable(wep, dist) ? max(0.0f, TouchWep_ExpectedDPS(wep->netname, dist, mclass)) : 0;
	plan = TouchWep_DryPlan(held_dps, nshots, refire, alt_dps, best >= 0 && TouchWep_IsFallback(names[best]), TW_HORIZON, TW_SWITCH_COST);
	// nothing worth a fight: the hand is empty or a last resort, and so is
	// everything else
	engage = !((nshots <= 0 || TouchWep_IsFallback(wep->netname)) && (best < 0 || TouchWep_IsFallback(names[best])));
	if (cl_touch_aim_disengage.integer != (fight && !engage))
	{
		Cvar_SetValueQuick(&cl_touch_aim_disengage, fight && !engage);
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: %s\n", fight && !engage ? va(vabuf, sizeof(vabuf), "disengage advised: %s in hand, nothing better with ammo", wep->netname)
				: "disengage advice off");
	}
	if (nshots > 3)
		said_plan = -1;
	if (cl_touch_aim_debug.integer && nshots < TW_SHOTS_UNLIMITED && nshots <= 3 && plan != said_plan)
		Con_Printf("touch aim: %s dry in %.2f s (%d shots left)%s\n", wep->netname, TouchWep_TimeToDry(nshots, refire), nshots,
			plan == TW_DRY_SWITCH && best >= 0 ? va(vabuf, sizeof(vabuf), " -> %s", names[best]) : plan == TW_DRY_EVADE ? ": nothing better, keep away" : ": firing it out first");
	if (nshots <= 3)
		said_plan = plan;
	if (plan != TW_DRY_SWITCH || best < 0 || !fight || !autoswitch_on())
		return false;
	if (now - ta.manual_switch < MANUAL_GUARD || ta.switch_to >= 0 || TouchHook_InUse() || TouchHook_Rescuing())
		return false;
	if ((ta.rk_state != RK_IDLE && ta.rk_state != RK_RELEASE) || (ta.num_free && !free_all_certain()) || (in_attack.state & 3) || (in_button3.state & 3))
		return false;
	Cbuf_AddText(cmd_local, va(vabuf, sizeof(vabuf), "weapon_%s\n", names[best]));
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch aim: switch %s -> %s at %.0f units (%s dry: %d shots left, expected damage %.0f vs %.0f per second)\n", wep->netname, names[best], dist,
			wep->netname, nshots, alt_dps, held_dps);
	ta.switch_to = index[best];
	ta.switch_asked = now;
	ta.own_switch = now;
	return true;
}

// Switch to the best owned weapon for the fight, under the guards.
static qbool free_all_certain(void);
static void auto_switch(const aimtarget_t *near, int held, const touch_wep_t *wep, double now)
{
	const char *names[72];
	qbool ready[72];
	int index[72], n = 0, i, best = -1, rank = 0, cur_rank = 0, heldpos = -1, mclass = TW_CLASS_STEADY, group = 0;
	qbool cur_ok, smart = cl_touch_autoswitch_smart.integer != 0, combo = false, escape, dropped;
	float bu = 0, hu = 0, dist;
	const track_t *t;
	if (!autoswitch_on() || held < 0 || !wep || TouchHook_InUse() || TouchHook_Rescuing())
		return;
	// a switch ends steering and remote detonation of every rocket out
	if (ta.num_free && !free_all_certain())
		return;
	if (near->entnum != ta.fight_ent)
	{
		ta.fight_ent = near->entnum;
		ta.fight_since = now;
	}
	if (near->entnum)
		ta.fight_dist = near->dist;
	// the server dropped the player to the Blaster (or another last resort)
	// off an empty weapon: leave it at once for the best owned weapon with
	// ammo, whether an enemy is in sight or not (that was no pick of theirs)
	dropped = cl_touch_autoswitch_lastresort.integer && ta.dropped && TouchWep_IsFallback(wep->netname);
	if (!dropped && (!near->entnum || now - ta.fight_since < FIGHT_MIN))
		return;
	dist = near->entnum ? near->dist : ta.fight_dist > 0 ? ta.fight_dist : 800;
	t = near->entnum ? &ta.tracks[near->entnum & (TRACK_MAX - 1)] : NULL;
	mclass = t ? t->cls : TW_CLASS_STEADY;
	// combo: our blast has just thrown the target into the air: finish it
	// with an instant-hit weapon now, not after the usual pause
	combo = cl_touch_autoswitch_combo.integer && t && mclass == TW_CLASS_AIR && wep->pri.splash >= 100 && now - ta.launcher_fired < 2.0;
	// in hand: the Blaster or another last resort: a real weapon wins
	// whatever the margin (the server's drop, or a fight it cannot win)
	escape = cl_touch_autoswitch_lastresort.integer && TouchWep_IsFallback(wep->netname);
	if (now - ta.manual_switch < MANUAL_GUARD)
		return;
	if (now - ta.own_switch < (combo || escape ? 0.3f : smart ? OWN_GUARD_SMART : OWN_GUARD))
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
		if (i == held)
			heldpos = n;
		index[n] = i;
		names[n] = TouchHUD_WeaponName(i);
		ready[n] = weapon_ready(i, TouchWep_Find(names[n]), held);
		n++;
	}
	cur_ok = weapon_ready(held, wep, held) && TouchWep_Usable(wep, dist);
	if (combo)
	{
		// the best instant-hit weapon owned, by its damage against a target in the air
		for (i = 0; i < n; i++)
		{
			const touch_wep_t *w = TouchWep_Find(names[i]);
			float u;
			if (!ready[i] || !w || i == heldpos || (w->pri.kind != TW_KIND_HITSCAN && w->pri.kind != TW_KIND_BEAM) || !TouchWep_Usable(w, dist))
				continue;
			u = TouchWep_Utility(names[i], dist, TW_CLASS_AIR, 0, TW_SWITCH_COST);
			if (u > bu)
			{
				bu = u;
				best = i;
			}
		}
		if (best < 0)
			combo = false;
	}
	// offline only: a sure one-shot kill, or a splash weapon for a target behind a wall
	if (!combo && near->entnum && (i = TouchTactics_PickWeapon(near->entnum, dist, names, ready, n, heldpos)) >= 0)
	{
		if (i == heldpos)
			return;
		best = i;
		combo = true;
	}
	if (!combo && smart)
	{
		if (!escape && t && now - t->cls_since < CLASS_SETTLE)
			return; // how the target moves is not settled yet
		group = near->entnum ? group_near(near) : 0;
		best = TouchWep_ChooseSmart(names, ready, n, heldpos, dist, mclass, group, cl_touch_autoswitch_lastresort.integer != 0, &bu, &hu);
		if (best < 0 || best == heldpos)
			return;
		if (!(escape && !TouchWep_IsFallback(names[best])) && !TouchWep_ShouldSwitchSmart(hu, cur_ok, bu))
			return;
	}
	else if (!combo)
	{
		best = TouchWep_Choose(names, ready, n, dist, cl_touch_autoswitch_lastresort.integer != 0, &rank);
		if (best < 0 || index[best] == held)
			return;
		cur_rank = TouchWep_Rank(wep->netname, dist);
		if (!(escape && !TouchWep_IsFallback(names[best])) && !TouchWep_ShouldSwitch(cur_rank, cur_ok, rank))
			return;
	}
	// never trade a weapon that can still fire for a last resort
	if (cl_touch_autoswitch_lastresort.integer && TouchWep_IsFallback(names[best]) && cur_ok)
		return;
	Cbuf_AddText(cmd_local, va(vabuf, sizeof(vabuf), "weapon_%s\n", names[best]));
	if (cl_touch_aim_debug.integer)
	{
		if (escape && !TouchWep_IsFallback(names[best]))
			Con_Printf("touch aim: switch %s -> %s at %.0f units (leaving a last-resort weapon%s)\n", wep->netname, names[best], dist,
				dropped ? " the server switched to when the last one ran dry" : "");
		else if (combo)
			Con_Printf("touch aim: combo: player %d in the air after our blast: %s -> %s at %.0f units\n", near->entnum, wep->netname, names[best], dist);
		else if (smart)
			Con_Printf("touch aim: switch %s -> %s at %.0f units (target %s, %d more in reach of a blast; expected damage %.0f vs %.0f%s; %.1f s after the player's last pick)\n",
				wep->netname, names[best], dist, touch_wep_class_name[mclass], group, bu, hu, cur_ok ? "" : ", current cannot shoot here", now - ta.manual_switch);
		else
			Con_Printf("touch aim: switch %s -> %s at %.0f units (%s; rank %d -> %d; %.1f s after the player's last pick)\n", wep->netname, names[best], dist,
				!cur_ok ? "current cannot shoot here" : cur_rank < 0 ? "current not on the list" : "clearly better", cur_rank, rank, now - ta.manual_switch);
	}
	ta.switch_to = index[best];
	ta.switch_asked = now;
	ta.own_switch = now;
}

static int projectile_type_raw(prvm_edict_t *ed)
{
	prvm_eval_t *v = PRVM_EDICTFIELDVALUE(ed, ta.projtype_ofs);
	return ta.projtype_type == ev_float ? (int)v->_float : v->_int;
}

// The csqcprojectile_type of a CSQC projectile entity, -1 for anything else.
static int projectile_type(prvm_edict_t *ed)
{
	prvm_prog_t *prog = CLVM_prog;
	prvm_eval_t *v;
	int type;
	if (ta.projtype_ofs < 0)
		return -1;
	v = PRVM_EDICTFIELDVALUE(ed, ta.projtype_ofs);
	type = ta.projtype_type == ev_float ? (int)v->_float : v->_int;
	if (ta.projtype_is_cnt && type == PROJECTILE_ROCKET && strcmp(PRVM_GetString(prog, PRVM_clientedictstring(ed, model)), ROCKET_MODEL))
		return -1;
	return type;
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

// ---------------------------------------------------------------------------
// Offline practice: a splash shot at an enemy behind cover
// ---------------------------------------------------------------------------
// The shot is simulated as the server flies it -- a straight box trace, or
// a lob stepped under gravity -- to its first impact. It counts only where
// that impact bursts within reach of the body, with a clear line from the
// blast to it (RadiusDamage), well away from the player, and not on sky (a
// shot that hits sky vanishes). Candidate burst points ring the enemy where
// it will be when the shot lands; the best is cached for HB_CACHE.
#define HB_BOX     4.0f   // half size of the projectile's box
#define HB_STEP    0.04f  // lob simulation step, seconds
#define HB_TMAX    2.5f   // longest lob followed
#define HB_CACHE   0.05   // seconds a found aim is kept
#define HB_REACH   0.6f   // of the splash radius: the farthest burst that counts
typedef struct hburst_s
{
	int ent;
	double until;
	qbool valid;
	float yaw, pitch, score;
	vec3_t impact;
} hburst_t;
static hburst_t hb;

// First impact of a shot fired along (yaw, pitch), and its flight time;
// false when it hits nothing in range, starts in a wall, or hits sky.
static qbool hb_impact(const vec3_t eye, const touch_wep_fire_t *f, float yaw, float pitch, float reach, vec3_t impact, float *tof)
{
	static const vec3_t mins = { -HB_BOX, -HB_BOX, -HB_BOX }, maxs = { HB_BOX, HB_BOX, HB_BOX };
	vec3_t ang, fwd, up, a, b, v;
	trace_t tr;
	float t, g = cl.movevars_gravity;
	VectorSet(ang, pitch, yaw, 0);
	AngleVectors(ang, fwd, NULL, up);
	if (f->kind != TW_KIND_BALLISTIC)
	{
		VectorMA(eye, reach, fwd, b);
		tr = CL_TraceBox(eye, mins, maxs, b, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false);
		if (tr.startsolid || tr.fraction >= 1 || (tr.hitq3surfaceflags & (Q3SURFACEFLAG_SKY | Q3SURFACEFLAG_NOIMPACT)))
			return false;
		VectorCopy(tr.endpos, impact);
		*tof = f->speed > 0 ? VectorDistance(eye, impact) / f->speed : 0;
		return true;
	}
	VectorScale(fwd, f->speed, v);
	VectorMA(v, f->speed_up, up, v);
	VectorCopy(eye, a);
	for (t = 0; t < HB_TMAX; t += HB_STEP)
	{
		b[0] = a[0] + v[0] * HB_STEP;
		b[1] = a[1] + v[1] * HB_STEP;
		b[2] = a[2] + v[2] * HB_STEP - 0.5f * g * HB_STEP * HB_STEP;
		v[2] -= g * HB_STEP;
		tr = CL_TraceBox(a, mins, maxs, b, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false);
		if (tr.startsolid)
			return false;
		if (tr.fraction < 1)
		{
			if (tr.hitq3surfaceflags & (Q3SURFACEFLAG_SKY | Q3SURFACEFLAG_NOIMPACT))
				return false;
			VectorCopy(tr.endpos, impact);
			*tof = t + tr.fraction * HB_STEP;
			return true;
		}
		VectorCopy(b, a);
	}
	return false;
}

// What a burst at p does to a body box (bmin, bmax) with centre c: 0 when it
// is out of reach, too near the player, or the blast has no line to the
// body, else 1 at the body falling to 0 at HB_REACH of the radius.
static float hb_value(const vec3_t p, const vec3_t eye, const vec3_t c, const vec3_t bmin, const vec3_t bmax, float splash)
{
	float gap = TouchWep_BoxGap(p, bmin, bmax);
	vec3_t q, d, hd;
	if (gap > HB_REACH * splash || VectorDistance(p, eye) < splash + 32)
		return 0;
	// the blast's line to the body, from just off the surface it hit
	VectorSubtract(c, p, d);
	VectorNormalize(d);
	VectorMA(p, 2, d, q);
	VectorCopy(c, hd);
	hd[2] = bmax[2] - 6;
	if (!line_clear(q, c) && !line_clear(q, hd))
		return 0;
	return 1 - gap / (HB_REACH * splash);
}

// Launch angles for a lob through p: low arc, or high (over cover).
static qbool hb_lob(const vec3_t eye, const vec3_t p, const touch_wep_fire_t *f, qbool high, float *yaw, float *pitch)
{
	float dx = sqrt((p[0] - eye[0]) * (p[0] - eye[0]) + (p[1] - eye[1]) * (p[1] - eye[1])), dz = p[2] - eye[2];
	float v2 = f->speed * f->speed + f->speed_up * f->speed_up, g = cl.movevars_gravity, disc, phi;
	if (dx < 1 || g <= 0)
		return false;
	disc = v2 * v2 - g * (g * dx * dx + 2 * dz * v2);
	if (disc < 0)
		return false;
	phi = atan((v2 + (high ? 1 : -1) * sqrt(disc)) / (g * dx)) - atan2(f->speed_up, f->speed);
	*yaw = atan2(p[1] - eye[1], p[0] - eye[0]) * (180.0f / M_PI);
	*pitch = -phi * (180.0f / M_PI);
	return *pitch > -85 && *pitch < 85;
}

// The enemy's body after t seconds (exact velocity offline), kept out of
// walls it would run into: centre and box.
static void hb_where(const aimtarget_t *near, float t, vec3_t c, vec3_t bmin, vec3_t bmax)
{
	const track_t *trk = &ta.tracks[near->entnum & (TRACK_MAX - 1)];
	vec3_t mv;
	VectorScale(near->vel, t, mv);
	mv[2] = 0;
	clip_lead(near->center, mv);
	VectorAdd(near->center, mv, c);
	VectorAdd(trk->bmin, mv, bmin);
	VectorAdd(trk->bmax, mv, bmax);
}

// Candidate i's burst point round body (c, bmin, bmax): 0 the body, 1-2
// above it, 3 the floor under it, 4-19 a ring of 8 at the feet at two radii.
static qbool hb_point(int i, const vec3_t c, const vec3_t bmin, const vec3_t bmax, float splash, vec3_t p)
{
	VectorCopy(c, p);
	if (i == 1 || i == 2)
		p[2] = bmax[2] + 24 * i;
	else if (i == 3)
	{
		vec3_t down;
		trace_t tr;
		VectorCopy(c, down);
		down[2] -= 256;
		tr = CL_TraceLine(c, down, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, true, false);
		if (tr.startsolid || tr.fraction >= 1)
			return false;
		VectorCopy(tr.endpos, p);
	}
	else if (i >= 4)
	{
		float a = (i & 7) * (float)(M_PI / 4), r = (i < 12 ? 0.3f : 0.55f) * splash + 16;
		p[0] += r * cos(a);
		p[1] += r * sin(a);
		p[2] = bmin[2] + 4;
	}
	return true;
}

// Fires the burst search models: a straight projectile or a lob that bursts
// on its first impact, with a tight spread. Not mines (they stick), the
// bouncing lobs (Mortar and Electro secondary), or a scatter.
static qbool hb_fire_ok(const touch_wep_t *w, const touch_wep_fire_t *f)
{
	if (!w || !f || f->splash < 60 || f->spread > 0.05f || (w->flags & (TW_MINES | TW_GUIDED)))
		return false;
	if (f->kind == TW_KIND_PROJ)
		return true;
	return f->kind == TW_KIND_BALLISTIC && f == &w->pri;
}

// Search the burst points round a hidden enemy for this fire (both arcs of
// a lob); the best aim goes into hb.
static void hb_search(const vec3_t eye, const aimtarget_t *near, const touch_wep_fire_t *f, float latency)
{
	vec3_t c, bmin, bmax, p, imp;
	float best = 0, t, tof, yaw = 0, pitch = 0, v;
	int i, arc, pass;
	qbool ok;
	hb.valid = false;
	hb.ent = near->entnum;
	hb.until = host.realtime + HB_CACHE;
	for (arc = 0; arc < (f->kind == TW_KIND_BALLISTIC ? 2 : 1); arc++)
		for (i = 0; i < 20; i++)
		{
			// where the enemy is when the shot lands: the flight time of
			// the first solve sets the meeting point of the second
			t = latency + (f->speed > 0 ? near->dist / f->speed : 0);
			ok = false;
			for (pass = 0; pass < 2; pass++)
			{
				hb_where(near, t, c, bmin, bmax);
				if (!hb_point(i, c, bmin, bmax, f->splash, p))
					break;
				if (f->kind != TW_KIND_BALLISTIC)
					angles_to(eye, p, &yaw, &pitch);
				else if (!hb_lob(eye, p, f, arc != 0, &yaw, &pitch))
					break;
				ok = hb_impact(eye, f, yaw, pitch, VectorDistance(eye, p) + f->splash, imp, &tof);
				if (!ok)
					break;
				t = latency + tof;
			}
			if (!ok)
				continue;
			hb_where(near, t, c, bmin, bmax);
			v = hb_value(imp, eye, c, bmin, bmax, f->splash);
			if (v <= 0)
				continue;
			// of two as good, the one nearer the view now (on target sooner)
			v -= 0.002f * (fabs(wrap180(yaw - cl.viewangles[YAW])) + fabs(pitch - cl.viewangles[PITCH]));
			if (v > best)
			{
				best = v;
				hb.valid = true;
				hb.yaw = yaw;
				hb.pitch = pitch;
				hb.score = v;
				VectorCopy(imp, hb.impact);
			}
		}
}

// Whether a shot from the view as it is now bursts where it counts.
static qbool hb_view_ok(const vec3_t eye, const aimtarget_t *near, const touch_wep_fire_t *f, float latency)
{
	vec3_t imp, c, bmin, bmax;
	float tof;
	if (!hb_impact(eye, f, cl.viewangles[YAW], cl.viewangles[PITCH], near->dist + 2 * f->splash, imp, &tof))
		return false;
	hb_where(near, latency + tof, c, bmin, bmax);
	return hb_value(imp, eye, c, bmin, bmax, f->splash) > 0;
}

// Splash weapons and groups (cl_touch_autoshoot_group): when two or more
// visible enemies stand near the target, score blast points -- the previous
// one, each enemy's floor point, the floor between each close pair -- by
// the splash they deal to every visible enemy where they will be when the
// shot lands (the aimed-at one counting 1.5, teammates against), and aim at
// the best if it beats the single-target point by TW_GROUP_HYSTERESIS.
static void group_aim(aimtarget_t *near, const touch_wep_fire_t *f, const vec3_t eye, float latency)
{
	touch_wep_body_t bodies[GROUP_MAX];
	float cands[1 + 1 + GROUP_MAX + GROUP_MAX * (GROUP_MAX - 1) / 2][3];
	float feet[GROUP_MAX][3];
	int isfeet[GROUP_MAX];
	int ncand = 0, nb = 0, i, j, cur = -1, own_i, best, nenemy = 0, hit = 0, near_i = -1;
	float T = near->tof + latency, R = f->splash, core = f->damage, edge = 0.5f * f->damage, sc, own_sc, yaw, pitch;
	vec3_t aim, p;
	for (i = 0; i < num_gbodies; i++)
	{
		const gbody_t *g = &gbodies[i];
		const track_t *t = &ta.tracks[g->n & (TRACK_MAX - 1)];
		touch_wep_body_t *b = &bodies[nb];
		int k;
		for (k = 0; k < 3; k++)
		{
			float shift = k < 2 ? t->vel[k] * T : 0;
			b->bmin[k] = g->bmin[k] + shift - eye[k];
			b->bmax[k] = g->bmax[k] + shift - eye[k];
		}
		b->weight = !g->enemy ? -0.5f : g->n == near->entnum ? 1.5f : 1;
		if (g->enemy)
		{
			feet[nenemy][0] = 0.5f * (b->bmin[0] + b->bmax[0]);
			feet[nenemy][1] = 0.5f * (b->bmin[1] + b->bmax[1]);
			feet[nenemy][2] = b->bmin[2] + 4;
			isfeet[nenemy] = b->bmin[2] < -16; // feet below the eye: the floor is in sight
			if (!isfeet[nenemy])
				feet[nenemy][2] = 0.5f * (b->bmin[2] + b->bmax[2]);
			if (g->n == near->entnum)
				near_i = nenemy;
			nenemy++;
		}
		nb++;
	}
	if (nenemy < 2 || near_i < 0)
	{
		ta.group_valid = false;
		return;
	}
	if (ta.group_valid)
	{
		VectorSubtract(ta.group_point, eye, cands[ncand]);
		cur = ncand++;
	}
	// the single-target point: where the shot meets the target (for a lob,
	// whose aim point only gives the launch direction, the target's floor point)
	if (f->kind == TW_KIND_BALLISTIC)
		VectorCopy(feet[near_i], cands[ncand]);
	else
	{
		VectorAdd(near->center, near->lead, p);
		VectorSubtract(p, eye, cands[ncand]);
	}
	own_i = ncand++;
	for (i = 0; i < nenemy; i++)
		if (f->kind != TW_KIND_BALLISTIC || i != near_i)
			VectorCopy(feet[i], cands[ncand++]);
	for (i = 0; i < nenemy; i++)
		for (j = i + 1; j < nenemy; j++)
		{
			float dx = feet[i][0] - feet[j][0], dy = feet[i][1] - feet[j][1];
			if (sqrt(dx * dx + dy * dy) < 2 * R + 32)
			{
				VectorMAM(0.5f, feet[i], 0.5f, feet[j], cands[ncand]);
				ncand++;
			}
		}
	// a point the shot cannot reach scores nothing
	for (i = 0; i < ncand; i++)
	{
		if (i == own_i || i == cur)
			continue;
		VectorAdd(eye, cands[i], p);
		if (!line_clear(eye, p))
			VectorSet(cands[i], 1e6f, 1e6f, 1e6f);
	}
	best = TouchWep_BestImpact((const float (*)[3])cands, ncand, bodies, nb, core, edge, R, cur, &sc);
	own_sc = TouchWep_GroupScore(cands[own_i], bodies, nb, core, edge, R);
	// the group point must beat the target alone: by the hysteresis margin
	// to take over, at all to stay
	if (best < 0 || best == own_i || sc < own_sc * (best == cur ? 1.0f : TW_GROUP_HYSTERESIS))
	{
		ta.group_valid = false;
		return;
	}
	for (i = 0; i < nb; i++)
		if (bodies[i].weight > 0 && TouchWep_SplashDamage(TouchWep_BoxGap(cands[best], bodies[i].bmin, bodies[i].bmax), core, edge, R) > 0)
			hit++;
	// aim there: straight at it, or the lob that comes down on it
	VectorCopy(cands[best], aim);
	if (f->kind == TW_KIND_BALLISTIC)
	{
		vec3_t zero;
		float tof;
		VectorClear(zero);
		if (!TouchWep_AimPoint(f, cands[best], zero, 0, cl.movevars_gravity, aim, &tof))
		{
			ta.group_valid = false;
			return;
		}
	}
	VectorAdd(eye, cands[best], ta.group_point);
	ta.group_valid = true;
	VectorAdd(eye, aim, p);
	VectorSubtract(p, near->center, near->lead);
	angles_to(eye, p, &yaw, &pitch);
	near->yaw = yaw;
	near->pitch = pitch;
	near->err = edge_err(wrap180(yaw - cl.viewangles[YAW]), pitch - cl.viewangles[PITCH], near->rh, near->rv);
	near->on_lead = near->err <= 0;
	near->has_lead = true;
	if (cl_touch_aim_debug.integer && (hit != ta.group_said || cl_touch_aim_debug.integer >= 3))
		Con_Printf("touch aim: group: blast point %.0f %.0f %.0f catches %d of %d enemies (score %.0f, aimed-at player alone %.0f)\n",
			ta.group_point[0], ta.group_point[1], ta.group_point[2], hit, nenemy, sc, own_sc);
	ta.group_said = hit;
}

// Visible enemies collected this frame.
static int group_enemies(void)
{
	int i, c = 0;
	for (i = 0; i < num_gbodies; i++)
		if (gbodies[i].enemy)
			c++;
	return c;
}

// Remote detonation by the damage to every visible enemy (TouchWep_DetonateGroup),
// used when two or more are in sight; one alone keeps the single-target rule.
static qbool detonate_group(float radius, const vec3_t eye, float dt)
{
	touch_wep_body_t bodies[GROUP_MAX];
	float vels[GROUP_MAX][3];
	vec3_t smin, smax;
	int i, nb = 0;
	for (i = 0; i < num_gbodies; i++)
	{
		VectorCopy(gbodies[i].bmin, bodies[nb].bmin);
		VectorCopy(gbodies[i].bmax, bodies[nb].bmax);
		bodies[nb].weight = gbodies[i].enemy ? 1 : -0.5f;
		VectorCopy(ta.tracks[gbodies[i].n & (TRACK_MAX - 1)].vel, vels[nb]);
		nb++;
	}
	VectorSet(smin, eye[0] - 16, eye[1] - 16, eye[2] - 59);
	VectorSet(smax, eye[0] + 16, eye[1] + 16, eye[2] + 10);
	return TouchWep_DetonateGroup(ta.rk_org, ta.rk_vel, bodies, (const float (*)[3])vels, nb, smin, smax, radius, shot_latency(), dt);
}

// Path check (cl_touch_autoshoot_clear): fly the rocket ahead the way the
// server steers it -- toward the view ray from eye through way (until it is
// passed) and then aim, turning at most ROCKET_GUIDE_RATE -- against the
// world only. ok: it gets to the blast point aim (passes within a quarter of
// the splash radius, or bursts on something within 0.6 of it).
typedef struct rkpath_s { qbool ok, hit_world; float miss, t; vec3_t end; } rkpath_t;
static void rk_simulate(const vec3_t org0, const vec3_t vel0, const vec3_t eye, const float *way, const vec3_t aim, float splash, rkpath_t *r)
{
	static const vec3_t mins = { -2, -2, -2 }, maxs = { 2, 2, 2 };
	vec3_t p, np, dir, view, seg, toaim, c;
	float sp = VectorLength(vel0), best;
	qbool via = way != NULL;
	int i;
	memset(r, 0, sizeof(*r));
	r->miss = 1e9f;
	if (sp < 1)
		return;
	VectorCopy(org0, p);
	VectorScale(vel0, 1.0f / sp, dir);
	best = VectorDistance(p, aim);
	for (i = 0; i < PLAN_STEPS; i++)
	{
		trace_t tr;
		float d, u, l2;
		if (via)
		{
			vec3_t a, b;
			VectorSubtract(p, way, a);
			VectorSubtract(aim, way, b);
			if (DotProduct(a, b) > 0 || VectorLength(a) < 96)
				via = false;
		}
		VectorSubtract(via ? way : aim, eye, view);
		VectorNormalize(view);
		VectorCopy(p, np);
		TouchWep_GuideStep(np, dir, &sp, eye, view, ROCKET_GUIDE_GOAL, ROCKET_GUIDE_RATE, ROCKET_SPEED, ROCKET_SPEED, PLAN_DT);
		tr = CL_TraceBox(p, mins, maxs, np, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false);
		if (tr.startsolid) // grazing something already: judge by the centre line
			tr = CL_TraceLine(p, np, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false, false);
		VectorSubtract(tr.endpos, p, seg);
		VectorSubtract(aim, p, toaim);
		l2 = DotProduct(seg, seg);
		u = l2 > 1e-6f ? bound(0.0f, DotProduct(toaim, seg) / l2, 1.0f) : 0;
		VectorMA(p, u, seg, c);
		d = VectorDistance(c, aim);
		best = min(best, d);
		r->t += PLAN_DT * tr.fraction;
		VectorCopy(tr.endpos, r->end);
		if (d < 0.25f * splash)
		{
			r->ok = true;
			break;
		}
		if (tr.fraction < 1 || tr.startsolid)
		{
			r->hit_world = true;
			r->ok = VectorDistance(tr.endpos, aim) < 0.6f * splash;
			break;
		}
		if (!via && i > 0 && VectorDistance(np, aim) > best + 1)
		{
			r->ok = best < 0.5f * splash; // passing by
			break;
		}
		VectorCopy(np, p);
	}
	r->miss = best;
}

// Where to steer the rocket so it gets to aim: aim itself while the path
// there is clear, else a waypoint over, beside or above what it would hit
// (the first of a few that the simulated flight clears), then back onto aim.
static void rk_steer_point(const vec3_t eye, const vec3_t aim, float splash, qbool floor_aim, vec3_t out)
{
	static const float offs[][2] = { { 48, 0 }, { 96, 0 }, { 0, 64 }, { 0, -64 }, { 160, 0 }, { 0, 128 }, { 0, -128 }, { 96, 96 }, { 96, -96 }, { 240, 0 } }; // up, side
	double now = host.realtime;
	VectorCopy(aim, out);
	// (only once the rocket's own motion is known: a velocity guessed from
	// its first frames would fly the simulation into the floor)
	if (!cl_touch_autoshoot_clear.integer || VectorLength(ta.rk_vel) < 800 || now - ta.rk_launch_seen < PLAN_SETTLE)
	{
		ta.rk_way_on = false;
		return;
	}
	if (ta.rk_way_on)
	{
		vec3_t a, b;
		VectorSubtract(ta.rk_org, ta.rk_way, a);
		VectorSubtract(aim, ta.rk_way, b);
		if (DotProduct(a, b) > 0 || VectorLength(a) < 96)
		{
			ta.rk_way_on = false;
			ta.rk_plan_next = 0;
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch aim: rocket path: waypoint passed, on to the target\n");
		}
	}
	if (now >= ta.rk_plan_next)
	{
		rkpath_t direct, c;
		ta.rk_plan_next = now + PLAN_EVERY;
		rk_simulate(ta.rk_org, ta.rk_vel, eye, NULL, aim, splash, &direct);
		// (a rocket aimed at the floor is meant to hit it: only something
		// clearly short of the floor point counts as in the way)
		if (direct.ok || !direct.hit_world || (floor_aim && VectorDistance(direct.end, aim) < 1.5f * splash))
		{
			if (ta.rk_way_on && cl_touch_aim_debug.integer)
				Con_Printf("touch aim: rocket path: clear to the target again\n");
			ta.rk_way_on = false;
		}
		else if (!ta.rk_way_on || (rk_simulate(ta.rk_org, ta.rk_vel, eye, ta.rk_way, aim, splash, &c), !c.ok))
		{
			vec3_t fwd, side, base, cand, best_way = { 0, 0, 0 }, up = { 0, 0, 1 };
			float best_miss = direct.miss - 32;
			int i, best = -1;
			qbool found = false;
			VectorSubtract(aim, ta.rk_org, fwd);
			fwd[2] = 0;
			VectorNormalize(fwd);
			VectorSet(side, -fwd[1], fwd[0], 0);
			VectorMA(direct.end, -48, fwd, base); // a step back from where it would hit
			for (i = 0; i < (int)(sizeof(offs) / sizeof(offs[0])); i++)
			{
				VectorMA(base, offs[i][0], up, cand);
				VectorMA(cand, offs[i][1], side, cand);
				if (!line_clear(ta.rk_org, cand))
					continue;
				rk_simulate(ta.rk_org, ta.rk_vel, eye, cand, aim, splash, &c);
				if (c.ok || c.miss < best_miss)
				{
					best = i;
					best_miss = c.miss;
					VectorCopy(cand, best_way);
					if (c.ok)
					{
						found = true;
						break;
					}
				}
			}
			// only a way round that gets there: a guess that does not is
			// worse than flying straight at the target
			if (best >= 0 && found)
			{
				if (cl_touch_aim_debug.integer && (!ta.rk_way_on || ta.rk_way_said != best))
					Con_Printf("touch aim: rocket path: would hit something %.0f units from the target in %.2f s; steering via a point %.0f up, %.0f to the side\n",
						VectorDistance(direct.end, aim), direct.t, offs[best][0], offs[best][1]);
				ta.rk_way_said = best;
				ta.rk_way_on = true;
				VectorCopy(best_way, ta.rk_way);
			}
			else
				ta.rk_way_on = false;
		}
	}
	if (ta.rk_way_on)
		VectorCopy(ta.rk_way, out);
}

// Judge an own rocket that flies straight (cl_touch_autoshoot_volley):
// where it meets the target (as last seen) or the world first.
static void rk_judge_straight(const vec3_t org, const vec3_t vel, const vec3_t tgt, const vec3_t tvel, float splash, qbool *good, qbool *certain, float *t_hit)
{
	vec3_t rel, relv, end, at;
	float t, d;
	trace_t tr;
	VectorSubtract(tgt, org, rel);
	VectorSubtract(tvel, vel, relv);
	d = TouchWep_PassDistance(rel, relv, 3.0f, &t);
	VectorMA(org, t + 0.1f, vel, end);
	tr = CL_TraceLine(org, end, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0, collision_extendmovelength.value, true, false, NULL, false, false);
	if (tr.fraction < 1 && tr.fraction * (t + 0.1f) < t)
	{
		// the world first: a burst on the floor or a wall beside the target still counts
		*t_hit = tr.fraction * (t + 0.1f);
		VectorMA(tgt, *t_hit, tvel, at);
		*good = VectorDistance(tr.endpos, at) < 0.6f * splash;
		*certain = false;
		return;
	}
	*t_hit = t;
	*good = d < 0.4f * splash;
	*certain = d < 24; // through the body
}

// The own rockets flying straight: follow them, drop those gone.
static void free_update(void)
{
	prvm_prog_t *prog = CLVM_prog;
	double now = host.realtime;
	const touch_wep_t *dev = TouchWep_Find("devastator");
	float splash = dev ? dev->pri.splash : 110;
	int i, k = 0;
	for (i = 0; i < ta.num_free; i++)
	{
		freerk_t *f = &ta.freerk[i];
		prvm_edict_t *ed = f->ent > 0 && f->ent < prog->num_edicts ? PRVM_EDICT_NUM(f->ent) : NULL;
		vec3_t org, d;
		if (!ed || ed->free || projectile_type(ed) != PROJECTILE_ROCKET)
		{
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch aim: earlier rocket (edict %d) gone\n", f->ent);
			continue;
		}
		VectorCopy(PRVM_clientedictvector(ed, origin), org);
		VectorSubtract(org, f->org, d);
		if (VectorLength2(d) > 0.01f && now > f->seen)
		{
			VectorScale(d, 1.0f / (float)(now - f->seen), f->vel);
			VectorCopy(org, f->org);
			f->seen = now;
		}
		if (f->target == ta.rk_target && ta.rk_state == RK_FLY)
		{
			VectorCopy(ta.rk_target_org, f->tgt_org);
			VectorCopy(ta.rk_target_vel, f->tgt_vel);
		}
		rk_judge_straight(f->org, f->vel, f->tgt_org, f->tgt_vel, splash, &f->good, &f->certain, &f->t_hit);
		ta.freerk[k++] = *f;
	}
	ta.num_free = k;
}

// Every own rocket out is either gone or certain to hit by itself (a weapon
// switch ends steering and remote detonation of them all).
static qbool free_all_certain(void)
{
	int i;
	for (i = 0; i < ta.num_free; i++)
		if (!ta.freerk[i].certain)
			return false;
	return true;
}

// Hold the guided rocket's blast while an earlier one is still on its way
// to a hit of its own (remote detonation sets off every rocket).
static qbool free_hold(int target)
{
	int i;
	for (i = 0; i < ta.num_free; i++)
		if (ta.freerk[i].good && ta.freerk[i].t_hit > shot_latency())
		{
			if (cl_touch_aim_debug.integer >= 2)
				Con_Printf("touch aim: holding the blast: an earlier rocket hits by itself in %.2f s\n", ta.freerk[i].t_hit);
			return true;
		}
	return false;
}

// An earlier rocket that flies past its target (no hit of its own) at its
// closest now: set them all off (the guided one with it).
static qbool free_detonate(const vec3_t me, float splash)
{
	int i;
	for (i = 0; i < ta.num_free; i++)
	{
		const freerk_t *f = &ta.freerk[i];
		vec3_t late, rel_v;
		if (f->good)
			continue;
		VectorSubtract(f->vel, f->tgt_vel, rel_v);
		VectorMA(f->org, shot_latency(), rel_v, late);
		if (TouchWep_Detonate(late, rel_v, f->tgt_org, me, splash) && !mate_at_blast(late, splash))
		{
			rk_set(RK_DETONATE, va(vabuf, sizeof(vabuf), "earlier rocket %.0f units from player %d, flying past", VectorDistance(f->org, f->tgt_org), f->target));
			return true;
		}
	}
	return false;
}

// Devastator with steering: press fire once lined up, keep it held while the
// rocket flies (the server guides it toward the view ray), detonate it next
// to the enemy with a one-frame attack2, then release fire long enough for
// the next press to launch again. Sets the fire bits and, while a rocket
// flies, where the view should be pulled.
static void steer_devastator(const vec3_t eye, const aimtarget_t *near, qbool want_fire, qbool manual, qbool ammo_more, int mate_block, float *pull_yaw, float *pull_pitch, qbool *pull)
{
	prvm_prog_t *prog = CLVM_prog;
	double now = host.realtime;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	const touch_wep_t *dev = TouchWep_Find("devastator");
	*pull = false;
	if (ta.num_free)
		free_update();
	switch (ta.rk_state)
	{
	case RK_IDLE:
		// (not before the refire of the last rocket is nearly up: a press the
		// server holds back leaves nothing to track, and a detonation timed
		// for a rocket that is not there would set off the next one at the muzzle)
		if (want_fire && !manual && near->entnum && now - ta.rk_launch_seen >= ROCKET_REFIRE - REFIRE_EARLY)
		{
			rockets_before();
			ta.rk_ent = 0;
			ta.rk_target = near->entnum;
			ta.rk_tseen = ta.rk_fired = now;
			ta.rk_way_on = false;
			ta.rk_plan_next = 0;
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
		ta.rk_closest = 1e9f;
		if (ta.rk_ent)
		{
			VectorCopy(PRVM_clientedictvector(PRVM_EDICT_NUM(ta.rk_ent), origin), ta.rk_org);
			VectorClear(ta.rk_vel);
			ta.rk_seen = ta.rk_launch_seen = now;
			rk_set(RK_FLY, va(vabuf, sizeof(vabuf), "tracking own rocket, edict %d", ta.rk_ent));
		}
		else if (now - max(ta.rk_time, ta.rk_launch_seen + ROCKET_REFIRE - REFIRE_EARLY) > ROCKET_DEAD_RECKON)
		{
			// none seen (yet): it is on its way along the view, steered there
			ta.rk_launch = ta.rk_time;
			rk_set(RK_FLY, "no rocket of ours seen: dead reckoning along the view");
		}
		return;
	case RK_FLY:
	{
		prvm_edict_t *ed = ta.rk_ent > 0 && ta.rk_ent < prog->num_edicts ? PRVM_EDICT_NUM(ta.rk_ent) : NULL;
		vec3_t org, d, rel_v, me, rk_late;
		float sp;
		if (!ta.rk_ent && now - ta.rk_launch < ROCKET_FIND_TIME)
		{
			// a late rocket (the weapon was still reloading): track it after all
			ta.rk_ent = find_own_rocket(eye);
			if (ta.rk_ent)
			{
				ed = PRVM_EDICT_NUM(ta.rk_ent);
				VectorCopy(PRVM_clientedictvector(ed, origin), ta.rk_org);
				VectorClear(ta.rk_vel);
				ta.rk_seen = ta.rk_launch_seen = now;
				if (cl_touch_aim_debug.integer)
					Con_Printf("touch aim: rocket seen after all, edict %d\n", ta.rk_ent);
			}
		}
		if (!ta.rk_ent)
		{
			// dead reckoning: the server steers it onto the view ray, at the
			// Devastator's speed since the press
			float t = (float)(now - ta.rk_launch), flown;
			vec3_t fwd;
			AngleVectors(cl.viewangles, fwd, NULL, NULL);
			flown = dev ? TouchWep_FlightDist(&dev->pri, t) : 1000 * t;
			VectorMA(eye, flown, fwd, ta.rk_org);
			VectorScale(fwd, dev ? min(dev->pri.speed_max, dev->pri.speed + dev->pri.accel * t) : 1000, ta.rk_vel);
			if (flown > VectorDistance(eye, ta.rk_target_org) + 2 * (dev ? dev->pri.splash : 110))
			{
				rk_set(RK_RELEASE, "dead-reckoned rocket should have arrived");
				set_firing(false, false, 0);
				return;
			}
		}
		else if (!ed || ed->free || projectile_type(ed) != PROJECTILE_ROCKET)
		{
			rk_set(RK_RELEASE, va(vabuf, sizeof(vabuf), "rocket gone: hit something; it came within %.0f units of the target", ta.rk_closest));
			set_firing(false, false, 0);
			return;
		}
		else
		{
			VectorCopy(PRVM_clientedictvector(ed, origin), org);
			VectorSubtract(org, ta.rk_org, d);
			if (VectorLength2(d) > 0.01f && now > ta.rk_seen)
			{
				// its velocity: networked for a projectile the CSQC moves
				// itself, else from its interpolated motion, smoothed
				float f = bound(0.0f, (float)(now - ta.rk_seen) / 0.05f, 1.0f);
				vec3_t v;
				VectorCopy(PRVM_clientedictvector(ed, velocity), v);
				if (VectorLength2(v) < 100 * 100)
				{
					VectorScale(d, 1.0f / (float)(now - ta.rk_seen), v);
					VectorLerp(ta.rk_vel, f, v, ta.rk_vel);
				}
				else
					VectorCopy(v, ta.rk_vel);
				VectorCopy(org, ta.rk_org);
				ta.rk_seen = now;
			}
		}
		if (now - ta.rk_time > ROCKET_MAX_FLIGHT)
		{
			rk_set(RK_RELEASE, "flight too long");
			set_firing(false, false, 0);
			return;
		}
		// follow the target while it is still the one in view; out of sight,
		// where it was last seen (on its last velocity for a moment: what
		// the player knew), never where only the network says it is
		if (near->entnum == ta.rk_target)
		{
			VectorCopy(near->center, ta.rk_target_org);
			VectorCopy(near->vel, ta.rk_target_vel);
			ta.rk_tseen = now;
		}
		else if (!cl_touch_autoshoot_clear.integer || now - ta.rk_tseen < LAST_SEEN_EXTRAP)
			VectorMA(ta.rk_target_org, dt, ta.rk_target_vel, ta.rk_target_org);
		else
			VectorClear(ta.rk_target_vel);
		// gone behind cover (and no offline see-through): steering on would
		// only fly the rocket, and the view, into the wall it went behind
		if (near->entnum != ta.rk_target && !practice.seethrough && now - ta.rk_tseen >= LAST_SEEN_EXTRAP
			&& !line_clear(ta.rk_org, ta.rk_target_org) && !line_clear(eye, ta.rk_target_org))
		{
			rk_set(RK_RELEASE, va(vabuf, sizeof(vabuf), "player %d went behind cover", ta.rk_target));
			set_firing(false, false, 0);
			return;
		}
		VectorSubtract(ta.rk_vel, ta.rk_target_vel, rel_v);
		ta.rk_closest = min(ta.rk_closest, VectorDistance(ta.rk_org, ta.rk_target_org));
		Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, me);
		// judged where the server will have rocket and target when the press
		// arrives: the screen shows both that much late
		VectorMA(ta.rk_org, cl_touch_autoshoot_predict.integer ? shot_latency() : 0, rel_v, rk_late);
		// (only a rocket of ours that is seen is set off: a dead-reckoned
		// one may not be where it is guessed, or not out yet)
		if (dev && ta.rk_ent && (cl_touch_autoshoot_group.integer && group_enemies() >= 2
			? detonate_group(dev->pri.splash, me, dt)
			: TouchWep_Detonate(rk_late, rel_v, ta.rk_target_org, me, dev->pri.splash))
			&& !free_hold(ta.rk_target) && !mate_at_blast(rk_late, dev->pri.splash))
		{
			rk_set(RK_DETONATE, va(vabuf, sizeof(vabuf), "rocket %.0f units from player %d", VectorDistance(ta.rk_org, ta.rk_target_org), ta.rk_target));
			// attack stays held this frame: with the rocket already out it
			// launches nothing, and attack2 sets off every rocket of ours
			set_firing(true, true, ta.rk_target);
			return;
		}
		// an earlier rocket flying past (not on a hitting line) is set off at
		// its closest: the blast takes this one with it
		if (dev && ta.num_free && free_detonate(me, dev->pri.splash))
		{
			set_firing(true, true, ta.rk_target);
			return;
		}
		// the next rocket while this one flies, once this one hits by itself
		if (cl_touch_autoshoot_volley.integer && ta.rk_ent && ammo_more && !mate_block && ta.num_free < FREE_MAX
			&& now - ta.rk_fired >= ROCKET_REFIRE - ROCKET_RELEASE && dev)
		{
			qbool good, certain;
			float t_hit;
			const track_t *vt = &ta.tracks[ta.rk_target & (TRACK_MAX - 1)];
			rk_judge_straight(ta.rk_org, ta.rk_vel, ta.rk_target_org, ta.rk_target_vel, dev->pri.splash, &good, &certain, &t_hit);
			// let it fly straight only when that is safe: a standing
			// target, or a hit within VOLLEY_SURE -- a strafing enemy that
			// turns while an unguided rocket is still far out is lost
			if (good && t_hit > 0.2f && ((certain && t_hit < VOLLEY_SURE) || (vt->cls == TW_CLASS_STAND && VectorLength(ta.rk_target_vel) < STAND_SPEED)))
			{
				freerk_t *f = &ta.freerk[ta.num_free++];
				f->ent = ta.rk_ent;
				VectorCopy(ta.rk_org, f->org);
				VectorCopy(ta.rk_vel, f->vel);
				f->seen = ta.rk_seen;
				f->target = ta.rk_target;
				VectorCopy(ta.rk_target_org, f->tgt_org);
				VectorCopy(ta.rk_target_vel, f->tgt_vel);
				f->good = good;
				f->certain = certain;
				f->t_hit = t_hit;
				rockets_before();
				ta.rk_ent = 0;
				rk_set(RK_VOLLEY, va(vabuf, sizeof(vabuf), "rocket on a line that %s in %.2f s by itself: the next one", certain ? "hits the body" : "bursts beside the target", t_hit));
				set_firing(false, false, 0);
				return;
			}
		}
		set_firing(true, false, ta.rk_target);
		// keep the view on where the rocket will meet the target: the server
		// steers it toward a point on the view ray ahead of it
		sp = VectorLength(ta.rk_vel);
		if (sp > 100)
		{
			// re-solved every frame from the rocket's own position and speed:
			// a grounded target's floor point (a rocket that is not set off
			// then bursts on the ground beside it), an airborne one's body on
			// its fall (an airburst, set off by the detonation rule)
			const track_t *tt = &ta.tracks[ta.rk_target & (TRACK_MAX - 1)];
			touch_wep_fire_t rf;
			vec3_t rel, aim, at;
			float t = 0, t_land = -1;
			qbool floor_aim = false;
			memset(&rf, 0, sizeof(rf));
			rf.kind = TW_KIND_PROJ;
			rf.speed = max(sp, dev ? dev->pri.speed : 1000);
			VectorCopy(ta.rk_target_org, at);
			if (tt->airborne && tt->t_land >= 0)
			{
				if (cl_touch_autoshoot_air.integer)
					t_land = tt->t_land;
			}
			else if (cl_touch_autoshoot_feet.integer && tt->bmin[2] < tt->bmax[2]
				&& (!cl_touch_autoshoot_clear.integer || (VectorDistance(ta.rk_org, ta.rk_target_org) < ROCKET_FLOOR_FINAL
					&& (tt->cls == TW_CLASS_STEADY || tt->cls == TW_CLASS_ERRATIC) && fabs(tt->range_rate) < TW_PRIO_SPEED)))
			{
				// its floor, followed with the body -- for an enemy strafing
				// across the view, where a near miss is likely; one standing,
				// running at you or away is hit directly on the body
				at[2] = tt->bmin[2] + 4 + (ta.rk_target_org[2] - 0.5f * (tt->bmin[2] + tt->bmax[2]));
				floor_aim = true;
			}
			VectorSubtract(at, ta.rk_org, rel);
			if (!cl_touch_autoshoot_air.integer && !cl_touch_autoshoot_feet.integer)
			{
				// the body's straight-line meeting point
				if (!TouchWep_InterceptTime(rel, ta.rk_target_vel, rf.speed, TW_LEAD_MAX_TIME, &t))
					t = 0;
				VectorMA(ta.rk_target_org, t, ta.rk_target_vel, aim);
			}
			else if (TouchWep_AimPointAir(&rf, rel, ta.rk_target_vel, 0, cl.movevars_gravity, t_land, aim, &t))
			{
				VectorAdd(ta.rk_org, aim, aim);
				if (cl_touch_autoshoot_predict.integer)
				{
					// not through a wall the target will stop at
					vec3_t mv;
					VectorSubtract(aim, at, mv);
					clip_lead(ta.rk_target_org, mv);
					aim[0] = at[0] + mv[0];
					aim[1] = at[1] + mv[1];
				}
			}
			else
				VectorCopy(at, aim);
			// offline practice, a target behind a wall: over the cover first,
			// down onto the target only once the rocket is nearly above it
			// (until the rocket itself has a clear line down to it)
			if (practice.seethrough && ta.rk_over_valid && near->entnum == ta.rk_target && near->hidden
				&& !line_clear(ta.rk_org, ta.rk_target_org))
				VectorCopy(ta.rk_over, aim);
			if (cl_touch_autoshoot_group.integer && ta.group_valid)
				VectorCopy(ta.group_point, aim); // the blast point for the whole group
			if (dev)
				rk_steer_point(eye, aim, dev->pri.splash, floor_aim, aim);
			angles_to(eye, aim, pull_yaw, pull_pitch);
			*pull = true;
		}
		return;
	}
	case RK_DETONATE:
		// attack2 stays down until a move carrying it has gone out and the
		// server has had a frame to see it (a one-frame press can fall
		// between two packets, or two moves within one server frame)
		if ((cl.timesincepacket > 0 || now - ta.rk_time < DETONATE_HOLD) && now - ta.rk_time < 0.25)
		{
			set_firing(true, true, ta.rk_target);
			return;
		}
		set_firing(false, false, 0);
		rk_set(RK_RELEASE, "attack2 sent");
		return;
	case RK_VOLLEY:
		// fire released for a moment (ending this rocket's guidance), then
		// pressed again: the server launches the next one once its refire is up
		set_firing(false, false, 0);
		if (now - ta.rk_time >= ROCKET_RELEASE)
		{
			ta.rk_fired = now;
			ta.rk_way_on = false;
			ta.rk_plan_next = 0;
			rk_set(RK_LAUNCH, "next rocket");
			set_firing(true, false, ta.rk_target);
		}
		return;
	case RK_RELEASE:
	default:
		set_firing(false, false, 0);
		if (now - ta.rk_time >= ROCKET_RELEASE)
			rk_set(RK_IDLE, "fire released");
		return;
	}
}

// cl_touch_aim_debug 3: every projectile that appears next to the player is
// taken as one of ours and followed until it is gone; its closest approach to
// the box of the enemy auto-shoot was aiming at is printed, as a measure of
// how well the lead works. Projectiles carry no owner, hence "next to the
// player". Debug only: nothing of this runs otherwise.
static qbool is_projectile(prvm_prog_t *prog, prvm_edict_t *ed)
{
	return !ed->free && !strcmp(PRVM_GetString(prog, PRVM_clientedictstring(ed, classname)), "ENT_CLIENT_PROJECTILE");
}

static float box_dist(const vec3_t p, const vec3_t bmin, const vec3_t bmax)
{
	vec3_t d;
	int k;
	for (k = 0; k < 3; k++)
		d[k] = p[k] < bmin[k] ? bmin[k] - p[k] : p[k] > bmax[k] ? p[k] - bmax[k] : 0;
	return VectorLength(d);
}

static void debug_shots(const vec3_t eye, int target)
{
	prvm_prog_t *prog = CLVM_prog;
	double now = host.realtime;
	int i, k;
	for (k = 0; k < num_shots; k++)
	{
		shot_t *sh = &shots[k];
		prvm_edict_t *ed = sh->ent < prog->num_edicts ? PRVM_EDICT_NUM(sh->ent) : NULL;
		const track_t *t = &ta.tracks[sh->target & (TRACK_MAX - 1)];
		if (ed && is_projectile(prog, ed))
		{
			// along the path flown since the last frame, not only its end
			vec3_t p;
			int f;
			for (f = 1; f <= 4; f++)
			{
				VectorLerp(sh->last, f * 0.25f, PRVM_clientedictvector(ed, origin), p);
				if (sh->target)
					sh->closest = min(sh->closest, box_dist(p, t->bmin, t->bmax));
			}
			VectorCopy(PRVM_clientedictvector(ed, origin), sh->last);
			continue;
		}
		if (sh->target)
			Con_Printf("touch aim: shot %d at player %d%s: closest %.0f units after %.2f s, last seen at %.0f %.0f %.0f\n", sh->type, sh->target, sh->hidden ? " (behind cover)" : "", sh->closest, now - sh->born, sh->last[0], sh->last[1], sh->last[2]);
		if (sh->ent < SHOT_SEEN_MAX)
			shot_seen[sh->ent] = 0;
		shots[k--] = shots[--num_shots];
	}
	for (i = 1; i < prog->num_edicts && i < SHOT_SEEN_MAX; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		if (!is_projectile(prog, ed))
		{
			shot_seen[i] = 0;
			continue;
		}
		if (shot_seen[i])
			continue;
		shot_seen[i] = 1;
		if (num_shots < SHOT_MAX && VectorDistance(PRVM_clientedictvector(ed, origin), eye) < 200)
		{
			shot_t *sh = &shots[num_shots++];
			sh->ent = i;
			sh->type = ta.projtype_ofs >= 0 ? projectile_type_raw(ed) : -1;
			sh->target = target;
			sh->hidden = shot_hidden;
			sh->closest = 1e9f;
			sh->born = now;
			VectorCopy(PRVM_clientedictvector(ed, origin), sh->last);
		}
	}
}

static void aim_frame(void)
{
	double now = host.realtime;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	qbool autoshoot, assist, aim_touch, manual, locked, guided, pull_target_set = false;
	float friction, strength, cone, turn_yaw, turn_pitch, turn_mag, radius, pull_yaw = 0, pull_pitch = 0;
	vec3_t eye, fwd;
	int on_target, friend_on, on_shoot, shootable, held, mode = TW_MODE_PRIMARY, why = TW_WHY_DEFAULT, mate_block;
	qbool was_firing = ta.firing || ta.firing2;
	const touch_wep_t *wep;
	const touch_wep_fire_t *fire = NULL;
	touch_wep_fire_t hitscan_fire;
	float hitscan_lag;
	aimtarget_t near;

	const char *blocked = TouchAim_BlockReason();
	if (blocked)
	{
		if (cl_touch_aim_debug.integer && (ta.was_allowed || !ta.blocked_said || strcmp(ta.blocked_said, blocked)))
			Con_Printf("touch aim: inactive: %s\n", blocked);
		ta.blocked_said = blocked;
		ta.was_allowed = false;
		// nothing offline-only outlives the frame that allowed it
		memset(&practice, 0, sizeof(practice));
		num_marks = 0;
		reset();
		publish();
		return;
	}
	if (!ta.was_allowed && cl_touch_aim_debug.integer)
		Con_Printf("touch aim: active (teamplay %d, my colours %d, %s HUD, %d weapons bound)\n", teamplay_mode(),
			cl.scores[cl.realplayerentity - 1].colors, TouchHUD_Active() ? "engine" : "CSQC", TouchHUD_NumWeapons());
	ta.was_allowed = true;
	ta.blocked_said = NULL;

	// see-through, full 360 and instant turn: only where cl_touch_esp_enabled
	// and the offline gate allow (a local match against bots)
	practice_gate_now = TouchTactics_EspGate(practice_gate());
	practice = TouchWep_PracticeEffective(practice_gate_now, cl_touch_practice_seethrough.integer != 0,
		cl_touch_practice_360.integer != 0 && cl_touch_aim_mode.integer == 2, cl_touch_practice_instant.integer != 0);
	if (cl_touch_aim_debug.integer && practice_gate_now != practice_gate_said)
	{
		if (practice_gate_now == TW_GATE_OPEN)
			Con_Printf("touch aim: practice helpers: on (local match; see-through %d, full 360 %d, instant %d)\n", practice.seethrough, practice.full360, practice.instant);
		else
			Con_Printf("touch aim: practice helpers: off (%s)\n", strcmp(cl_touch_esp_enabled.string, "off") ? touch_practice_why[practice_gate_now] : "cl_touch_esp_enabled is off");
	}
	practice_gate_said = practice_gate_now;
	num_marks = 0;

	// what the server allows (touch_assist.c): all, on a server that says nothing
	autoshoot = cl_touch_autoshoot.integer != 0 && TouchAssist_Allowed(TOUCH_ASSIST_AUTOSHOOT);
	assist = cl_touch_aimassist.integer != 0 && TouchAssist_Allowed(TOUCH_ASSIST_AIM);
	friction = assist ? bound(0.0f, cl_touch_aimassist_friction.value, FRICTION_MAX) : 0;
	strength = assist ? bound(0.0f, cl_touch_aimassist_strength.value, STRENGTH_MAX) : 0;
	// the cone reaches at most the edge of the view: half the horizontal
	// field of view, and never beyond CONE_MAX
	cone = CONE_MAX;
	if (r_refdef.view.frustum_x > 0)
		cone = min(cone, (float)(atan(r_refdef.view.frustum_x) * (180.0 / M_PI)));
	// Field of view mode (and Full 360, which only an offline match gets):
	// anyone on screen; Radius mode: the cone the player set
	if (cl_touch_aim_mode.integer <= 0)
		cone = bound(1.0f, cl_touch_aimassist_cone.value, max(1.0f, cone));
	else if (practice.full360)
		cone = 180; // offline practice: all around
	radius = bound(0.0f, cl_touch_autoshoot_radius.value, RADIUS_MAX);

	// damage our hits did (the hit-sound stat every CSQC keeps), credited
	// to the enemy auto-shoot is fighting: "already hurt" for the target choice
	if (ta.stat_dealt >= 0)
	{
		int total = dealt_stat(), who = ta.fire_target ? ta.fire_target : ta.lock_ent;
		if (total > ta.dealt_last && who > 0)
		{
			track_t *t = &ta.tracks[who & (TRACK_MAX - 1)];
			if (now - t->dealt_time > DEALT_KEEP)
				t->dealt = 0;
			t->dealt += total - ta.dealt_last;
			t->dealt_time = now;
			if (cl_touch_aim_debug.integer >= 2)
				Con_Printf("touch aim: hit for %d on player %d (%.0f lately)\n", total - ta.dealt_last, who, t->dealt);
		}
		ta.dealt_last = total;
	}

	// the weapon in hand, and whether the player picked it
	held = TouchHUD_HeldWeapon();
	wep = held >= 0 ? TouchWep_Find(TouchHUD_WeaponName(held)) : NULL;
	note_held(held, now);

	if (!autoshoot && friction <= 0 && strength <= 0 && !autoswitch_on())
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
	// a fast drag of the player's own: no hysteresis or hold for the old
	// target next, so whatever the view now points nearest wins at once
	if (turn_mag / dt > FLICK_RATE)
	{
		ta.track_ent = 0;
		ta.flick_time = now;
	}
	if (TouchHUD_Active())
		aim_touch = ta.hud_aim_touch;
	else if (cl_touch_csqc_active)
		aim_touch = cl_touch_hud_state.integer > 0;
	else
		aim_touch = false;

	// the fire whose flight the aim leads: last decision's mode
	hitscan_lag = 0;
	if (wep && cl_touch_autoshoot_lead.integer)
	{
		fire = ta.last_mode == TW_MODE_SECONDARY && wep->sec.kind != TW_KIND_NONE ? &wep->sec : &wep->pri;
		if (fire->kind != TW_KIND_PROJ && fire->kind != TW_KIND_BALLISTIC)
		{
			// instant hits: the server's antilag rewinds the target by the
			// ping, at most ANTILAG_MAX; beyond that the hit lands where the
			// target has moved meanwhile, so lead by the uncovered part
			fire = NULL;
			if (cl_touch_autoshoot_predict.integer && latency_estimate() > ANTILAG_MAX)
			{
				memset(&hitscan_fire, 0, sizeof(hitscan_fire));
				hitscan_fire.kind = TW_KIND_PROJ;
				hitscan_fire.speed = 1e6f;
				fire = &hitscan_fire;
				hitscan_lag = latency_estimate() - ANTILAG_MAX;
			}
		}
	}

	ta.head_mult = 0;
	if (wep && cl_touch_autoshoot_head.integer)
	{
		const touch_wep_fire_t *hf = ta.last_mode == TW_MODE_SECONDARY && wep->sec.kind != TW_KIND_NONE ? &wep->sec : &wep->pri;
		ta.head_mult = TouchWep_HeadMultiplier(wep->netname, ta.last_mode == TW_MODE_SECONDARY ? TW_MODE_SECONDARY : TW_MODE_PRIMARY, cl_touch_autoshoot_head_rifle.value);
		ta.head_spread = hf->spread;
	}

	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	AngleVectors(cl.viewangles, fwd, NULL, NULL);
	// combat lock: the target auto-shoot fired at may be followed a little
	// beyond the cone; any other enemy only inside it
	locked = cl_touch_aimassist_track.integer && ta.lock_ent && now < ta.lock_until;
	// a steered rocket is re-aimed in flight: it launches without the slew,
	// the fall model or the interpolation term, which measured best
	guided = wep && (wep->flags & TW_GUIDED) && cl_touch_autoshoot_steer.integer;
	scan(eye, fwd, cone, locked ? cone + LOCK_CONE_EXTRA : 0, wep, fire, fire ? (hitscan_lag > 0 ? hitscan_lag : guided ? latency_estimate() : shot_latency()) : 0,
		cl_touch_aimassist_track.integer && !guided, guided,
		cl_touch_autoshoot_group.integer || cl_touch_autoswitch_smart.integer || cl_touch_aimassist_priority.integer,
		&on_target, &friend_on, &near);
	if (near.entnum && near.err > cone && near.entnum != ta.lock_ent)
		near.entnum = 0;
	// the player turned the view away during a fight: the engagement goes on
	// with the enemy they now look nearest to, not the one they left
	if (cl_touch_aimassist_track.integer && ta.lock_ent && near.entnum && near.entnum != ta.lock_ent
		&& now - ta.lock_fired < ENGAGE_HOLD && now - ta.flick_time < FLICK_KEEP)
	{
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: combat lock moves to player %d (the player turned toward them)\n", near.entnum);
		ta.lock_ent = near.entnum;
		ta.lock_until = now + LOCK_HOLD;
	}
	if (near.entnum)
		gbodies_in_sight(eye, near.center, near.entnum);
	else
		num_gbodies = 0;
	// offline practice: a rocket at a target behind a wall goes over it: the
	// launch aims at the lowest point above the target that the eye sees and
	// that sees the target
	if (practice.seethrough && guided && near.entnum && near.hidden && ta.rk_state == RK_IDLE)
	{
		vec3_t over;
		int k;
		ta.rk_over_valid = false;
		for (k = 1; k <= 10; k++)
		{
			VectorCopy(near.center, over);
			over[2] += 48 * k;
			// the rocket's own size must pass, not only the eye's line
			if (launch_clear(eye, over, 0) && line_clear(over, near.center))
			{
				angles_to(eye, over, &near.yaw, &near.pitch);
				near.err = edge_err(wrap180(near.yaw - cl.viewangles[YAW]), near.pitch - cl.viewangles[PITCH], near.rh, near.rv);
				near.on_lead = near.err <= 0;
				VectorCopy(over, ta.rk_over);
				ta.rk_over_valid = true;
				break;
			}
		}
	}
	// a blast for the whole group, when the weapon has one worth sharing
	if (cl_touch_autoshoot_group.integer && near.entnum && fire && fire->splash >= 100)
		group_aim(&near, fire, eye, shot_latency());
	else
		ta.group_valid = false;
	// offline practice: a splash shot (not a steered rocket) at a target
	// behind cover aims where its simulated impact bursts next to it
	if (practice.seethrough && cl_touch_practice_burst.integer && !guided && near.entnum && near.hidden && hb_fire_ok(wep, fire))
	{
		if (hb.ent != near.entnum || now >= hb.until)
			hb_search(eye, &near, fire, shot_latency());
		if (hb.valid)
		{
			near.yaw = hb.yaw;
			near.pitch = hb.pitch;
			near.rh = near.rv = 0.5f;
			near.err = edge_err(wrap180(near.yaw - cl.viewangles[YAW]), near.pitch - cl.viewangles[PITCH], near.rh, near.rv);
			near.on_lead = near.err <= 0;
			ta.group_valid = false;
		}
	}
	else
		hb.valid = false;

	if (cl_touch_aim_debug.integer && on_target != ta.debug_target)
	{
		if (on_target)
			Con_Printf("touch aim: crosshair on player %d (%s)%s\n", on_target, player_name(on_target), cl_touch_aim_debug.integer >= 2 ? va(vabuf, sizeof(vabuf), " at %.3f", cl.time) : "");
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
				// (a guided rocket is re-aimed in flight: its lead point may
				// lie beyond a wall the target will turn at)
				if (!near.has_lead || guided || (f && f->kind == TW_KIND_BALLISTIC) || (near.hidden && hb.valid) || (cl_touch_autoshoot_feet.integer ? shot_path_ok(eye, lp, f) : line_clear(eye, lp)))
					on_shoot = near.entnum;
				// a rocket that would hit the lip in front of the muzzle
				// (and likely its shooter) is held
				if (on_shoot && guided && f && cl_touch_autoshoot_clear.integer && ta.rk_state == RK_IDLE && !launch_clear_view(eye, near.dist, f->splash))
				{
					static double said;
					ta.rk_lift_ent = near.entnum;
					ta.rk_lift_until = now + 3.0;
					if (cl_touch_aim_debug.integer && now - said > 1)
					{
						Con_Printf("touch aim: rocket held: its launch path hits something in front of the muzzle\n");
						said = now;
					}
					on_shoot = 0;
				}
			}
		}
	}

	// a target behind a wall (offline practice): only a splash weapon whose
	// blast reaches it round the cover fires; anything else only pre-aims
	if (on_shoot && near.entnum == on_shoot && near.hidden)
	{
		const touch_wep_fire_t *f = wep ? (ta.last_mode == TW_MODE_SECONDARY && wep->sec.kind != TW_KIND_NONE ? &wep->sec : &wep->pri) : NULL;
		vec3_t lp;
		VectorAdd(near.center, near.lead, lp);
		// a steered rocket follows the crosshair: with no way over the
		// cover it only flies into the wall in front; anything else fires only when the shot from the view as it is now
		// is simulated to burst within reach of where the target will be
		if (!f || f->splash < 60 || !(guided ? ta.rk_over_valid || launch_clear(eye, near.center, f->splash)
			: !cl_touch_practice_burst.integer ? shot_path_ok(eye, lp, f)
			: hb.valid && hb.ent == near.entnum && hb_fire_ok(wep, f) && hb_view_ok(eye, &near, f, shot_latency())))
			on_shoot = 0;
	}
	// a steered rocket at a target in sight: the straight line there must
	// carry it, or end where its blast still reaches (only the head showing
	// over a low wall puts the wall in its way)
	else if (on_shoot && near.entnum == on_shoot && guided && ta.rk_state == RK_IDLE && wep && cl_touch_autoshoot_clear.integer
		&& !launch_clear(eye, near.center, wep->pri.splash))
	{
		vec3_t hd;
		VectorCopy(near.center, hd);
		hd[2] = ta.tracks[near.entnum & (TRACK_MAX - 1)].bmax[2] - 6;
		if (!launch_clear(eye, hd, 0))
		{
			static double said;
			if (cl_touch_aim_debug.integer && now - said > 1)
			{
				Con_Printf("touch aim: rocket held: no clear launch line to player %d\n", near.entnum);
				said = now;
			}
			on_shoot = 0;
		}
	}
	shootable = on_shoot;

	// a projectile that cannot meet a moving enemy (too far, or it outruns
	// the shot) would only land where the enemy was: hold it. A steered
	// rocket is re-aimed in flight and may still go.
	if (on_shoot && near.entnum == on_shoot && near.lead_failed && !guided && cl_touch_autoshoot_predict.integer)
		on_shoot = 0;

	// aimed at the head: wait (up to HEAD_WAIT) until the pull has the
	// crosshair at head height, not the first moment it touches the body
	if (on_shoot && near.entnum == on_shoot && near.head_aim)
	{
		float zone = atan(0.5f * TW_HEAD_ZONE_H / max(near.dist, 1.0f)) * (180.0f / M_PI);
		if (!ta.head_wait)
			ta.head_wait = now;
		if (fabs(near.pitch - cl.viewangles[PITCH]) > zone && now - ta.head_wait < HEAD_WAIT)
			on_shoot = 0;
	}
	else
		ta.head_wait = 0;

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
		// Electro combo: orbs first (the attack2 hold lobs up to three), the
		// bolt that sets them off follows by the ordinary rule
		if (cl_touch_autoshoot_combo.integer && s.use_alt && s.alt_ready && near.entnum == on_shoot && !strcmp(wep->netname, "electro"))
		{
			const track_t *ot = &ta.tracks[on_shoot & (TRACK_MAX - 1)];
			if (now < ta.orbs_until)
			{
				mode = TW_MODE_SECONDARY;
				why = TW_WHY_ORBS;
			}
			else if (now - ta.orbs_time > TW_ORB_CYCLE && TouchWep_ElectroOrbs(ot->cls, ot->range_rate, dist, TouchHUD_WeaponAmmo(held), group_near(&near)))
			{
				if (ta.last_mode == TW_MODE_SECONDARY)
				{
					// the lob was aimed last frame: throw
					mode = TW_MODE_SECONDARY;
					why = TW_WHY_ORBS;
					ta.orbs_time = now;
					ta.orbs_until = now + ORB_HOLD;
					if (cl_touch_aim_debug.integer)
						Con_Printf("touch aim: electro combo: orbs at player %d (%s, %.0f units)\n", on_shoot, touch_wep_class_name[ot->cls], dist);
				}
				else
				{
					// aim the lob first (the aim follows the last mode)
					ta.last_mode = TW_MODE_SECONDARY;
					mode = TW_MODE_NONE;
					why = TW_WHY_ORBS;
				}
			}
		}
		if (mode == TW_MODE_NONE && !cl_touch_autoshoot_safe.integer && why != TW_WHY_NO_AMMO)
			mode = TW_MODE_PRIMARY; // the player turned the safety rules off
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

	// a teammate on the line of the shot, or where its blast lands: hold
	// (a guided rocket's teammates are judged again at the detonation)
	mate_block = 0;
	if (on_shoot && num_mates && cl_touch_autoshoot_teamsafe.integer)
	{
		const touch_wep_fire_t *f = wep ? (mode == TW_MODE_SECONDARY && wep->sec.kind != TW_KIND_NONE ? &wep->sec : &wep->pri) : NULL;
		touch_wep_fire_t unknown;
		vec3_t ip;
		int how = 0;
		if (!f)
		{
			// a weapon the table does not know: as a hitscan, the line only
			memset(&unknown, 0, sizeof(unknown));
			unknown.kind = TW_KIND_HITSCAN;
			f = &unknown;
		}
		if (near.entnum == on_shoot && f->kind == TW_KIND_BALLISTIC)
			VectorMA(near.center, near.tof, near.vel, ip);
		else if (near.entnum == on_shoot)
			VectorAdd(near.center, near.lead, ip);
		else
			VectorCopy(ta.tracks[on_shoot & (TRACK_MAX - 1)].org, ip);
		mate_block = mate_blocks(eye, fwd, VectorDistance(eye, ip), wep, f, ip, &how);
		if (mate_block)
		{
			if (cl_touch_aim_debug.integer && ta.mate_said != mate_block)
				Con_Printf("touch aim: holding fire: teammate %d (%s) %s\n", mate_block, player_name(mate_block), how == TW_MATE_LINE ? "on the line of the shot" : "inside the blast");
			on_shoot = 0;
		}
	}
	ta.mate_said = mate_block;

	if (cl_touch_aim_debug.integer && (near.entnum != ta.said_target || held != ta.said_weapon || (on_shoot ? mode : -1) != ta.said_mode || why != ta.said_why || near.hidden != ta.said_hidden))
	{
		if (near.entnum)
			Con_Printf("touch aim: target %d (%s)%s at %.0f units, %.1f deg off; weapon %s: %s (%s)%s\n", near.entnum, player_name(near.entnum), near.hidden ? " behind a wall" : "", near.dist, near.err,
				held >= 0 ? TouchHUD_WeaponName(held) : "?", !on_shoot ? "hold" : mode == TW_MODE_SECONDARY ? "secondary" : "primary",
				wep ? touch_wep_why[why] : "unknown weapon",
				near.has_lead ? va(vabuf, sizeof(vabuf), ", lead %.2f s (%.0f units)", near.tof, VectorLength(near.lead)) : "");
		ta.said_target = near.entnum;
		ta.said_weapon = held;
		ta.said_mode = on_shoot ? mode : -1;
		ta.said_why = why;
		ta.said_hidden = near.hidden;
	}

	ta.lock = !assist && !autoshoot ? 0 : ((on_target || (near.entnum && near.on_lead)) ? 2 : (near.entnum ? 1 : 0));

	// switch first: no rocket is launched while a switch is on its way
	if (!dry_check(&near, held, wep, now))
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
		steer_devastator(eye, &near, want, manual, weapon_ready(held, wep, held), mate_block, &pull_yaw, &pull_pitch, &pull_target_set);
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
		ta.num_free = 0; // no longer the Devastator: rockets out fly on unsteered
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
		// the player's own press decides alone; a teammate in the way stops
		// the shot at once (no release tail)
		if (manual || mate_block)
			on = on2 = false;
		set_firing(on, on2, target);
		ta.fire_target = on || on2 ? target : 0;
	}

	// fire latency (cl_touch_aim_debug 2): frames from the target becoming
	// shootable (in the fire window) to auto-shoot pressing
	if (shootable)
	{
		if (!ta.shootable_since)
		{
			ta.shootable_since = now;
			ta.shootable_frames = 0;
		}
		else
			ta.shootable_frames++;
	}
	if ((ta.firing || ta.firing2) && !was_firing && cl_touch_aim_debug.integer >= 2)
		Con_Printf("touch aim: fire latency: %d frames (%.0f ms) after the target came into the fire window\n",
			ta.shootable_since ? ta.shootable_frames : -1, ta.shootable_since ? (now - ta.shootable_since) * 1000 : -1.0);
	if (!shootable)
		ta.shootable_since = 0;

	// a splash launcher fired at the target (for the knock-up combo)
	if (ta.firing && ta.fire_target && wep && wep->pri.splash >= 100)
		ta.launcher_fired = now;

	// combat lock: renewed while auto-shoot fires, kept LOCK_HOLD after, and
	// for as long as that enemy stays the target in sight and was fired at
	// within ENGAGE_HOLD (a Devastator between rockets, a reload): the fight
	// auto-shoot started goes on with the thumb at rest
	if (cl_touch_aimassist_track.integer && ta.fire_target && (ta.firing || ta.firing2))
	{
		if (cl_touch_aim_debug.integer && (ta.lock_ent != ta.fire_target || now >= ta.lock_until))
			Con_Printf("touch aim: combat lock on player %d (%s)\n", ta.fire_target, player_name(ta.fire_target));
		ta.lock_ent = ta.fire_target;
		ta.lock_until = now + LOCK_HOLD;
		ta.lock_fired = now;
	}
	else if (cl_touch_aimassist_track.integer && ta.lock_ent && now < ta.lock_until && near.entnum == ta.lock_ent && now - ta.lock_fired < ENGAGE_HOLD)
		ta.lock_until = now + LOCK_HOLD;
	else if (ta.lock_ent && now < ta.lock_until && (TouchHook_InUse() || TouchHook_Rescuing()))
		ta.lock_until = now + LOCK_HOLD; // held through the hook or a turn: the fight resumes after it
	else if (ta.lock_ent && now >= ta.lock_until)
	{
		if (cl_touch_aim_debug.integer)
			Con_Printf("touch aim: combat lock on player %d ended\n", ta.lock_ent);
		ta.lock_ent = 0;
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
		// nor while a deliberate drag is leaving it: that is the player's choice
		qbool leaving = turn_mag / dt > LEAVE_RATE && turn_yaw * ey + turn_pitch * ep < 0;
		if (!approaching && !leaving)
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
	// last seen: the enemy of the fight went out of sight and nobody else is
	// in view: the pull holds the crosshair where it was expected to be,
	// from what was seen (the ghost is never fed a hidden position)
	if (near.entnum)
		TouchWep_GhostUpdate(&ta.tracks[near.entnum & (TRACK_MAX - 1)].ghost, true, near.center, near.vel, now);
	if (!pull_target_set && !near.entnum && cl_touch_aimassist_lastseen.integer && cl_touch_aimassist_track.integer
		&& ta.lock_ent && now - ta.lock_fired < ENGAGE_HOLD + TW_GHOST_KEEP)
	{
		track_t *gt = &ta.tracks[ta.lock_ent & (TRACK_MAX - 1)];
		vec3_t gp, mv;
		float conf;
		qbool held_point = TouchWep_GhostPoint(&gt->ghost, now, gp, &conf);
		if (held_point)
		{
			// not on through a wall it would have run into
			VectorSubtract(gp, gt->ghost.org, mv);
			clip_lead(gt->ghost.org, mv);
			VectorAdd(gt->ghost.org, mv, gp);
			// and only a point the eye sees: never held on a wall the enemy
			// went behind (offline see-through aims through it on purpose)
			held_point = practice.seethrough || line_clear(eye, gp);
		}
		if (held_point)
		{
			angles_to(eye, gp, &pull_yaw, &pull_pitch);
			pull_target_set = true;
			ta.lock_until = now + LOCK_HOLD;
			if (cl_touch_aim_debug.integer && ta.ghost_said != ta.lock_ent)
				Con_Printf("touch aim: player %d out of sight: holding on where it was last seen%s\n", ta.lock_ent, cl_touch_aim_debug.integer >= 2 ? va(vabuf, sizeof(vabuf), " at %.3f", cl.time) : "");
			ta.ghost_said = ta.lock_ent;
		}
		else
		{
			// out of sight too long, or behind cover: the fight with it ends
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch aim: player %d forgotten (%s)\n", ta.lock_ent, ta.ghost_said ? "out of sight too long" : "behind cover");
			ta.ghost_said = 0;
			ta.lock_ent = 0;
		}
	}
	else if (near.entnum)
		ta.ghost_said = 0;
	if (pull_target_set && !near.entnum && ta.rk_state == RK_IDLE)
	{
		near.entnum = ta.lock_ent;
		near.err = 1;
		near.rh = near.rv = 0.5f;
	}
	else if (pull_target_set && near.entnum != ta.rk_target)
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
	if (cl_touch_aim_debug.integer >= 5 && near.entnum)
		Con_Printf("touch aim: pull? target %d err %.1f yaw %+.1f pitch %+.1f lock %d until %+.2f fired %.2f ago touch %d input %.2f ago rocket %s\n", near.entnum, near.err,
			wrap180(near.yaw - cl.viewangles[YAW]), near.pitch - cl.viewangles[PITCH], ta.lock_ent, ta.lock_until - now, now - ta.lock_fired, aim_touch, now - ta.last_input, rk_state_name[ta.rk_state]);
	if (strength > 0 && near.entnum && !TouchHook_Rescuing()
		&& ((aim_touch && now - ta.last_input < INPUT_HOLD)
			|| practice.seethrough || practice.full360 // offline practice: the pull acts on its own
			|| ta.rk_state != RK_IDLE
			|| (cl_touch_aimassist_track.integer && ta.lock_ent && ta.lock_ent == near.entnum && now < ta.lock_until)))
	{
		float ey = wrap180(near.yaw - cl.viewangles[YAW]);
		float ep = near.pitch - cl.viewangles[PITCH];
		float turn_rate = turn_mag / dt;
		// heading away: clearly against the target (not a thumb drifting
		// sideways, not a slow drift); the pull resumes the first frame the
		// drag slows down or turns back
		qbool away = turn_rate > AWAY_NOISE_RATE && (turn_yaw * ey + turn_pitch * ep) < -0.5f * turn_mag * sqrt(ey * ey + ep * ep);
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
					// a guided rocket's view hold closes faster: the rocket
					// turns toward the view at 90 deg/s and lags it anyway
					float rate = strength * max(turn_rate, (ta.rk_state == RK_FLY ? SETTLE_GAIN_ROCKET : SETTLE_GAIN) * gap);
					float step = min(rate * dt, gap);
					py += gy / gap * step;
					pp += gp / gap * step;
				}
			}
			len = sqrt(py * py + pp * pp);
			cap = bound(0.0f, cl_touch_aimassist_maxrate.value, MAXRATE_MAX) * dt;
			if (practice.instant)
			{
				// offline practice: the whole way at once
				py = ey;
				pp = ep;
				len = sqrt(py * py + pp * pp);
				cap = len + 1;
			}
			if (len > cap)
			{
				py *= cap / len;
				pp *= cap / len;
			}
			cl.viewangles[YAW] += py;
			cl.viewangles[PITCH] = bound(-89.0f, cl.viewangles[PITCH] + pp, 89.0f);
		}
	}
	if (cl_touch_aim_debug.integer >= 3)
	{
		if (on_shoot && near.entnum == on_shoot && near.hidden)
			shot_hidden_fired = now;
		shot_hidden = now - shot_hidden_fired < 0.15;
		debug_shots(eye, ta.fire_target ? ta.fire_target : ta.lock_ent ? ta.lock_ent : near.entnum);
	}
	ta.track_ent = near.entnum;
	ta.track_yaw = near.yaw;
	ta.track_pitch = near.pitch;

	VectorCopy(cl.viewangles, ta.prev_angles);
	ta.prev_valid = true;
	publish();
}

// The hook first: a fall rescue turns the view, and the aim pull stands down
// while it does (TouchHook_Rescuing). Then the angles the next frame's turn is
// measured from.
// Offline practice markers: enemies behind walls (an outlined square and the
// distance) and, in Full 360, enemies off the screen (a square at the edge,
// toward them). Drawn by the engine so both touch HUDs get them; nothing is
// collected unless the gate is open.
void TouchAim_Draw(void)
{
	vec3_t org, fwd, left, up, d;
	float w = vid_conwidth.value, h = vid_conheight.value, fx = r_refdef.view.frustum_x, fy = r_refdef.view.frustum_y;
	int i;
	TouchTactics_Draw(); // offline only: health, armour, weapon and aim of every enemy
	// Mobile (touch) builds draw the practice markers in every match; elsewhere
	// they stay offline-only.
	if (!num_marks || TouchTactics_EspGate(practice_gate()) != TW_GATE_OPEN || fx <= 0 || fy <= 0)
		return;
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, org);
	for (i = 0; i < num_marks; i++)
	{
		const mark_t *mk = &marks[i];
		float df, x, y, s = bound(8.0f, 9000.0f / max(mk->dist, 1.0f), 40.0f);
		qbool off;
		char txt[16];
		VectorSubtract(mk->org, org, d);
		df = DotProduct(d, fwd);
		x = 0.5f * w * (1 - DotProduct(d, left) / (max(df, 1.0f) * fx));
		y = 0.5f * h * (1 - DotProduct(d, up) / (max(df, 1.0f) * fy));
		off = df <= 1 || x < 0 || x > w || y < 0 || y > h;
		if (off)
		{
			// at the edge, toward it
			float ax = -DotProduct(d, left), ay = -DotProduct(d, up), m = max(fabs(ax) / (0.5f * w - 12), fabs(ay) / (0.5f * h - 12));
			if (m <= 0)
				continue;
			x = 0.5f * w + ax / m;
			y = 0.5f * h + ay / m;
			s = 12;
			DrawQ_Fill(x - s * 0.5f, y - s * 0.5f, s, s, 1, 0.3f, 0.1f, 0.85f, 0);
			continue;
		}
		if (!mk->hidden)
			continue;
		DrawQ_Fill(x - s * 0.5f, y - s * 0.5f, s, 2, 1, 0.5f, 0.1f, 0.9f, 0);
		DrawQ_Fill(x - s * 0.5f, y + s * 0.5f - 2, s, 2, 1, 0.5f, 0.1f, 0.9f, 0);
		DrawQ_Fill(x - s * 0.5f, y - s * 0.5f, 2, s, 1, 0.5f, 0.1f, 0.9f, 0);
		DrawQ_Fill(x + s * 0.5f - 2, y - s * 0.5f, 2, s, 1, 0.5f, 0.1f, 0.9f, 0);
		dpsnprintf(txt, sizeof(txt), "%.0f", mk->dist);
		DrawQ_String(x - 12, y + s * 0.5f + 2, txt, 0, 10, 10, 1, 0.6f, 0.2f, 0.9f, 0, NULL, true, FONT_CHAT);
	}
}

// cl_touch_aim_dumptracks: the players the CSQC draws, one line each a
// frame, for scripts/eval-lead (testing only; works in demo playback).
static void dump_tracks(void)
{
	prvm_prog_t *prog = CLVM_prog;
	int i;
	if (!prog->loaded || cls.state != ca_connected)
		return;
	for (i = 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		int n;
		float *o, *v;
		if (ed->free)
			continue;
		n = (int)PRVM_clientedictfloat(ed, entnum);
		if (n < 1 || n > cl.maxclients || PRVM_clientedictfloat(ed, solid) == SOLID_CORPSE || !PRVM_clientedictfloat(ed, modelindex))
			continue;
		o = PRVM_clientedictvector(ed, origin);
		v = PRVM_clientedictvector(ed, velocity);
		Con_Printf("XTRK %.4f %d %.1f %.1f %.1f %.1f %.1f %.1f\n", cl.time, n, o[0], o[1], o[2], v[0], v[1], v[2]);
	}
}

void TouchAim_Frame(void)
{
	double t0;
	if (cl_touch_aim_dumptracks.integer)
		dump_tracks();
	TouchAssist_Frame();
	TouchHook_Frame();
	t0 = Sys_DirtyTime();
	aim_frame();
	if (cl_touch_aim_debug.integer >= 4)
	{
		// frame cost of the aim helpers, every 5 seconds
		double dt = Sys_DirtyTime() - t0, now = host.realtime;
		ta.cost_time += dt;
		ta.cost_frames++;
		if (now - ta.cost_since >= 5)
		{
			if (ta.cost_since > 0 && ta.cost_frames)
				Con_Printf("touch aim: frame cost %.1f us on average over %d frames, %d players in range\n", ta.cost_time / ta.cost_frames * 1e6, ta.cost_frames, ta.cost_players);
			ta.cost_since = now;
			ta.cost_time = 0;
			ta.cost_frames = 0;
		}
	}
	TouchHook_EndFrame();
}
