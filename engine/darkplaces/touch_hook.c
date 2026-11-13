/*
Godnotic: the HOOK button's hold and the fall rescue (see touch_hook.h).

Where the player is: the view origin less the view height, the server's
velocity and on-ground flag (svc_clientdata, the same under any CSQC), and the
last spot they stood on. What the fall meets: the world and the CSQC's brush
entities (CL_TraceBox), and the map's trigger_hurt volumes, which the client
never sees as entities but finds in the map's own entity list. The decisions
are touch_rescue.h's.
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "csprogs.h"
#include "touch_hud.h"
#include "touch_aim.h"
#include "touch_hook.h"
#include "touch_rescue.h"
#include "touch_turn.h"
#include "touch_pause.h"
#include "touch_nade.h"
#include "touch_frontend.h"
#include "touch_tactics.h"

extern cvar_t vid_touchscreen;
extern qbool cl_touch_csqc_active;
extern cvar_t cl_touch_aim_debug;
extern kbutton_t in_button6;

cvar_t cl_touch_hook_rescue = {CF_CLIENT | CF_ARCHIVE, "cl_touch_hook_rescue", "1",
	"Touch controls: when you fall with nothing below to land on, fire the grappling hook at something it can catch, pull you back and let go over ground. "
	"Only with a hook to hand (the server's offhand hook, or the Hook weapon with fuel), only in touch mode"};
cvar_t cl_touch_hook_rescue_turn = {CF_CLIENT | CF_ARCHIVE, "cl_touch_hook_rescue_turn", "720",
	"Fall rescue: fastest the view turns to the hook's target and back, degrees per second (180 - 1440)"};
cvar_t cl_touch_hook_rescue_range = {CF_CLIENT | CF_ARCHIVE, "cl_touch_hook_rescue_range", "1600",
	"Fall rescue: farthest point the hook is sent to, units (400 - 3000)"};
cvar_t cl_touch_hook_rescue_cooldown = {CF_CLIENT | CF_ARCHIVE, "cl_touch_hook_rescue_cooldown", "1",
	"Fall rescue: seconds after one rescue before the next may start (0.3 - 5)"};
// Engine -> CSQC: what the port's HOOK button shows.
cvar_t cl_touch_hook_avail = {CF_CLIENT, "_cl_touch_hook_avail", "0",
	"Set by the engine: the hook the touch controls can use now, 0 none, 1 the offhand hook, 2 the Hook weapon"};
cvar_t cl_touch_hook_state = {CF_CLIENT, "_cl_touch_hook_state", "0",
	"Set by the engine: 1 while the hook is held by the HOOK button, 2 while the fall rescue holds or aims it"};

// Ceilings whatever the cvars say.
#define TURN_MIN        180.0f
#define TURN_MAX        1440.0f
#define RANGE_MIN       400.0f
#define RANGE_MAX       3000.0f
#define COOLDOWN_MIN    0.3f
#define COOLDOWN_MAX    5.0f
// The view turn slows into its target: deg/s per degree still to go, and the
// slowest it gets.
#define TURN_GAIN       14.0f
#define TURN_FLOOR      90.0f
// The hook flies at 1800 u/s (g_balance_grapplehook_speed_fly); the catch is
// given that long, plus this for the round trip to the server.
#define HOOK_FLY_SPEED  1800.0f
#define ATTACH_SLACK    0.35f
// The Hook weapon: fired once the switch has landed; the switch is given up
// after this long. Auto-switch keeps away for HOOK_GUARD after a release.
#define SWITCH_GIVEUP   1.5f
#define HOOK_GUARD      1.5f
// A position jump this much larger than the velocity explains, in one frame,
// is a teleport or a respawn.
#define TELEPORT_DIST   300.0f
// trigger_hurt volumes kept per map; ones that hurt less than this per hit are
// not a death zone (vorix's cosmetic ones have dmg 0).
#define MAX_HURT        64
#define HURT_MIN_DMG    5

// active_mutators bits (common/util.qh): the offhand hook in every version;
// the mutators that take +hook for themselves differ by version. 0.8.6
// numbers nades 23 and offhand blaster 24, today's code 24 and 25 (it added
// "no start weapons" at 23). A 0.8.6 CSQC is told by its MUTATOR_ARGV_0_int
// global, which today's code replaced with arrays.
#define MUT_HOOK_BIT    8

typedef struct hurt_s { vec3_t mins, maxs; } hurt_t;

static struct
{
	// what there is
	int avail;
	int hook_index;            // TouchHUD index of the Hook weapon, -1 none
	int mut_ofs0, mut_ofs1;    // active_mutators[0], [1] global offsets, -1 none
	qbool mut_int;             // the globals are ints (else floats)
	qbool mut_old_layout;      // 0.8.6 numbering
	// the hold
	qbool hud_down, cmd_down, rescue_hold;
	qbool held;                // held last frame
	int buttons;
	int prev_weapon;           // weapon to take back after the Hook weapon, -1 none
	qbool switched;            // we asked for the Hook weapon
	double switch_time, release_time;
	// the rescue
	tr_state_t rs;
	tr_anchor_t anchor;
	qbool have_anchor;
	vec3_t fire_vel;           // velocity when the hook left
	double fire_time;
	double out_time;           // the rescue's hook left (0 = not yet)
	vec3_t saved_angles;       // the view when the rescue began
	vec3_t prev_angles;        // the view at the end of the last frame
	qbool prev_valid;
	vec3_t home;               // where the player last stood
	qbool home_valid;
	vec3_t last_org;
	qbool last_org_valid;
	// how fast the player really moves: a player wedged under a ledge keeps
	// the rope's velocity without going anywhere (one sliding along under a
	// platform is moving, and often comes up over its edge)
	vec3_t move_org;
	double move_time;
	float move_speed;
	int fall;                  // the last prediction, -1 none
	float ground_below;
	int traces;                // this frame's traces
	double cost_sum;           // seconds spent judging falls (debug 2 report)
	int cost_frames;
	double cost_report;
	// the map's death volumes
	model_t *hurt_model;
	hurt_t hurt[MAX_HURT];
	int num_hurt;
	// debug
	const char *blocked_said;
	int avail_said;
} th;

// ---------------------------------------------------------------------------
// The world for touch_rescue.h
// ---------------------------------------------------------------------------
#define FALL_MASK   (SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP | SUPERCONTENTS_LIQUIDSMASK | SUPERCONTENTS_SKY)
#define HOOK_MASK   (SUPERCONTENTS_SOLID | SUPERCONTENTS_SKY)

static void th_trace(void *ctx, const float start[3], const float end[3], int hull, tr_trace_t *out)
{
	vec3_t s, e;
	trace_t tr;
	(void)ctx;
	VectorCopy(start, s);
	VectorCopy(end, e);
	if (hull)
		tr = CL_TraceBox(s, cl.playerstandmins, cl.playerstandmaxs, e, MOVE_NOMONSTERS, NULL, FALL_MASK, 0, 0,
			collision_extendmovelength.value, true, false, NULL, true);
	else
		tr = CL_TraceLine(s, e, MOVE_NOMONSTERS, NULL, HOOK_MASK, 0, 0,
			collision_extendmovelength.value, true, false, NULL, true, false);
	th.traces++;
	out->fraction = (float)tr.fraction;
	VectorCopy(tr.endpos, out->endpos);
	VectorCopy(tr.plane.normal, out->normal);
	out->startsolid = tr.startsolid;
	if (tr.fraction >= 1)
		out->hit = TR_HIT_NONE;
	else if ((tr.hitsupercontents & (SUPERCONTENTS_SKY | SUPERCONTENTS_LAVA | SUPERCONTENTS_SLIME)) || (tr.hitq3surfaceflags & Q3SURFACEFLAG_SKY))
		out->hit = TR_HIT_DEATH;
	else if (tr.hitsupercontents & SUPERCONTENTS_WATER)
		out->hit = TR_HIT_WATER;
	else
		out->hit = TR_HIT_SOLID;
}

// The player's box moving start -> end against the trigger_hurt volumes:
// the first entry, as a fraction of the way, or 2 for none.
static float th_death(void *ctx, const float start[3], const float end[3])
{
	float best = 2;
	int i, k;
	(void)ctx;
	for (i = 0; i < th.num_hurt; i++)
	{
		float tmin = 0, tmax = 1;
		qbool miss = false;
		for (k = 0; k < 3 && !miss; k++)
		{
			float lo = th.hurt[i].mins[k] - cl.playerstandmaxs[k];
			float hi = th.hurt[i].maxs[k] - cl.playerstandmins[k];
			float d = end[k] - start[k];
			if (fabs(d) < 1e-6f)
			{
				if (start[k] < lo || start[k] > hi)
					miss = true;
				continue;
			}
			{
				float t0 = (lo - start[k]) / d, t1 = (hi - start[k]) / d;
				if (t0 > t1)
				{
					float t = t0;
					t0 = t1;
					t1 = t;
				}
				tmin = max(tmin, t0);
				tmax = min(tmax, t1);
				if (tmin > tmax)
					miss = true;
			}
		}
		if (!miss && tmin < best)
			best = tmin;
	}
	return best;
}

// The map's trigger_hurt boxes, from its entity list: each names its brush
// model ("*N"), whose bounds are the volume.
static void th_bind_map(void)
{
	const char *data;
	char key[64], classname[64], model[16], dmg[32];
	model_t *wm = cl.worldmodel;
	th.hurt_model = wm;
	th.num_hurt = 0;
	if (!wm || !wm->brush.entities)
		return;
	data = wm->brush.entities;
	while (COM_ParseToken_Simple(&data, false, false, true) && com_token[0] == '{')
	{
		classname[0] = model[0] = dmg[0] = 0;
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
			else if (!strcmp(key, "dmg"))
				dp_strlcpy(dmg, com_token, sizeof(dmg));
		}
		if (strcmp(classname, "trigger_hurt") || model[0] != '*' || th.num_hurt >= MAX_HURT)
			continue;
		if (dmg[0] && atof(dmg) < HURT_MIN_DMG)
			continue;
		{
			int n = atoi(model + 1);
			model_t *sub;
			if (n <= 0 || n >= wm->brush.numsubmodels || !(sub = wm->brush.submodels[n]))
				continue;
			VectorCopy(sub->normalmins, th.hurt[th.num_hurt].mins);
			VectorCopy(sub->normalmaxs, th.hurt[th.num_hurt].maxs);
			th.num_hurt++;
		}
	}
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch hook: %d trigger_hurt volumes on this map; lowest point z %.0f\n", th.num_hurt, wm->normalmins[2]);
}

static tr_world_t th_world(void)
{
	tr_world_t w;
	memset(&w, 0, sizeof(w));
	w.trace = th_trace;
	w.death = th_death;
	w.floor_z = cl.worldmodel ? cl.worldmodel->normalmins[2] : -65536;
	w.gravity = cl.movevars_gravity * (cl.movevars_entgravity ? cl.movevars_entgravity : 1);
	return w;
}

// The world as touch_rescue.h sees it, for touch_dodge.c.
void TouchHook_World(struct tr_world_s *w)
{
	*w = th_world();
}

// ---------------------------------------------------------------------------
// What hook there is
// ---------------------------------------------------------------------------
static void bind_mutators(void)
{
	prvm_prog_t *prog = CLVM_prog;
	mdef_t *d;
	th.mut_ofs0 = th.mut_ofs1 = -1;
	if (!prog->loaded)
		return;
	d = PRVM_ED_FindGlobal(prog, "active_mutators");
	if (d)
	{
		// an int array in every build so far; read a float one all the same
		th.mut_int = (d->type & ~DEF_SAVEGLOBAL) != ev_float;
		th.mut_ofs0 = d->ofs;
		d = PRVM_ED_FindGlobal(prog, "active_mutators[1]");
		if (d)
			th.mut_ofs1 = d->ofs;
	}
	th.mut_old_layout = PRVM_ED_FindGlobal(prog, "MUTATOR_ARGV_0_int") != NULL;
}

static int mut_word(int ofs)
{
	prvm_prog_t *prog = CLVM_prog;
	if (ofs < 0 || !prog->loaded)
		return 0;
	return th.mut_int ? PRVM_GLOBALFIELDVALUE(ofs)->_int : (int)PRVM_GLOBALFIELDVALUE(ofs)->_float;
}

// A bit of the loaded CSQC's active_mutators (bit 0: dodging, in every version).
qbool TouchHook_Mutator(int bit)
{
	return bit < 24 ? (mut_word(th.mut_ofs0) & (1 << bit)) != 0 : (mut_word(th.mut_ofs1) & (1 << (bit - 24))) != 0;
}

// The server gives +hook to the offhand hook (and nothing else has it).
static qbool offhand_hook(void)
{
	int w0 = mut_word(th.mut_ofs0), w1 = mut_word(th.mut_ofs1);
	qbool taken;
	if (!(w0 & (1 << MUT_HOOK_BIT)))
		return false;
	if (th.mut_old_layout)
		taken = (w0 & (1 << 23)) || (w1 & 1);       // nades, offhand blaster
	else
		taken = (w1 & 1) || (w1 & 2);               // nades, offhand blaster
	return !taken;
}

static void bind_weapon(void)
{
	int i;
	th.hook_index = -1;
	for (i = 0; i < TouchHUD_NumWeapons(); i++)
		if (!strcmp(TouchHUD_WeaponName(i), "hook"))
			th.hook_index = i;
}

void TouchHook_ProgsChanged(void)
{
	bind_mutators();
	bind_weapon();
	TouchDodge_ProgsChanged();
	TouchNade_ProgsChanged();
	th.switched = false;
	th.prev_weapon = -1;
	TouchRescue_Reset(&th.rs);
}

static int hook_weapon_ready(void)
{
	int ammo;
	if (th.hook_index < 0 || !TouchHUD_WeaponOwned(th.hook_index))
		return false;
	ammo = TouchHUD_WeaponAmmo(th.hook_index);
	return ammo != 0; // -1: no fuel needed
}

static int which_hook(void)
{
	if (offhand_hook())
		return TOUCH_HOOK_OFFHAND;
	if (hook_weapon_ready())
		return TOUCH_HOOK_WEAPON;
	return TOUCH_HOOK_NONE;
}

// The conditions of the aim helpers (touch mode, no mouse aiming, alive, in
// play, not spectating, no menu), or NULL when the hook may be used.
static const char *block_reason(void)
{
	return TouchAim_BlockReason();
}

int TouchHook_Available(void)
{
	return th.avail;
}

qbool TouchHook_ButtonShown(void)
{
	cvar_t *mode = Cvar_FindVar(&cvars_all, "touch_hook_visible", ~0);
	int m = mode ? mode->integer : 2;
	if (m <= 0 || !vid_touchscreen.integer || vid_touchscreen_mouselook.integer)
		return false;
	if (m == 1)
		return true;
	return th.avail != TOUCH_HOOK_NONE;
}

qbool TouchHook_Held(void)
{
	return th.held;
}

qbool TouchHook_InUse(void)
{
	return th.held || th.switched || host.realtime - th.release_time < HOOK_GUARD;
}

qbool TouchHook_Rescuing(void)
{
	return th.rs.phase == TR_AIM || th.rs.phase == TR_HOLD || th.rs.phase == TR_RESTORE
		|| TouchTurn_Spinning(); // the quick turn owns the view too
}

void TouchHook_SetHudButton(qbool down)
{
	th.hud_down = down;
}

static void TouchHook_Down_f(cmd_state_t *cmd)
{
	th.cmd_down = true;
}

static void TouchHook_Up_f(cmd_state_t *cmd)
{
	th.cmd_down = false;
}

int TouchHook_Buttons(void)
{
	return th.buttons | TouchDodge_Buttons() | TouchNade_Buttons() | TouchTactics_Buttons();
}

// The server runs the nades mutator (offhand grenades on +hook).
qbool TouchHook_NadesActive(void)
{
	return th.mut_old_layout ? (mut_word(th.mut_ofs0) & (1 << 23)) != 0 : (mut_word(th.mut_ofs1) & 1) != 0;
}

// ---------------------------------------------------------------------------
// The hold: button6 for the offhand hook; for the Hook weapon a switch to it,
// attack once it is in hand, and the previous weapon back on release.
// ---------------------------------------------------------------------------
static void hold_frame(qbool want, qbool prepare, double now)
{
	int held_wep = TouchHUD_HeldWeapon();
	th.buttons = 0;
	if ((want || prepare) && th.avail == TOUCH_HOOK_WEAPON)
	{
		if (held_wep != th.hook_index && !th.switched)
		{
			th.prev_weapon = held_wep;
			th.switched = true;
			th.switch_time = now;
			Cbuf_AddText(cmd_local, "weapon_hook\n");
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch hook: switching to the Hook weapon (from %s)\n", held_wep >= 0 ? TouchHUD_WeaponName(held_wep) : "?");
		}
		// fire only with the Hook weapon up: attack during the switch would
		// fire the weapon going away
		if (!want)
			; // the rescue is still aiming: in hand, not fired
		else if (TouchHUD_ActiveWeapon() == th.hook_index || (TouchHUD_ActiveWeapon() < 0 && held_wep == th.hook_index && now - th.switch_time > 0.5))
			th.buttons = 1;
		else if (th.switched && now - th.switch_time > SWITCH_GIVEUP && held_wep != th.hook_index)
			th.switched = false; // the switch never landed (the player picked another)
	}
	else if (want)
		th.buttons = 32; // the offhand hook, or +hook as a key would send it
	if (!want && !prepare && th.switched)
	{
		// take the previous weapon back, unless the player has moved on
		if (held_wep == th.hook_index && th.prev_weapon >= 0 && th.prev_weapon != th.hook_index)
		{
			char cmd[64];
			dpsnprintf(cmd, sizeof(cmd), "weapon_%s\n", TouchHUD_WeaponName(th.prev_weapon));
			Cbuf_AddText(cmd_local, cmd);
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch hook: back to %s", cmd + 7);
		}
		th.switched = false;
		th.prev_weapon = -1;
	}
	if (th.held && !want)
		th.release_time = now;
	th.held = want;
}

// ---------------------------------------------------------------------------
// The rescue
// ---------------------------------------------------------------------------
static float wrap180(float a)
{
	while (a > 180)
		a -= 360;
	while (a < -180)
		a += 360;
	return a;
}

static void angles_to(const vec3_t from, const vec3_t to, float *yaw, float *pitch)
{
	vec3_t d;
	VectorSubtract(to, from, d);
	*yaw = atan2(d[1], d[0]) * (180.0f / M_PI);
	*pitch = -atan2(d[2], sqrt(d[0] * d[0] + d[1] * d[1])) * (180.0f / M_PI);
}

static float angle_err(float yaw, float pitch)
{
	float ey = wrap180(yaw - cl.viewangles[YAW]), ep = pitch - cl.viewangles[PITCH];
	return sqrt(ey * ey + ep * ep);
}

// Turn the view toward yaw/pitch: quick, slowing into it, capped.
static void turn_toward(float yaw, float pitch, float dt)
{
	float ey = wrap180(yaw - cl.viewangles[YAW]), ep = pitch - cl.viewangles[PITCH];
	float err = sqrt(ey * ey + ep * ep), rate, step;
	float cap = bound(TURN_MIN, cl_touch_hook_rescue_turn.value, TURN_MAX);
	if (err < 0.01f)
		return;
	rate = bound(TURN_FLOOR, err * TURN_GAIN, cap);
	step = min(err, rate * dt);
	cl.viewangles[YAW] += ey / err * step;
	cl.viewangles[PITCH] = bound(-89.0f, cl.viewangles[PITCH] + ep / err * step, 89.0f);
}

static void debug_line(const char *fmt, ...) DP_FUNC_PRINTF(1);
static void debug_line(const char *fmt, ...)
{
	va_list args;
	char msg[512];
	if (!cl_touch_aim_debug.integer)
		return;
	va_start(args, fmt);
	dpvsnprintf(msg, sizeof(msg), fmt, args);
	va_end(args);
	Con_Printf("touch hook: %s\n", msg);
}

void TouchHook_Frame(void)
{
	double now = host.realtime, t0;
	float dt = bound(0.001f, (float)cl.realframetime, 0.1f);
	const char *blocked = block_reason();
	qbool allowed, manual, onground;
	vec3_t eye, org, vel;
	tr_frame_t f;
	tr_out_t o;
	float turn_rate = 0;
	char publish[8];

	TouchPause_Frame();
	TouchNade_Frame();
	TouchTurn_Frame();
	if (cl.worldmodel != th.hurt_model)
		th_bind_map();
	th.traces = 0;
	t0 = Sys_DirtyTime();

	th.avail = blocked ? TOUCH_HOOK_NONE : which_hook();
	if (cl_touch_hook_avail.integer != th.avail)
	{
		dpsnprintf(publish, sizeof(publish), "%d", th.avail);
		Cvar_SetQuick(&cl_touch_hook_avail, publish);
	}
	if (cl_touch_aim_debug.integer && th.avail != th.avail_said && !blocked)
	{
		Con_Printf("touch hook: available: %s\n", th.avail == TOUCH_HOOK_OFFHAND ? "the offhand hook (+hook)"
			: th.avail == TOUCH_HOOK_WEAPON ? "the Hook weapon" : "none");
		th.avail_said = th.avail;
	}
	if (cl_touch_aim_debug.integer && blocked && blocked != th.blocked_said)
		Con_Printf("touch hook: inactive: %s\n", blocked);
	th.blocked_said = blocked;

	// the player's own turn since the last frame
	if (th.prev_valid && !cl.fixangle[0])
	{
		float ty = wrap180(cl.viewangles[YAW] - th.prev_angles[YAW]), tp = cl.viewangles[PITCH] - th.prev_angles[PITCH];
		if (fabs(ty) < 60 && fabs(tp) < 60)
			turn_rate = sqrt(ty * ty + tp * tp) / dt;
	}

	manual = !blocked && (th.hud_down || th.cmd_down);
	allowed = !blocked && cl_touch_hook_rescue.integer;
	onground = cl.onground;
	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	VectorCopy(eye, org);
	org[2] -= cl.stats[STAT_VIEWHEIGHT] ? cl.stats[STAT_VIEWHEIGHT] : 35;
	VectorCopy(cl.mvelocity[0], vel);
	if (th.last_org_valid && VectorDistance(org, th.last_org) > TELEPORT_DIST + VectorLength(vel) * dt * 2)
	{
		// teleported or respawned: whatever was going on is over
		if (th.rs.phase != TR_IDLE && th.rs.phase != TR_COOLDOWN)
			debug_line("rescue ended: teleported");
		TouchRescue_Reset(&th.rs);
		th.home_valid = false;
	}
	VectorCopy(org, th.last_org);
	th.last_org_valid = true;
	if (now - th.move_time >= 0.25 || now < th.move_time)
	{
		th.move_speed = th.move_time > 0 && now > th.move_time ? VectorDistance(org, th.move_org) / (now - th.move_time) : 0;
		VectorCopy(org, th.move_org);
		th.move_time = now;
	}
	if (onground && !blocked)
	{
		VectorCopy(org, th.home);
		th.home_valid = true;
	}

	memset(&f, 0, sizeof(f));
	f.now = now;
	f.allowed = allowed;
	f.available = th.avail != TOUCH_HOOK_NONE;
	f.manual = manual || (in_button6.state & 3);
	f.hook_out = th.out_time > 0;
	f.onground = onground;
	f.vz = vel[2];
	f.fall = -1;
	f.turn_rate = turn_rate;
	f.cooldown = bound(COOLDOWN_MIN, cl_touch_hook_rescue_cooldown.value, COOLDOWN_MAX);
	f.ground_below = -1;
	f.speed = th.move_speed;
	// where the fall ends: only while it could matter
	if (allowed && f.available && !onground
		&& (th.rs.phase == TR_AIM || th.rs.phase == TR_HOLD || (th.rs.phase == TR_IDLE && !f.manual && vel[2] < -TR_MIN_FALL_SPEED)))
	{
		tr_world_t w = th_world();
		tr_fall_t fall;
		f.fall = TouchRescue_Predict(&w, org, vel, &fall);
		if (th.rs.phase == TR_IDLE && f.fall != th.fall && cl_touch_aim_debug.integer >= 2)
			debug_line("falling at %.0f u/s from z %.0f: comes down on %s at z %.0f in %.2f s", -vel[2], org[2], tr_outcome_name[f.fall], fall.point[2], fall.time);
		if (th.rs.phase == TR_HOLD)
		{
			tr_fall_t below;
			if (f.fall == TR_LAND && !TouchRescue_LandsClear(&w, &fall, vel))
				f.fall = TR_VOID; // it would come down on the lip, heading out
			if (TouchRescue_Below(&w, org, TR_RELEASE_DROP, &below) == TR_LAND)
				f.ground_below = org[2] - below.point[2];
		}
	}
	th.fall = f.fall;
	if (th.rs.phase == TR_AIM || th.rs.phase == TR_HOLD)
	{
		f.anchor = th.have_anchor;
		if (th.have_anchor)
		{
			float yaw, pitch;
			vec3_t d;
			angles_to(eye, th.anchor.point, &yaw, &pitch);
			f.aim_err = angle_err(yaw, pitch);
			VectorSubtract(th.anchor.point, org, d);
			VectorNormalize(d);
			// the hook has caught once the player gains speed toward the
			// anchor beyond what the fall alone would give: the pull takes
			// a second or so to turn a fast fall around
			if (th.rs.phase == TR_HOLD && th.out_time)
			{
				vec3_t free_vel;
				VectorCopy(th.fire_vel, free_vel);
				free_vel[2] -= cl.movevars_gravity * (now - th.fire_time);
				f.pull_speed = DotProduct(vel, d) - DotProduct(free_vel, d);
			}
			f.attach_time = th.anchor.dist / HOOK_FLY_SPEED + ATTACH_SLACK;
		}
	}
	else if (th.rs.phase == TR_RESTORE)
		f.aim_err = angle_err(th.saved_angles[YAW], th.saved_angles[PITCH]);

	TouchRescue_Step(&th.rs, &f, &o);

	if (o.find_anchor)
	{
		tr_world_t w = th_world();
		tr_from_t from;
		float base_yaw, search_ms;
		vec3_t home;
		if (th.home_valid)
			VectorCopy(th.home, home);
		else
			VectorCopy(org, home);
		if (VectorDistance2(home, org) - (home[2] - org[2]) * (home[2] - org[2]) > 16 * 16)
			base_yaw = atan2(home[1] - org[1], home[0] - org[0]) * (180.0f / M_PI);
		else if (vel[0] * vel[0] + vel[1] * vel[1] > 100)
			base_yaw = atan2(-vel[1], -vel[0]) * (180.0f / M_PI);
		else
			base_yaw = cl.viewangles[YAW];
		VectorCopy(cl.viewangles, th.saved_angles);
		VectorCopy(org, from.org);
		VectorCopy(vel, from.vel);
		from.eye_h = eye[2] - org[2];
		from.home_z = home[2];
		{
			double ts = Sys_DirtyTime();
			th.have_anchor = TouchRescue_FindAnchor(&w, &from, eye, home, base_yaw, bound(RANGE_MIN, cl_touch_hook_rescue_range.value, RANGE_MAX), &th.anchor);
			search_ms = (Sys_DirtyTime() - ts) * 1000;
		}
		debug_line("rescue: %s from z %.0f (last stood at z %.0f, %.0f units away), falling at %.0f u/s",
			th.rs.why, org[2], home[2], sqrt(VectorDistance2(home, org)), -vel[2]);
		if (th.have_anchor)
			debug_line("rescue: anchor at %.0f %.0f %.0f, %.0f units, %s%s (%d traces, %.2f ms)", th.anchor.point[0], th.anchor.point[1], th.anchor.point[2],
				th.anchor.dist, th.anchor.over_ground ? "pull ends over ground" : "high above the ledge",
				th.anchor.late_ok ? "" : ", only if the hook is quick", w.traces, search_ms);
		else
			debug_line("rescue: no anchor in reach (%d traces, %.2f ms)", w.traces, search_ms);
	}
	else if (o.changed)
		debug_line("rescue %s: %s (z %.0f, vz %.0f, moving at %.0f u/s)", tr_phase_name[th.rs.phase], th.rs.why, org[2], vel[2], th.move_speed);
	if (th.rs.phase == TR_HOLD && th.rs.attached == now)
		debug_line("rescue: the hook caught %.2f s after the shot (z %.0f)", now - th.fire_time, org[2]);

	if (o.aim && th.have_anchor)
	{
		float yaw, pitch;
		angles_to(eye, th.anchor.point, &yaw, &pitch);
		turn_toward(yaw, pitch, dt);
	}
	else if (o.restore)
		turn_toward(th.saved_angles[YAW], th.saved_angles[PITCH], dt);
	th.rescue_hold = o.hold;

	hold_frame(manual || th.rescue_hold, o.prepare, now);
	// the hook has left: the catch is timed, and the pull measured, from here
	if (th.rescue_hold && th.buttons && !th.out_time)
	{
		th.out_time = now;
		VectorCopy(vel, th.fire_vel);
		th.fire_time = now;
	}
	if (!th.rescue_hold)
		th.out_time = 0;
	{
		int st = th.rescue_hold || th.rs.phase == TR_AIM ? 2 : th.held ? 1 : 0;
		if (cl_touch_hook_state.integer != st)
		{
			dpsnprintf(publish, sizeof(publish), "%d", st);
			Cvar_SetQuick(&cl_touch_hook_state, publish);
		}
	}

	// offline tactics (bots on this machine only): the autopilot and aim dodge
	// write the move first, so auto dodge and the ledge guard judge it
	TouchTactics_Frame();
	// auto dodge: after the hook, which it stands down for
	TouchDodge_Frame();
	// the ledge guard: after the dodge, which it stands down for
	TouchLedge_Frame();

	// the cost of judging falls, with cl_touch_aim_debug 2: once a second
	if (th.traces)
	{
		th.cost_sum += Sys_DirtyTime() - t0;
		th.cost_frames++;
	}
	if (cl_touch_aim_debug.integer >= 2 && th.cost_frames && now - th.cost_report > 1)
	{
		debug_line("cost: %.3f ms a frame over %d airborne frames", th.cost_sum * 1000 / th.cost_frames, th.cost_frames);
		th.cost_sum = 0;
		th.cost_frames = 0;
		th.cost_report = now;
	}
}

void TouchHook_EndFrame(void)
{
	VectorCopy(cl.viewangles, th.prev_angles);
	th.prev_valid = true;
}

// One line of state, for scripted tests and bug reports.
static void TouchHook_Status_f(cmd_state_t *cmd)
{
	vec3_t eye;
	Matrix4x4_OriginFromMatrix(&r_refdef.view.matrix, eye);
	Con_Printf("touch hook: avail %d held %d buttons %d rescue %s; eye %.0f %.0f %.0f vel %.0f %.0f %.0f onground %d; view %.1f %.1f; hurt volumes %d\n",
		th.avail, th.held, th.buttons, tr_phase_name[th.rs.phase], eye[0], eye[1], eye[2],
		cl.mvelocity[0][0], cl.mvelocity[0][1], cl.mvelocity[0][2], cl.onground, cl.viewangles[PITCH], cl.viewangles[YAW], th.num_hurt);
	TouchDodge_PrintStatus();
	TouchLedge_PrintStatus();
}

void TouchHook_Init(void)
{
	TouchTurn_Init();
	TouchDodge_Init();
	TouchLedge_Init();
	TouchTactics_Init();
	TouchPause_Init();
	TouchNade_Init();
	TouchFE_Init(); // native front-end bridge (touch_frontend.c)
	Cvar_RegisterVariable(&cl_touch_hook_rescue);
	Cvar_RegisterVariable(&cl_touch_hook_rescue_turn);
	Cvar_RegisterVariable(&cl_touch_hook_rescue_range);
	Cvar_RegisterVariable(&cl_touch_hook_rescue_cooldown);
	Cvar_RegisterVariable(&cl_touch_hook_avail);
	Cvar_RegisterVariable(&cl_touch_hook_state);
	Cmd_AddCommand(CF_CLIENT, "+touch_hook", TouchHook_Down_f, "hold the grappling hook from the touch controls' HOOK button (the offhand hook, or the Hook weapon switched to and back)");
	Cmd_AddCommand(CF_CLIENT, "-touch_hook", TouchHook_Up_f, "let go of the grappling hook held by +touch_hook");
	Cmd_AddCommand(CF_CLIENT, "touch_hook_status", TouchHook_Status_f, "print the hook's state (what hook there is, the hold, the fall rescue's phase, position and velocity) and auto dodge's: on or why off, its counts and reaction");
	th.hook_index = -1;
	th.prev_weapon = -1;
	th.mut_ofs0 = th.mut_ofs1 = -1;
	th.avail_said = -1;
	th.fall = -1;
	th.release_time = -100;
	TouchRescue_Reset(&th.rs);
}
