/*
Godnotic: offline tactics. Everything here runs only while the offline
practice gate is open (touch_weapons.h TouchWep_PracticeGate): this process
runs the server, this client is connected to it over the loopback, no other
human is connected, and no demo plays. The gate is checked every frame; with
it closed nothing here reads, draws, steers or picks anything, whatever the
cvars say. That is what makes it fair to read the server directly: the only
opponents are bots on the player's own machine.

- Intel: every enemy's health, armour, held weapon, position, velocity and
  aim (the server's v_angle), read from the local server's edicts. Labels
  over each enemy (cl_touch_practice_intel); whether one aims at the player.
- Targets: the aim helpers count an enemy that is nearly dead, aims at the
  player or carries a strong weapon as nearer the crosshair
  (TouchTactics_TargetBonus), so the fight goes to the one worth taking.
- Weapons: a sure one-shot kill (an instant-hit weapon whose shot beats the
  target's health and armour) is switched to; against a target behind a wall
  the best splash weapon is (TouchTactics_PickWeapon, from the auto-switch).
- Aim dodge (cl_touch_practice_aimdodge): while an enemy with line of sight
  holds its aim on the player, the move steps across that line, toward the
  side with room, changing course at random intervals so the next shot's
  lead is wrong. Only while the stick is idle (or the autopilot drives).
- Autopilot (cl_touch_practice_pilot, on by default in touch mode): while the stick is
  idle, the move keeps the held weapon's best range to the focus enemy and
  circle-strafes it; chases one that is nearly dead; outnumbered or losing,
  it backs away toward the places fewest enemies can see; with nobody in
  sight it fetches a weapon it lacks, health when hurt or armour, on about
  its own level (no route finding), else heads toward the nearest enemy.
  Each direction is chosen from 16 by tracing the player's box: room to
  move, safe ground along the way (no void, lava or slime), a jump over a
  low step onto safe ground, and getting unstuck.

Runs from the hook's frame before auto dodge and the ledge guard, so both
judge the move made here (a projectile dodge or a ledge stop wins).
touch_tactics_status prints the gate and the intel.
*/

#include "quakedef.h"
#include "cl_collision.h"
#include "touch_aim.h"
#include "touch_hook.h"
#include "touch_hud.h"
#include "touch_tactics.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "touch_weapons.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern cvar_t cl_touch_aim_debug;
extern cvar_t cl_forwardspeed;
extern cvar_t cl_sidespeed;

cvar_t cl_touch_esp_enabled = {CF_CLIENT | CF_ARCHIVE, "cl_touch_esp_enabled", "local",
	"Where the see-through, intel and autopilot helpers may run: local (a match against bots on this machine, nobody else connected) or off. "
	"There is no server setting: on a server they would play against people who do not have them"};
cvar_t cl_touch_practice_intel = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_intel", "1",
	"Offline practice only (a match against bots on this machine, nobody else connected): show every enemy's health, armour and weapon, "
	"take the target worth taking first, switch to a weapon that finishes it in one shot, and to a splash weapon when it is behind a wall"};
cvar_t cl_touch_practice_aimdodge = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_aimdodge", "1",
	"Offline practice only (as above): step out of the line of an enemy that is aiming at you, while the stick is idle"};
cvar_t cl_touch_practice_exact = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_exact", "1",
	"Offline practice only (as above): lead shots from each enemy's exact velocity on the local server, and aim at any distance"};
cvar_t cl_touch_practice_exposure = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_exposure", "0",
	"Offline practice only (as above): while the autopilot fights one enemy, prefer spots the other enemies cannot see"};
cvar_t cl_touch_practice_threat = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_threat", "1",
	"Offline practice only (as above): the autopilot sizes up the enemy it fights from its weapon and health: stays out of reach of a "
	"short-range weapon it outranges, closes in on one that outranges it when stronger and keeps out of its sight when weaker, and backs off "
	"to health when one more hit would kill"};
cvar_t cl_touch_practice_detour = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_detour", "1",
	"Offline practice only (as above): during a fight the autopilot steps aside for a pickup close by that is worth it now (health or armour by how hurt you are, "
	"a weapon you lack that does better at this range, ammo for the weapon in hand when low), if no more enemies see it there; for at most 1.5 s"};
cvar_t cl_touch_practice_position = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_position", "0",
	"Offline practice only (as above): during a fight the autopilot moves toward the best nearby spot to fight from: in sight of the enemy, hidden from the others, "
	"at the weapon's range, higher ground, near the pickup it is going for, never a ledge or a dead end"};
cvar_t cl_touch_practice_magnet = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_magnet", "0",
	"Offline practice only (as above): pickups within cl_touch_practice_magnet_radius of you, in clear view, are picked up as if touched. "
	"Changes the game's rules, so off by default; never for bots, never anywhere else"};
cvar_t cl_touch_practice_magnet_radius = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_magnet_radius", "96",
	"Offline practice only: how far (units) the pickup magnet reaches"};
cvar_t cl_touch_practice_objectives = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_objectives", "1",
	"Offline practice only (as above): play the game type: in CTF fetch the enemy flag, carry it home avoiding fights, return your own when dropped, "
	"and go first for the enemy carrying your flag or near your base; in team games go first for whoever is hitting your mates or camping a frozen one, "
	"and keep near your mates when outnumbered"};
cvar_t cl_touch_practice_pilot = {CF_CLIENT | CF_ARCHIVE, "cl_touch_practice_pilot", "1",
	"Offline practice only (as above): while the stick is idle, move for you: keep the weapon's best range and strafe, chase a nearly dead enemy, "
	"back into cover when outnumbered, head for the nearest enemy when none is in sight. Touching the stick takes over"};

#define TT_MAX 256
#define TT_EVAL 0.1     // seconds between direction choices
#define TT_DIRS 16
#define TT_LOOK 192.0f  // units of room looked for in each direction

typedef struct intel_s
{
	qbool live;      // a living player
	qbool enemy;
	qbool seen;      // a clear line from the player's eye
	qbool aiming;    // its aim passes by the player
	qbool frozen;    // Freeze Tag: frozen (no damage; a mate is thawed by standing by it)
	vec3_t org, vel, eye, center;
	vec3_t aimdir;
	float health, armor, eff; // eff: damage that kills it, armour counted
	float dist;
	char weapon[32];
	int kind;        // TW_KIND_* of its weapon's primary
	float dmg;       // that primary's damage
	float team;
	qbool hits_mate; // an enemy whose aim passes by one of the player's mates
} intel_t;

// CTF flags, read from the local server (classname item_flag_team)
#define TT_FLAGS 4
#define FLAG_AT_BASE 1
#define FLAG_DROPPED 2
#define FLAG_CARRIED 3
typedef struct flag_s
{
	float team;
	int status;      // FLAG_AT_BASE, FLAG_DROPPED, FLAG_CARRIED (4: being passed)
	int carrier;     // player edict carrying it, 0 none
	vec3_t base, pos;
} flag_t;

static struct
{
	intel_t e[TT_MAX];
	int maxn;
	qbool open;
	int gate_said;
	const char *why_said;

	int me;
	vec3_t my_org, my_eye, my_center, my_vel;
	float my_eff;
	int focus;
	int num_seen, num_aiming;
	double wep_next;

	// steering
	int buttons;
	qbool steering;
	double eval_next;
	vec3_t move_dir;
	qbool jump_dir;
	double strafe_until;
	float strafe_sign;
	double dodge_until;
	int dodge_from;
	vec3_t dodge_dir;
	double slow_since, stuck_until;
	vec3_t stuck_dir;
	const char *mode, *mode_said;
	int cover_skip;      // the enemy choose()'s cover leaves out (the one fought), 0 none
	// game type
	qbool team_game;
	float my_team;
	flag_t flags[TT_FLAGS];
	int num_flags, my_flag, enemy_flag; // indices, -1 none
	double flags_next;
	int dodges, stucks;
	// cl_touch_aim_debug 3: the last moves, printed when the player dies
	struct { double t; vec3_t org, dir; float score; const char *mode; qbool ground; } trail[16];
	int trail_n;
	qbool was_alive;

	// pickups
	double items_next;
	int item;            // server edict of the pickup being fetched, 0 none
	vec3_t item_org;
	vec3_t last_safe;  // where the player last stood on safe ground
	qbool have_safe;
	double item_since;
	float item_best_dist;
	int item_ban[8];
	double item_ban_until[8];
	int pickups_tried;

	// a pickup close by, taken during a fight
	double near_next, near_since;
	int near_item;
	vec3_t near_org;
	int near_ban;
	double near_ban_until;
	int detours, magnet_pickups;
	// the spot to fight from
	double spot_next;
	qbool have_spot;
	vec3_t spot;
} tt;

// cl_touch_esp_enabled over the offline gate: "off" closes it; anything but
// "local" or "off" is taken as "local" (said once).
int TouchTactics_EspGate(int g)
{
	static char said[32];
	const char *v = cl_touch_esp_enabled.string;
	if (!strcmp(v, "off") || !strcmp(v, "0"))
		return TW_GATE_NO_SERVER;
	if (strcmp(v, "local") && strcmp(said, v))
	{
		Con_Printf("cl_touch_esp_enabled \"%s\": only \"local\" or \"off\"; using local (these helpers never run on a server with other people)\n", v);
		dp_strlcpy(said, v, sizeof(said));
	}
	return g;
}

static const char *gate_why(int g)
{
	if (g != TW_GATE_OPEN && (!strcmp(cl_touch_esp_enabled.string, "off") || !strcmp(cl_touch_esp_enabled.string, "0")))
		return "cl_touch_esp_enabled is off";
	return touch_practice_why[g];
}

static int gate(void)
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
			if (c->active && c->netconnection && LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) != LHNETADDRESSTYPE_LOOP)
				e.other_humans++;
		}
	return TouchTactics_EspGate(TouchWep_PracticeGate(&e));
}

static qbool line_clear(const vec3_t a, const vec3_t b)
{
	trace_t tr = CL_TraceLine(a, b, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID, 0, 0,
		collision_extendmovelength.value, true, false, NULL, true, false);
	return !tr.startsolid && tr.fraction >= 0.999f;
}

// Damage that kills a player with health h and armour a: armour takes
// g_balance_armor_blockpercent of each hit while it lasts.
static float eff_health(float h, float a)
{
	float b = Cvar_VariableValueOr(&cvars_all, "g_balance_armor_blockpercent", 0.7f, ~0);
	b = bound(0.0f, b, 0.99f);
	if (a >= h * b / (1 - b))
		return h / (1 - b);
	return h + a;
}

static const char *sv_string(prvm_prog_t *prog, prvm_edict_t *ed, int ofs)
{
	const char *s = PRVM_GetString(prog, PRVM_EDICTFIELDVALUE(ed, ofs)->string);
	return s ? s : "";
}

// The weapon each player holds: its weapon entity (owner = the player) has
// .m_weapon, the weapon's registry object, whose netname is the weapon.
static void read_weapons(prvm_prog_t *prog)
{
	int mw = PRVM_ED_FindFieldOffset(prog, "m_weapon"), i;
	for (i = 0; i < TT_MAX; i++)
		tt.e[i].weapon[0] = 0;
	if (mw < 0)
		return;
	for (i = svs.maxclients + 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i), *wed;
		int owner, we;
		const char *name;
		if (ed->free)
			continue;
		owner = PRVM_serveredictedict(ed, owner);
		if (owner < 1 || owner > svs.maxclients || owner >= TT_MAX || tt.e[owner].weapon[0])
			continue;
		we = PRVM_EDICTFIELDVALUE(ed, mw)->edict;
		if (we <= 0 || we >= prog->num_edicts)
			continue;
		wed = PRVM_EDICT_NUM(we);
		name = sv_string(prog, wed, prog->fieldoffsets.netname);
		if (*name && strcmp(name, "null"))
		{
			dp_strlcpy(tt.e[owner].weapon, name, sizeof(tt.e[owner].weapon));
		}
	}
}

// Freeze Tag's frozen stat on a player edict (0 when the mod has no such field).
static qbool is_frozen(prvm_prog_t *prog, prvm_edict_t *ed)
{
	static const void *for_defs;
	static int ofs = -1;
	if (for_defs != (const void *)prog->fielddefs)
	{
		for_defs = prog->fielddefs;
		ofs = PRVM_ED_FindFieldOffset(prog, "stat_FROZEN"); // Xonotic's STAT(FROZEN, player)
		if (ofs < 0)
			ofs = PRVM_ED_FindFieldOffset(prog, "frozen");
	}
	return ofs >= 0 && PRVM_EDICTFIELDVALUE(ed, ofs)->_int != 0;
}

// A field of ed by name (cached per loaded progs): a float or an int.
static int field_ofs(prvm_prog_t *prog, const char *name, int *is_float)
{
	mdef_t *d = PRVM_ED_FindField(prog, name);
	if (!d)
		return -1;
	*is_float = (d->type & ~DEF_SAVEGLOBAL) == ev_float;
	return d->ofs;
}

// CTF: every flag's team, state, carrier, base and where it is now.
static void read_flags(prvm_prog_t *prog)
{
	static const void *for_defs;
	static int ofs_status = -1, ofs_base = -1, status_float;
	int i, base_float;
	tt.num_flags = 0;
	tt.my_flag = tt.enemy_flag = -1;
	if (!cl_touch_practice_objectives.integer)
		return;
	if (for_defs != (const void *)prog->fielddefs)
	{
		for_defs = prog->fielddefs;
		ofs_status = field_ofs(prog, "ctf_status", &status_float);
		ofs_base = field_ofs(prog, "dropped_origin", &base_float); // ctf_spawnorigin
	}
	if (ofs_status < 0 || ofs_base < 0)
		return;
	for (i = svs.maxclients + 1; i < prog->num_edicts && tt.num_flags < TT_FLAGS; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		flag_t *fl;
		int carrier;
		if (ed->free || strcmp(sv_string(prog, ed, prog->fieldoffsets.classname), "item_flag_team"))
			continue;
		fl = &tt.flags[tt.num_flags];
		fl->team = PRVM_serveredictfloat(ed, team);
		fl->status = status_float ? (int)ed->fields.fp[ofs_status] : ed->fields.ip[ofs_status];
		VectorCopy(ed->fields.fp + ofs_base, fl->base);
		carrier = PRVM_NUM_FOR_EDICT(PRVM_PROG_TO_EDICT(PRVM_serveredictedict(ed, owner)));
		fl->carrier = fl->status == FLAG_CARRIED && carrier >= 1 && carrier <= svs.maxclients ? carrier : 0;
		if (fl->carrier)
			VectorCopy(PRVM_serveredictvector(PRVM_EDICT_NUM(fl->carrier), origin), fl->pos);
		else
			VectorCopy(PRVM_serveredictvector(ed, origin), fl->pos);
		if (fl->team == tt.my_team)
			tt.my_flag = tt.num_flags;
		else if (tt.enemy_flag < 0)
			tt.enemy_flag = tt.num_flags;
		tt.num_flags++;
	}
}

// What the game type adds to an enemy's priority: the one carrying the
// player's flag, one near the player's flag base, one hitting a mate.
static float objective_bonus(int n)
{
	const intel_t *t = &tt.e[n];
	float b = 0;
	if (!cl_touch_practice_objectives.integer || !t->live || !t->enemy || t->frozen)
		return 0;
	if (tt.my_flag >= 0)
	{
		const flag_t *mf = &tt.flags[tt.my_flag];
		if (mf->carrier == n)
			b += 3;
		else if (VectorDistance(t->org, mf->base) < 800)
			b += 1;
	}
	if (t->hits_mate)
		b += 1;
	// (one standing by a frozen mate, camping the thaw, was tried first:
	// Freeze Tag went 24 / 17 -> 17 / 22 and 20 / 16 -> 7 / 23)
	return b;
}

static void wp_check(prvm_prog_t *prog, double now);

static void read_intel(double now)
{
	prvm_prog_t *prog = SVVM_prog;
	int i, n, team_game = Cvar_VariableValue(&cvars_all, "teamplay", ~0) != 0;
	float my_team = 0;
	prvm_edict_t *me;

	tt.me = 0;
	for (i = 0; i < svs.maxclients; i++)
	{
		const client_t *c = &svs.clients[i];
		if (c->active && c->netconnection && LHNETADDRESS_GetAddressType(&c->netconnection->peeraddress) == LHNETADDRESSTYPE_LOOP)
		{
			tt.me = i + 1;
			break;
		}
	}
	tt.maxn = min(svs.maxclients, TT_MAX - 1);
	for (i = 0; i <= tt.maxn; i++)
		tt.e[i].live = false;
	if (!tt.me || !prog->loaded)
		return;
	if (now >= tt.wep_next)
	{
		read_weapons(prog);
		tt.wep_next = now + 0.25;
	}
	me = PRVM_EDICT_NUM(tt.me);
	VectorCopy(PRVM_serveredictvector(me, origin), tt.my_org);
	VectorCopy(PRVM_serveredictvector(me, velocity), tt.my_vel);
	VectorAdd(tt.my_org, PRVM_serveredictvector(me, view_ofs), tt.my_eye);
	VectorCopy(tt.my_org, tt.my_center);
	tt.my_center[2] += 10;
	tt.my_eff = eff_health(PRVM_serveredictfloat(me, health), PRVM_serveredictfloat(me, armorvalue));
	if (PRVM_serveredictfloat(me, health) <= 0)
		tt.have_safe = false; // a new life starts somewhere else
	if (cl_touch_aim_debug.integer >= 3)
	{
		qbool alive = PRVM_serveredictfloat(me, health) > 0;
		if (tt.was_alive && !alive)
		{
			int k;
			Con_Printf("touch tactics: died at %.0f %.0f %.0f; last moves:\n", tt.my_org[0], tt.my_org[1], tt.my_org[2]);
			for (k = 0; k < 16; k++)
			{
				int j = (tt.trail_n + k) & 15;
				if (tt.trail[j].t > 0 && now - tt.trail[j].t < 3)
					Con_Printf("  -%.2fs %s at %.0f %.0f %.0f dir %.2f %.2f score %.1f ground %d\n", now - tt.trail[j].t, tt.trail[j].mode ? tt.trail[j].mode : "?",
						tt.trail[j].org[0], tt.trail[j].org[1], tt.trail[j].org[2], tt.trail[j].dir[0], tt.trail[j].dir[1], tt.trail[j].score, tt.trail[j].ground);
			}
		}
		tt.was_alive = alive;
	}
	my_team = PRVM_serveredictfloat(me, team);
	tt.team_game = team_game;
	tt.my_team = my_team;
	tt.num_seen = tt.num_aiming = 0;

	for (n = 1; n <= tt.maxn; n++)
	{
		intel_t *t = &tt.e[n];
		prvm_edict_t *ed;
		vec3_t to, ang;
		float along;
		const touch_wep_t *w;
		if (n == tt.me || !svs.clients[n - 1].active)
			continue;
		ed = PRVM_EDICT_NUM(n);
		if (strcmp(sv_string(prog, ed, prog->fieldoffsets.classname), "player")
			|| PRVM_serveredictfloat(ed, health) <= 0 || PRVM_serveredictfloat(ed, deadflag)
			|| PRVM_serveredictfloat(ed, solid) == SOLID_NOT)
			continue;
		t->live = true;
		t->team = PRVM_serveredictfloat(ed, team);
		t->enemy = !team_game || t->team != my_team;
		t->hits_mate = false;
		t->frozen = is_frozen(prog, ed);
		t->health = PRVM_serveredictfloat(ed, health);
		t->armor = PRVM_serveredictfloat(ed, armorvalue);
		t->eff = eff_health(t->health, t->armor);
		VectorCopy(PRVM_serveredictvector(ed, origin), t->org);
		VectorCopy(PRVM_serveredictvector(ed, velocity), t->vel);
		VectorAdd(t->org, PRVM_serveredictvector(ed, view_ofs), t->eye);
		VectorCopy(t->org, t->center);
		t->center[2] += 10;
		t->dist = VectorDistance(tt.my_eye, t->center);
		w = TouchWep_Find(t->weapon);
		t->kind = w ? w->pri.kind : TW_KIND_NONE;
		t->dmg = w ? w->pri.damage : 0;
		if (!t->enemy || t->frozen)
		{
			t->seen = t->aiming = false;
			continue;
		}
		t->seen = line_clear(tt.my_eye, t->center) || line_clear(tt.my_eye, t->eye);
		VectorCopy(PRVM_serveredictvector(ed, v_angle), ang);
		AngleVectors(ang, t->aimdir, NULL, NULL);
		VectorSubtract(tt.my_center, t->eye, to);
		along = DotProduct(to, t->aimdir);
		t->aiming = false;
		if (t->seen && along > 0 && t->kind != TW_KIND_NONE)
		{
			vec3_t miss;
			VectorMA(to, -along, t->aimdir, miss);
			// the body is 32 wide; a little more for spread and a lead
			t->aiming = VectorLength(miss) < 36 + along * 0.03f;
		}
		if (t->seen)
			tt.num_seen++;
		if (t->aiming)
			tt.num_aiming++;
	}

	// team games: an enemy whose aim passes by a mate it has a line to
	if (team_game)
		for (n = 1; n <= tt.maxn; n++)
		{
			intel_t *t = &tt.e[n];
			int m;
			if (!t->live || !t->enemy || t->frozen || t->kind == TW_KIND_NONE)
				continue;
			for (m = 1; m <= tt.maxn && !t->hits_mate; m++)
			{
				const intel_t *mt = &tt.e[m];
				vec3_t to, miss;
				float along;
				if (!mt->live || mt->enemy || mt->frozen)
					continue;
				VectorSubtract(mt->center, t->eye, to);
				along = DotProduct(to, t->aimdir);
				if (along <= 0)
					continue;
				VectorMA(to, -along, t->aimdir, miss);
				t->hits_mate = VectorLength(miss) < 36 + along * 0.03f && line_clear(t->eye, mt->center);
			}
		}
	if (now >= tt.flags_next)
	{
		read_flags(prog);
		if (tt.num_flags)
			wp_check(prog, now);
		tt.flags_next = now + 0.2;
	}

	// the focus: in sight, nearly dead, aiming at the player, near, and what
	// the game type makes urgent
	{
		int best = 0;
		float bs = -1e9f;
		for (n = 1; n <= tt.maxn; n++)
		{
			const intel_t *t = &tt.e[n];
			float s;
			if (!t->live || !t->enemy || t->frozen)
				continue;
			s = (t->seen ? 3 : 0) + 2 * (1 - min(t->eff, 200.0f) / 200) + (t->aiming ? 2 : 0) - t->dist / 800 + objective_bonus(n);
			if (n == tt.focus)
				s += 1;
			if (s > bs)
			{
				bs = s;
				best = n;
			}
		}
		tt.focus = best;
	}
}

qbool TouchTactics_Open(void)
{
	return tt.open;
}

qbool TouchTactics_ExactVelocity(int n, float *vel)
{
	if (!tt.open || !cl_touch_practice_exact.integer || n < 1 || n > tt.maxn || !tt.e[n].live)
		return false;
	VectorCopy(tt.e[n].vel, vel);
	return true;
}

float TouchTactics_TargetBonus(int entnum)
{
	const intel_t *t;
	float b;
	if (!tt.open || !cl_touch_practice_intel.integer || entnum < 1 || entnum > tt.maxn)
		return 0;
	t = &tt.e[entnum];
	if (!t->live || !t->enemy)
		return 0;
	if (t->frozen)
		return -30; // Freeze Tag: a frozen enemy takes no damage; anyone else first
	b = 2.5f * (1 - min(t->eff, 200.0f) / 200);
	if (t->aiming)
		b += 1.5f;
	if (t->dmg >= 80)
		b += 0.5f;
	b += objective_bonus(entnum);
	return min(b, 6.0f);
}

int TouchTactics_PickWeapon(int entnum, float dist, const char *const *names, const qbool *ready, int n, int heldpos)
{
	const intel_t *t;
	int i, best = -1;
	float bv = 0;
	if (!tt.open || !cl_touch_practice_intel.integer || entnum < 1 || entnum > tt.maxn)
		return -1;
	t = &tt.e[entnum];
	if (!t->live || !t->enemy || t->frozen)
		return -1;
	if (dist > 900)
	{
		// Exact lead aims at any range; with nothing that reaches that far
		// ready, keep the held weapon rather than drop to the Blaster
		static const char *const longrange[] = { "vortex", "rifle", "vaporizer", "machinegun", "hlac", "electro",
			"devastator", "hagar", "seeker", "crylink", "arc", "mortar", "minelayer", "fireball", NULL };
		qbool any = false;
		int k;
		for (i = 0; i < n && !any; i++)
			for (k = 0; longrange[k] && !any; k++)
				any = ready[i] && !strcmp(names[i], longrange[k]);
		if (!any)
			return heldpos;
	}
	if (!t->seen)
	{
		// behind a wall: the blast has to do it
		for (i = 0; i < n; i++)
		{
			const touch_wep_t *w = TouchWep_Find(names[i]);
			float v;
			if (!ready[i] || !w || (w->flags & TW_NO_SWITCH_TO) || w->pri.splash < 100 || !TouchWep_Usable(w, dist))
				continue;
			v = w->pri.damage * w->pri.splash / 100;
			if (i == heldpos)
				v *= 1.25f;
			if (v > bv)
			{
				bv = v;
				best = i;
			}
		}
		if (best >= 0 && best != heldpos && cl_touch_aim_debug.integer)
			Con_Printf("touch tactics: player %d behind a wall at %.0f units: %s\n", entnum, dist, names[best]);
		return best;
	}
	if (t->eff > 160)
		return -1;
	// one sure shot that kills it
	for (i = 0; i < n; i++)
	{
		const touch_wep_t *w = TouchWep_Find(names[i]);
		float dmg, v;
		if (!ready[i] || !w || (w->flags & TW_NO_SWITCH_TO) || w->pri.kind != TW_KIND_HITSCAN || !TouchWep_Usable(w, dist))
			continue;
		dmg = w->pri.damage;
		if (w->pri.spread > 0.05f)
			dmg *= bound(0.0f, 1 - (dist - 200) / 800, 1.0f); // pellets spread
		if (dmg < t->eff)
			continue;
		v = 1000 - (dmg - t->eff) + (i == heldpos ? 500 : 0);
		if (v > bv)
		{
			bv = v;
			best = i;
		}
	}
	if (best >= 0 && best != heldpos && cl_touch_aim_debug.integer)
		Con_Printf("touch tactics: finish player %d (%.0f health, %.0f armour) with %s\n", entnum, t->health, t->armor, names[best]);
	return best;
}

// ---------------------------------------------------------------------------
// Steering
// ---------------------------------------------------------------------------

// The best range for a weapon: where it does its damage and is safe to use.
static float best_range(const char *wep)
{
	static const struct { const char *n; float r; } ranges[] =
	{
		{ "shotgun", 260 }, { "machinegun", 700 }, { "arc", 450 }, { "mortar", 650 }, { "minelayer", 500 },
		{ "electro", 600 }, { "crylink", 400 }, { "hlac", 550 }, { "vortex", 1200 }, { "rifle", 1300 },
		{ "vaporizer", 1100 }, { "hagar", 550 }, { "seeker", 700 }, { "devastator", 750 }, { "fireball", 800 },
		{ "blaster", 350 },
	};
	int i;
	for (i = 0; i < (int)(sizeof(ranges) / sizeof(ranges[0])); i++)
		if (!strcmp(ranges[i].n, wep))
			return ranges[i].r;
	return 600;
}

// Ground within a step-down of p (a box at foot height), and not lava, slime
// or a no-drop (death) volume above it.
static qbool hazard_below(const vec3_t p, float floor_z)
{
	const int bad = SUPERCONTENTS_LAVA | SUPERCONTENTS_SLIME | SUPERCONTENTS_NODROP;
	vec3_t at;
	float z;
	// every 12 units from just over the floor up to p: a liquid surface lies
	// somewhere in between, not always at the floor or halfway
	VectorCopy(p, at);
	for (z = floor_z + 4; z < p[2]; z += 12)
	{
		at[2] = z;
		if (CL_PointSuperContents(at) & bad)
			return true;
	}
	at[2] = p[2];
	return (CL_PointSuperContents(at) & bad) != 0;
}

static qbool safe_ground(const vec3_t p)
{
	static const vec3_t mins = { -16, -16, 0 }, maxs = { 16, 16, 44 };
	vec3_t down;
	trace_t box, thin;
	VectorCopy(p, down);
	down[2] -= 200;
	box = CL_TraceBox(p, mins, maxs, down, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
		collision_extendmovelength.value, true, false, NULL, false);
	// Straight down from the centre: the full box can rest on the lip of a
	// trench, or start inside a wall it hugs, while the player's middle hangs
	// over lava (Stormkeep's lava trench runs along a wall).
	thin = CL_TraceLine(p, down, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
		collision_extendmovelength.value, true, false, NULL, false, false);
	if (thin.startsolid)
		return box.startsolid || box.fraction < 1; // inside a wall: the run check has it
	if (hazard_below(p, thin.fraction < 1 ? thin.endpos[2] : down[2]))
		return false;
	if (!box.startsolid && box.fraction < 1 && hazard_below(p, box.endpos[2]))
		return false;
	return box.startsolid || box.fraction < 1 || thin.fraction < 1;
}

// Room to move along dir from the player's feet: the box's free run, whether
// there is ground (no void, lava or slime) at its end, and whether a jump
// clears what stops it.
static float room(const vec3_t dir, qbool *ground, qbool *jump)
{
	static const vec3_t mins = { -16, -16, 0 }, maxs = { 16, 16, 44 };
	vec3_t start, end;
	trace_t tr, tr2;
	float run;
	VectorCopy(tt.my_org, start);
	start[2] += -24 + 20; // feet, plus a step
	VectorMA(start, TT_LOOK, dir, end);
	tr = CL_TraceBox(start, mins, maxs, end, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
		collision_extendmovelength.value, true, false, NULL, false);
	run = tr.startsolid ? 0 : tr.fraction * TT_LOOK;
	*jump = false;
	if (run < 48 && !tr.startsolid)
	{
		vec3_t js, je;
		VectorCopy(start, js);
		js[2] += 40;
		VectorMA(js, 64, dir, je);
		tr2 = CL_TraceBox(js, mins, maxs, je, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
			collision_extendmovelength.value, true, false, NULL, false);
		// a jump only onto safe ground on the far side
		*jump = !tr2.startsolid && tr2.fraction > 0.95f && safe_ground(je);
	}
	// ground along the run: a third of the way, two thirds, and its end (a
	// short step where there is no run), so a pit or a lava pool is seen
	// before the player is over it
	{
		int k;
		*ground = true;
		for (k = 1; k <= 3 && *ground; k++)
		{
			VectorMA(start, max(run * k / 3.0f, 24.0f), dir, end);
			*ground = safe_ground(end);
		}
	}
	return run;
}

// How many enemies in sight could see the player at p (eye height), the
// one being fought (skip) aside.
static int seen_from(const vec3_t p, int skip)
{
	int n, c = 0;
	vec3_t eye;
	VectorCopy(p, eye);
	eye[2] = tt.my_eye[2];
	for (n = 1; n <= tt.maxn; n++)
		if (n != skip && tt.e[n].live && tt.e[n].enemy && tt.e[n].seen && line_clear(tt.e[n].eye, eye))
			c++;
	return c;
}

// Of 16 directions, the one that goes most where `want` points, has room and
// ground, keeps the course (no jitter), and with `cover` set, is seen by the
// fewest enemies at its end.
static void choose(const vec3_t want, qbool cover, int skip)
{
	int k, best = -1;
	float bs = -1e9f;
	vec3_t bdir = { 0, 0, 0 };
	qbool bjump = false, bground = false;
	vec3_t feet, back = { 0, 0, 0 };
	// standing in lava or slime (or over it): the way back to the last safe
	// spot beats everything else
	VectorCopy(tt.my_org, feet);
	feet[2] += -24 + 20;
	if (safe_ground(feet))
	{
		VectorCopy(tt.my_org, tt.last_safe);
		tt.have_safe = true;
	}
	else if (tt.have_safe)
	{
		VectorSubtract(tt.last_safe, tt.my_org, back);
		back[2] = 0;
		VectorNormalize(back);
	}
	if (VectorLength2(back) == 0)
	{
		// sliding toward a hazard (down a ramp into a lava pit): where the
		// player's momentum carries it in 0.35 s must be safe too
		vec3_t ahead;
		VectorMA(feet, 0.35f, tt.my_vel, ahead);
		ahead[2] = feet[2];
		if (!safe_ground(ahead))
		{
			VectorSet(back, -tt.my_vel[0], -tt.my_vel[1], 0);
			VectorNormalize(back);
		}
	}
	for (k = 0; k < TT_DIRS; k++)
	{
		float a = k * (2 * M_PI / TT_DIRS), run, s;
		vec3_t dir = { cos(a), sin(a), 0 };
		qbool ground, jump;
		run = room(dir, &ground, &jump);
		s = 2 * DotProduct(dir, want) + 1.5f * (jump ? 1 : run / TT_LOOK) + 0.5f * DotProduct(dir, tt.move_dir);
		if (!ground)
			s -= 6;
		s += 5 * DotProduct(dir, back);
		if (run < 24 && !jump)
			s -= 3;
		if (cover && run > 48)
		{
			vec3_t p;
			VectorMA(tt.my_org, run * 0.8f, dir, p);
			s -= 1.5f * seen_from(p, skip);
		}
		if (s > bs)
		{
			bs = s;
			best = k;
			VectorCopy(dir, bdir);
			bjump = jump;
			bground = ground;
		}
	}
	if (best < 0)
		return;
	VectorCopy(bdir, tt.move_dir);
	{
		int j = tt.trail_n++ & 15;
		tt.trail[j].t = host.realtime;
		VectorCopy(tt.my_org, tt.trail[j].org);
		VectorCopy(bdir, tt.trail[j].dir);
		tt.trail[j].score = bs;
		tt.trail[j].mode = tt.mode;
		tt.trail[j].ground = bground;
	}
	tt.jump_dir = bjump;
}

// Whether the player owns weapon `netname`.
static qbool owns(const char *netname)
{
	int i;
	for (i = 0; i < TouchHUD_NumWeapons(); i++)
		if (TouchHUD_WeaponOwned(i) && !strcmp(TouchHUD_WeaponName(i), netname))
			return true;
	return false;
}

// What a pickup is worth to the player now: a weapon not owned, health when
// hurt, armour when short of it.
static float item_value(const char *cname, float health, float armor)
{
	if (!strncmp(cname, "weapon_", 7))
	{
		const char *w = cname + 7;
		if (owns(w))
			return 0;
		return !strcmp(w, "vortex") || !strcmp(w, "devastator") || !strcmp(w, "vaporizer") || !strcmp(w, "electro") ? 3 : 2;
	}
	if (!strncmp(cname, "item_health", 11))
	{
		if (strstr(cname, "mega"))
			return health < 200 ? 3 : 0;
		return health < 100 ? (100 - health) / 25 : 0;
	}
	if (!strncmp(cname, "item_armor", 10))
	{
		if (strstr(cname, "mega") || strstr(cname, "large") || strstr(cname, "big"))
			return armor < 150 ? 2 : 0;
		return armor < 100 ? 0.5f : 0;
	}
	return 0;
}

static qbool item_banned(int e, double now)
{
	int i;
	for (i = 0; i < 8; i++)
		if (tt.item_ban[i] == e && now < tt.item_ban_until[i])
			return true;
	return false;
}

// The pickup to fetch: the best value for the way, on about the player's
// level (no route finding: one that needs a jump pad or a lift is left).
static void pick_item(double now)
{
	prvm_prog_t *prog = SVVM_prog;
	prvm_edict_t *me = PRVM_EDICT_NUM(tt.me);
	float health = PRVM_serveredictfloat(me, health), armor = PRVM_serveredictfloat(me, armorvalue), bs = 0.6f;
	int i, best = 0;
	vec3_t bo = { 0, 0, 0 };
	for (i = svs.maxclients + 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		const char *cname;
		float v, d, dz;
		vec3_t o;
		if (ed->free || PRVM_serveredictfloat(ed, solid) != SOLID_TRIGGER)
			continue;
		cname = sv_string(prog, ed, prog->fieldoffsets.classname);
		if (strncmp(cname, "weapon_", 7) && strncmp(cname, "item_", 5))
			continue;
		v = item_value(cname, health, armor);
		if (v <= 0 || item_banned(i, now))
			continue;
		VectorCopy(PRVM_serveredictvector(ed, origin), o);
		dz = fabs(o[2] - (tt.my_org[2] - 24));
		d = VectorDistance(o, tt.my_org);
		if (d > 2500 || dz > 120)
			continue;
		{
			// not one standing in or over lava or slime
			vec3_t q;
			VectorCopy(o, q);
			q[2] += 20;
			if (!safe_ground(q))
				continue;
			// nor one at the edge of a pit: the approach slides in
			{
				int k;
				qbool edge = false;
				for (k = 0; k < 8 && !edge; k++)
				{
					vec3_t r;
					VectorSet(r, q[0] + 80 * cos(k * M_PI / 4), q[1] + 80 * sin(k * M_PI / 4), q[2]);
					edge = !safe_ground(r);
				}
				if (edge)
					continue;
			}
		}
		v /= 1 + d / 600;
		if (i == tt.item)
			v *= 1.3f; // keep the one on the way
		if (v > bs)
		{
			bs = v;
			best = i;
			VectorCopy(o, bo);
		}
	}
	if (best != tt.item)
	{
		tt.item = best;
		tt.item_since = now;
		tt.item_best_dist = 1e9f;
		if (best)
			tt.pickups_tried++;
		if (best && cl_touch_aim_debug.integer)
			Con_Printf("touch tactics: fetch %s at %.0f %.0f %.0f, %.0f units\n", sv_string(prog, PRVM_EDICT_NUM(best), prog->fieldoffsets.classname), bo[0], bo[1], bo[2], VectorDistance(bo, tt.my_org));
	}
	VectorCopy(bo, tt.item_org);
}

// Fetching a pickup that gets no nearer for 4 seconds: leave it for 20.
static void item_progress(double now)
{
	float d;
	int i, slot = 0;
	if (!tt.item)
		return;
	d = VectorDistance(tt.item_org, tt.my_org);
	if (d < tt.item_best_dist - 32)
	{
		tt.item_best_dist = d;
		tt.item_since = now;
		return;
	}
	if (now - tt.item_since < 4)
		return;
	for (i = 0; i < 8; i++)
		if (now >= tt.item_ban_until[i])
			slot = i;
	tt.item_ban[slot] = tt.item;
	tt.item_ban_until[slot] = now + 20;
	if (cl_touch_aim_debug.integer)
		Con_Printf("touch tactics: pickup %d out of reach, leaving it\n", tt.item);
	tt.item = 0;
}

// Ammo of a kind on the player (Xonotic's ammo_* fields; float or int).
static float ammo_of(prvm_prog_t *prog, prvm_edict_t *ed, const char *field)
{
	mdef_t *d = PRVM_ED_FindField(prog, field);
	if (!d)
		return -1;
	if ((d->type & ~DEF_SAVEGLOBAL) == ev_float)
		return ed->fields.fp[d->ofs];
	return (float)ed->fields.ip[d->ofs];
}

// The ammo kind a weapon fires, NULL for none.
static const char *ammo_kind(const char *w)
{
	static const struct { const char *w, *a; } k[] =
	{
		{ "shotgun", "item_shells" }, { "machinegun", "item_bullets" }, { "rifle", "item_bullets" },
		{ "mortar", "item_rockets" }, { "devastator", "item_rockets" }, { "hagar", "item_rockets" },
		{ "minelayer", "item_rockets" }, { "seeker", "item_rockets" },
		{ "vortex", "item_cells" }, { "electro", "item_cells" }, { "crylink", "item_cells" },
		{ "hlac", "item_cells" }, { "arc", "item_cells" }, { "vaporizer", "item_cells" },
	};
	int i;
	for (i = 0; i < (int)(sizeof(k) / sizeof(k[0])); i++)
		if (!strcmp(k[i].w, w))
			return k[i].a;
	return NULL;
}

static float best_range(const char *wep);
static int seen_from(const vec3_t p, int skip);

// What a pickup close by is worth in the middle of a fight: health and
// armour by how hurt the player is, a weapon not owned that does better at
// the focus's range than the one in hand, ammo for the one in hand when low.
static float fight_item_value(prvm_prog_t *prog, prvm_edict_t *me, const char *cname, float health, float armor)
{
	const char *held = tt.e[tt.me].weapon, *ak;
	if (!strncmp(cname, "item_health", 11))
	{
		if (strstr(cname, "mega"))
			return health < 150 ? 4 : 0;
		if (strstr(cname, "big"))
			return health < 100 ? (125 - health) / 25 : 0;
		return health < 90 ? (100 - health) / 30 : 0;
	}
	if (!strncmp(cname, "item_armor", 10))
	{
		if (strstr(cname, "mega") || strstr(cname, "large") || strstr(cname, "big"))
			return armor < 100 ? 2.5f : 0;
		return armor < 50 && health < 100 ? 0.6f : 0;
	}
	if (!strncmp(cname, "weapon_", 7) && tt.focus)
	{
		const char *w = cname + 7;
		float d = tt.e[tt.focus].dist;
		if (owns(w))
			return 0;
		// nearer its best range to this enemy than the weapon in hand
		return fabs(best_range(w) - d) + 150 < fabs(best_range(held) - d) ? 2 : 0;
	}
	ak = ammo_kind(held);
	if (ak && !strcmp(cname, ak))
	{
		float a = ammo_of(prog, me, !strcmp(ak, "item_shells") ? "ammo_shells" : !strcmp(ak, "item_bullets") ? "ammo_nails"
			: !strcmp(ak, "item_rockets") ? "ammo_rockets" : "ammo_cells");
		return a >= 0 && a < 15 ? 1.5f : 0;
	}
	return 0;
}

// A pickup close by worth a detour during a fight: within 350 units, on the
// player's level, a clear way there with safe ground, and no more enemies
// seeing the player there than now.
static void pick_near(double now)
{
	prvm_prog_t *prog = SVVM_prog;
	prvm_edict_t *me = PRVM_EDICT_NUM(tt.me);
	float health = PRVM_serveredictfloat(me, health), armor = PRVM_serveredictfloat(me, armorvalue), bs = 0;
	int i, best = 0, seen_now = seen_from(tt.my_org, 0);
	vec3_t bo = { 0, 0, 0 };
	for (i = svs.maxclients + 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		const char *cname;
		float v, d;
		vec3_t o, q, dir;
		trace_t tr;
		static const vec3_t mins = { -16, -16, 0 }, maxs = { 16, 16, 44 };
		if (ed->free || PRVM_serveredictfloat(ed, solid) != SOLID_TRIGGER)
			continue;
		VectorCopy(PRVM_serveredictvector(ed, origin), o);
		d = VectorDistance(o, tt.my_org);
		if (d > 350 || fabs(o[2] - (tt.my_org[2] - 24)) > 64 || (i == tt.near_ban && now < tt.near_ban_until))
			continue;
		cname = sv_string(prog, ed, prog->fieldoffsets.classname);
		if (strncmp(cname, "weapon_", 7) && strncmp(cname, "item_", 5))
			continue;
		v = fight_item_value(prog, me, cname, health, armor);
		if (v <= 0)
			continue;
		// the way there: the player's box fits, and safe ground at the item
		VectorCopy(tt.my_org, q);
		q[2] += -24 + 20;
		VectorCopy(o, dir);
		dir[2] = q[2];
		tr = CL_TraceBox(q, mins, maxs, dir, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
			collision_extendmovelength.value, true, false, NULL, false);
		if (tr.startsolid || tr.fraction < 0.9f)
			continue;
		VectorCopy(o, q);
		q[2] += 20;
		if (!safe_ground(q))
			continue;
		if (seen_from(o, 0) > seen_now)
			continue;
		v /= 1 + d / 200;
		if (i == tt.near_item)
			v *= 1.3f;
		if (v > bs)
		{
			bs = v;
			best = i;
			VectorCopy(o, bo);
		}
	}
	if (best != tt.near_item)
	{
		tt.near_item = best;
		tt.near_since = now;
		if (best)
		{
			tt.detours++;
			if (cl_touch_aim_debug.integer)
				Con_Printf("touch tactics: detour for %s, %.0f units\n", sv_string(prog, PRVM_EDICT_NUM(best), prog->fieldoffsets.classname), VectorDistance(bo, tt.my_org));
		}
	}
	VectorCopy(bo, tt.near_org);
}

// Blend a detour to the pickup close by into a fight's move; given up after
// 1.5 s (and that pickup left for 10).
static void detour(double now, vec3_t want)
{
	vec3_t to;
	if (!cl_touch_practice_detour.integer || !tt.near_item)
		return;
	if (now - tt.near_since > 1.5)
	{
		tt.near_ban = tt.near_item;
		tt.near_ban_until = now + 10;
		tt.near_item = 0;
		return;
	}
	VectorSubtract(tt.near_org, tt.my_org, to);
	to[2] = 0;
	VectorNormalize(to);
	VectorMA(want, 1.2f, to, want);
	want[2] = 0;
	VectorNormalize(want);
}

// The best spot near the player to fight focus f from, wanting range r:
// 16 standable points (two rings) scored by sight of the focus, cover from
// the others, range, height, the pickup being gone for, and room round it.
// Cached SPOT_CACHE.
#define SPOT_CACHE 0.3
static void find_spot(double now, const intel_t *f, float r)
{
	static const vec3_t mins = { -16, -16, 0 }, maxs = { 16, 16, 44 };
	vec3_t start, p, eye;
	float bs, sc;
	int k, j;
	tt.spot_next = now + SPOT_CACHE;
	tt.have_spot = false;
	VectorCopy(tt.my_org, start);
	start[2] += -24 + 20;
	// where the player stands counts too: moving must beat it
	bs = -1e9f;
	for (k = -1; k < 16; k++)
	{
		if (k < 0)
			VectorCopy(start, p);
		else
		{
			float a = (k & 7) * (float)(M_PI / 4) + (k >= 8 ? (float)(M_PI / 8) : 0), d = k < 8 ? 96 : 192;
			vec3_t end;
			trace_t tr;
			VectorSet(end, start[0] + d * cos(a), start[1] + d * sin(a), start[2]);
			tr = CL_TraceBox(start, mins, maxs, end, MOVE_NOMONSTERS, NULL, SUPERCONTENTS_SOLID | SUPERCONTENTS_PLAYERCLIP, 0, 0,
				collision_extendmovelength.value, true, false, NULL, false);
			if (tr.startsolid || tr.fraction < 0.95f)
				continue;
			VectorCopy(tr.endpos, p);
			if (!safe_ground(p))
				continue;
		}
		VectorCopy(p, eye);
		eye[2] = p[2] - 20 + 24 + (tt.my_eye[2] - tt.my_org[2]);
		sc = 0;
		// sight of the focus: the fight goes on from there
		sc += line_clear(eye, f->center) || line_clear(eye, f->eye) ? 2 : -2;
		// the others' sight
		sc -= 1.5f * seen_from(eye, tt.focus);
		// range
		sc -= fabs(VectorDistance(eye, f->center) - r) / max(r, 200.0f);
		// height over the focus, a little
		sc += 0.5f * bound(-1.0f, (p[2] - (f->org[2] - 4)) / 128, 1.0f);
		// toward the pickup the detour goes for
		if (tt.near_item)
			sc -= 0.5f * VectorDistance(p, tt.near_org) / 400;
		// a ledge or a corner: room round it
		for (j = 0; j < 4; j++)
		{
			vec3_t q;
			VectorSet(q, p[0] + 64 * cos(j * M_PI / 2), p[1] + 64 * sin(j * M_PI / 2), p[2]);
			if (!safe_ground(q))
				sc -= 0.75f;
		}
		if (k < 0)
			sc += 0.3f; // staying costs nothing
		if (sc > bs)
		{
			bs = sc;
			VectorCopy(p, tt.spot);
			tt.have_spot = k >= 0;
		}
	}
}

static void flat_norm(vec3_t v)
{
	float l;
	v[2] = 0;
	l = sqrt(v[0] * v[0] + v[1] * v[1]);
	if (l > 0.001f)
		VectorScale(v, 1 / l, v);
}

// The direction the player wants to go this frame, and why. Returns false
// when there is nothing to do.
// Freeze Tag: toward the nearest frozen mate in sight, stopping by it (the
// thaw runs while the player stands within about 100 units).
static qbool revive_target(vec3_t want)
{
	int n, best = 0;
	float bd = 2000;
	for (n = 1; n <= tt.maxn; n++)
	{
		const intel_t *t = &tt.e[n];
		// in a clear line on about the same level: without route finding a
		// mate behind a wall only gets the player stuck against it
		if (t->live && !t->enemy && t->frozen && t->dist < bd && fabs(t->org[2] - tt.my_org[2]) < 120
			&& line_clear(tt.my_eye, t->center))
		{
			bd = t->dist;
			best = n;
		}
	}
	if (!best)
		return false;
	VectorSubtract(tt.e[best].org, tt.my_org, want);
	want[2] = 0;
	if (VectorLength(want) < 64)
		VectorClear(want); // there: stay while the thaw runs
	else
		VectorNormalize(want);
	tt.mode = "revive";
	return true;
}

// ---------------------------------------------------------------------------
// Routes over the bots' waypoint graph (local server)
// ---------------------------------------------------------------------------
// Xonotic's bots navigate by waypoint entities (classname "waypoint"), each
// with up to 32 outgoing links (.wp00-.wp31) and their costs
// (.wp00mincost-...). The graph is read once per map; a route is the
// cheapest chain of links from the waypoint nearest the player (in clear
// view) to the one nearest the goal (in clear view of it), and the move
// heads for the next waypoint on it.
#define WP_MAX 2048
#define WP_LINKS 32
static struct
{
	int n;
	vec3_t org[WP_MAX];
	short link[WP_MAX][WP_LINKS];
	float cost[WP_MAX][WP_LINKS];
	unsigned char nl[WP_MAX];
	const void *defs;
	int edicts;
	double checked;
	// the route being followed
	short path[WP_MAX];
	int plen, pnext;
	vec3_t goal;
	double planned, node_since;
} wpg;

static void wp_build(prvm_prog_t *prog)
{
	static short idx[MAX_EDICTS];
	int ofs_link[WP_LINKS], ofs_cost[WP_LINKS], i, k, isf;
	char name[16];
	wpg.n = 0;
	wpg.plen = 0;
	for (k = 0; k < WP_LINKS; k++)
	{
		dpsnprintf(name, sizeof(name), "wp%02d", k);
		ofs_link[k] = PRVM_ED_FindFieldOffset(prog, name);
		dpsnprintf(name, sizeof(name), "wp%02dmincost", k);
		ofs_cost[k] = field_ofs(prog, name, &isf);
		if (ofs_link[k] < 0)
			return;
	}
	for (i = 0; i < prog->num_edicts && i < MAX_EDICTS; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		idx[i] = -1;
		if (ed->free || wpg.n >= WP_MAX || strcmp(sv_string(prog, ed, prog->fieldoffsets.classname), "waypoint"))
			continue;
		idx[i] = (short)wpg.n;
		VectorAdd(PRVM_serveredictvector(ed, absmin), PRVM_serveredictvector(ed, absmax), wpg.org[wpg.n]);
		VectorScale(wpg.org[wpg.n], 0.5f, wpg.org[wpg.n]);
		wpg.n++;
	}
	for (i = 0; i < prog->num_edicts && i < MAX_EDICTS; i++)
	{
		prvm_edict_t *ed;
		int w = idx[i];
		if (w < 0)
			continue;
		ed = PRVM_EDICT_NUM(i);
		wpg.nl[w] = 0;
		for (k = 0; k < WP_LINKS; k++)
		{
			int to = PRVM_EDICTFIELDVALUE(ed, ofs_link[k])->edict;
			if (to <= 0 || to >= prog->num_edicts || to >= MAX_EDICTS || idx[to] < 0)
				continue;
			wpg.link[w][wpg.nl[w]] = idx[to];
			wpg.cost[w][wpg.nl[w]] = ofs_cost[k] >= 0 && ed->fields.fp[ofs_cost[k]] > 0 ? ed->fields.fp[ofs_cost[k]]
				: VectorDistance(wpg.org[w], wpg.org[idx[to]]) / 360; // seconds at run speed, as the bots count
			wpg.nl[w]++;
		}
	}
}

// The graph for the map now loaded (rebuilt when the progs or edict count change).
static void wp_check(prvm_prog_t *prog, double now)
{
	if (wpg.defs == (const void *)prog->fielddefs && wpg.edicts == prog->num_edicts && (wpg.n || now < wpg.checked))
		return;
	wpg.defs = prog->fielddefs;
	wpg.edicts = prog->num_edicts;
	wpg.checked = now + 5;
	wp_build(prog);
}

// The waypoint nearest p that p sees (of the 8 nearest), -1 none.
static int wp_near(const vec3_t p)
{
	int best[8], i, k, n = 0;
	float bd[8];
	for (i = 0; i < wpg.n; i++)
	{
		float d = VectorDistance(p, wpg.org[i]);
		if (n < 8 || d < bd[n - 1])
		{
			k = n < 8 ? n++ : 7;
			while (k > 0 && bd[k - 1] > d)
			{
				bd[k] = bd[k - 1];
				best[k] = best[k - 1];
				k--;
			}
			bd[k] = d;
			best[k] = i;
		}
	}
	for (k = 0; k < n; k++)
		if (line_clear(p, wpg.org[best[k]]))
			return best[k];
	return -1;
}

// Cheapest route from waypoint a to b (Dijkstra, binary heap) into wpg.path.
static qbool wp_route(int a, int b)
{
	static float dist[WP_MAX];
	static short prev[WP_MAX], heap[WP_MAX * 4];
	static unsigned char done[WP_MAX];
	int hn = 0, i, u, n;
	for (i = 0; i < wpg.n; i++)
	{
		dist[i] = 1e30f;
		prev[i] = -1;
		done[i] = 0;
	}
	dist[a] = 0;
	heap[hn++] = (short)a;
	while (hn)
	{
		// pop the nearest
		int c = 0, j;
		u = heap[0];
		heap[0] = heap[--hn];
		for (;;)
		{
			int l = 2 * c + 1, r = l + 1, m = c;
			if (l < hn && dist[heap[l]] < dist[heap[m]]) m = l;
			if (r < hn && dist[heap[r]] < dist[heap[m]]) m = r;
			if (m == c) break;
			j = heap[c]; heap[c] = heap[m]; heap[m] = (short)j; c = m;
		}
		if (done[u])
			continue;
		done[u] = 1;
		if (u == b)
			break;
		for (i = 0; i < wpg.nl[u]; i++)
		{
			int v = wpg.link[u][i];
			float nd = dist[u] + wpg.cost[u][i];
			if (nd < dist[v] && hn < WP_MAX * 4)
			{
				dist[v] = nd;
				prev[v] = (short)u;
				// push
				c = hn;
				heap[hn++] = (short)v;
				while (c > 0 && dist[heap[(c - 1) / 2]] > dist[heap[c]])
				{
					j = heap[c]; heap[c] = heap[(c - 1) / 2]; heap[(c - 1) / 2] = (short)j; c = (c - 1) / 2;
				}
			}
		}
	}
	if (!done[b])
		return false;
	for (n = 0, u = b; u >= 0 && n < WP_MAX; u = prev[u])
		n++;
	wpg.plen = n;
	for (u = b; u >= 0 && n > 0; u = prev[u])
		wpg.path[--n] = (short)u;
	wpg.pnext = 0;
	return true;
}

// Toward goal along the waypoint route (replanned every second, when the
// goal moves, or when a waypoint is not reached in 4 s); straight when the
// goal is in clear view or there is no route. False with no way at all.
static qbool route_to(double now, const vec3_t goal, vec3_t want)
{
	vec3_t eye;
	VectorCopy(goal, eye);
	eye[2] += 16;
	if (!wpg.n || line_clear(tt.my_eye, eye))
	{
		VectorSubtract(goal, tt.my_org, want);
		flat_norm(want);
		return true;
	}
	if (now - wpg.planned > 1 || VectorDistance(goal, wpg.goal) > 200 || now - wpg.node_since > 4 || wpg.pnext >= wpg.plen)
	{
		int a = wp_near(tt.my_eye), b = wp_near(eye);
		wpg.plen = 0;
		wpg.planned = now;
		wpg.node_since = now;
		VectorCopy(goal, wpg.goal);
		if (a < 0 || b < 0 || !wp_route(a, b))
		{
			VectorSubtract(goal, tt.my_org, want);
			flat_norm(want);
			return true;
		}
	}
	// at the next waypoint, or the one after it in clear view: on
	while (wpg.pnext < wpg.plen)
	{
		const float *w = wpg.org[wpg.path[wpg.pnext]];
		float dxy = sqrt((w[0] - tt.my_org[0]) * (w[0] - tt.my_org[0]) + (w[1] - tt.my_org[1]) * (w[1] - tt.my_org[1]));
		qbool reached = dxy < 48 && fabs(w[2] - tt.my_org[2]) < 72;
		qbool skip = wpg.pnext + 1 < wpg.plen && VectorDistance(wpg.org[wpg.path[wpg.pnext + 1]], tt.my_org) < 500
			&& line_clear(tt.my_eye, wpg.org[wpg.path[wpg.pnext + 1]]) && fabs(wpg.org[wpg.path[wpg.pnext + 1]][2] - tt.my_org[2]) < 72;
		if (!reached && !skip)
			break;
		wpg.pnext++;
		wpg.node_since = now;
	}
	if (wpg.pnext >= wpg.plen)
		VectorSubtract(goal, tt.my_org, want);
	else
		VectorSubtract(wpg.org[wpg.path[wpg.pnext]], tt.my_org, want);
	flat_norm(want);
	return true;
}

// Toward p (flat), or held still within `there` units.
static void go_to(const vec3_t p, float there, vec3_t want)
{
	VectorSubtract(p, tt.my_org, want);
	want[2] = 0;
	if (VectorLength(want) < there)
		VectorClear(want);
	else
		route_to(host.realtime, p, want);
}

// CTF: carry the enemy flag home, return the player's own, fetch the enemy's.
static qbool objective(double now, const intel_t *f, vec3_t want, qbool *cover)
{
	const flag_t *mf, *ef;
	if (!cl_touch_practice_objectives.integer || tt.my_flag < 0 || tt.enemy_flag < 0)
		return false;
	mf = &tt.flags[tt.my_flag];
	ef = &tt.flags[tt.enemy_flag];
	if (ef->carrier == tt.me)
	{
		// carrying: home, out of sight, health on the way. The player's own
		// flag must be home to score: near the base, wait for it (or fight
		// the one carrying it, once in sight)
		if (mf->status != FLAG_AT_BASE && VectorDistance(tt.my_org, mf->base) < 300)
		{
			if (mf->carrier && tt.e[mf->carrier].seen)
				return false;
			if (mf->status == FLAG_DROPPED && VectorDistance(tt.my_org, mf->pos) < 1500)
				go_to(mf->pos, 0, want);
			else
				go_to(mf->base, 64, want);
		}
		else
			go_to(mf->base, 0, want);
		*cover = true;
		tt.cover_skip = 0;
		detour(now, want);
		tt.mode = "capture";
		return true;
	}
	if (mf->status == FLAG_DROPPED && VectorDistance(tt.my_org, mf->pos) < 2500
		&& !(f && f->seen && VectorDistance(tt.my_org, mf->pos) > 600))
	{
		go_to(mf->pos, 0, want);
		tt.mode = "return";
		return true;
	}
	if ((ef->status == FLAG_AT_BASE || ef->status == FLAG_DROPPED)
		&& (!f || !f->seen || VectorDistance(tt.my_org, ef->pos) < 600))
	{
		go_to(ef->pos, 0, want);
		detour(now, want);
		tt.mode = "steal";
		return true;
	}
	return false;
}

static qbool plan(double now, qbool pilot, qbool dodge, vec3_t want, qbool *cover)
{
	const intel_t *f = tt.focus ? &tt.e[tt.focus] : NULL;
	int n, threat = 0;
	float tbest = 1e9f;
	*cover = false;
	tt.cover_skip = 0;
	VectorClear(want);

	// someone has the player in their sights: step across their line
	if (dodge)
		for (n = 1; n <= tt.maxn; n++)
			if (tt.e[n].live && tt.e[n].enemy && tt.e[n].aiming && tt.e[n].dist < tbest)
			{
				tbest = tt.e[n].dist;
				threat = n;
			}
	if (threat)
	{
		if (now >= tt.dodge_until || tt.dodge_from != threat)
		{
			vec3_t across, side;
			qbool g1, g2, j1, j2;
			float r1, r2;
			VectorSet(across, -tt.e[threat].aimdir[1], tt.e[threat].aimdir[0], 0);
			flat_norm(across);
			r1 = room(across, &g1, &j1);
			VectorNegate(across, side);
			r2 = room(side, &g2, &j2);
			r1 += g1 ? 0 : -1000;
			r2 += g2 ? 0 : -1000;
			// both open: either, at random, so the next lead is wrong
			if (fabs(r1 - r2) < 48 ? lhrandom(0, 1) < 0.5 : r2 > r1)
				VectorCopy(side, across);
			VectorCopy(across, tt.dodge_dir);
			tt.dodge_until = now + lhrandom(0.25, 0.55);
			tt.dodge_from = threat;
			tt.dodges++;
		}
		VectorCopy(tt.dodge_dir, want);
		// still close the range with a weapon that wants it
		if (pilot && f && f->seen)
		{
			vec3_t to;
			float r = best_range(tt.e[tt.me].weapon);
			VectorSubtract(f->center, tt.my_center, to);
			flat_norm(to);
			VectorMA(want, 0.4f * bound(-1.0f, (f->dist - r) / max(r, 200.0f), 1.0f), to, want);
		}
		detour(now, want);
		tt.mode = "dodge";
		return true;
	}
	tt.dodge_from = 0;
	if (!pilot)
		return false;
	if (objective(now, f, want, cover))
		return true;
	if (!f)
	{
		if (revive_target(want))
			return true;
		if (!tt.item)
			return false;
		VectorSubtract(tt.item_org, tt.my_org, want);
		flat_norm(want);
		tt.mode = "collect";
		return true;
	}

	if (tt.num_aiming >= 2 || tt.num_seen >= 3 || (tt.my_eff < 70 && f->seen && f->eff > tt.my_eff * 1.5f)
		// one more hit from it kills, and it would take more than that to kill it
		|| (cl_touch_practice_threat.integer && f->seen && f->aiming && f->dmg >= tt.my_eff && f->eff > tt.my_eff))
	{
		// outnumbered or losing: away from them, into cover
		for (n = 1; n <= tt.maxn; n++)
			if (tt.e[n].live && tt.e[n].enemy && tt.e[n].seen)
			{
				vec3_t away;
				VectorSubtract(tt.my_org, tt.e[n].org, away);
				flat_norm(away);
				VectorAdd(want, away, want);
			}
		flat_norm(want);
		// health or armour on the way out
		if (tt.item)
		{
			vec3_t to;
			VectorSubtract(tt.item_org, tt.my_org, to);
			flat_norm(to);
			if (DotProduct(to, want) > -0.3f)
			{
				VectorAdd(want, to, want);
				flat_norm(want);
			}
		}
		*cover = true;
		// team games: back toward the nearest mate rather than off alone
		if (tt.team_game && cl_touch_practice_objectives.integer)
		{
			int m, bm = 0;
			float bd = 1e9f;
			for (m = 1; m <= tt.maxn; m++)
				if (tt.e[m].live && !tt.e[m].enemy && !tt.e[m].frozen && tt.e[m].dist < bd)
				{
					bd = tt.e[m].dist;
					bm = m;
				}
			if (bm && bd > 300)
			{
				vec3_t to;
				VectorSubtract(tt.e[bm].org, tt.my_org, to);
				flat_norm(to);
				VectorMA(want, 0.8f, to, want);
				flat_norm(want);
			}
		}
		detour(now, want);
		tt.mode = "cover";
		return true;
	}
	if (f->seen)
	{
		vec3_t to, perp;
		float my_r = best_range(tt.e[tt.me].weapon), r = my_r, radial;
		const char *mode = "fight";
		qbool hide = false;
		if (f->eff < 50)
		{
			r *= 0.5f; // nearly dead: close in and finish it
			mode = "finish";
		}
		else if (cl_touch_practice_threat.integer && f->kind != TW_KIND_NONE)
		{
			float their_r = best_range(f->weapon);
			if (their_r < 0.75f * my_r)
			{
				// a short-range weapon this one outranges: stay out of its reach
				r = max(my_r, their_r + 300);
				mode = "kite";
			}
			else if (their_r > 1.4f * my_r)
			{
				if (tt.my_eff >= f->eff)
				{
					// it wins at range and this side is stronger: close in
					r = 0.7f * my_r;
					mode = "rush";
				}
				else
				{
					// it wins at range and this side is weaker: out of its sight
					hide = true;
					mode = "break";
				}
			}
		}
		VectorSubtract(f->center, tt.my_center, to);
		flat_norm(to);
		radial = bound(-1.0f, (f->dist - r) / max(r, 200.0f), 1.0f);
		if (now >= tt.strafe_until)
		{
			tt.strafe_sign = lhrandom(0, 1) < 0.5 ? -1 : 1;
			tt.strafe_until = now + lhrandom(0.5, 1.3);
		}
		VectorSet(perp, -to[1] * tt.strafe_sign, to[0] * tt.strafe_sign, 0);
		VectorScale(to, radial, want);
		VectorMA(want, 0.9f, perp, want);
		// toward the best spot to fight from (map shape), then the strafe
		if (cl_touch_practice_position.integer)
		{
			if (now >= tt.spot_next)
				find_spot(now, f, r);
			if (tt.have_spot && VectorDistance(tt.spot, tt.my_org) > 40)
			{
				vec3_t sp;
				VectorSubtract(tt.spot, tt.my_org, sp);
				flat_norm(sp);
				VectorMA(want, 1.2f, sp, want);
			}
		}
		flat_norm(want);
		detour(now, want);
		tt.mode = mode;
		// the others in sight: fight from where they cannot see
		*cover = cl_touch_practice_exposure.integer && tt.num_seen >= 2;
		tt.cover_skip = tt.focus;
		if (hide)
		{
			*cover = true;
			tt.cover_skip = 0; // its sight too
		}
		return true;
	}
	// nobody in sight: a frozen mate to thaw (Freeze Tag: stand by it), a
	// pickup worth having, else toward the focus (straight; the room check
	// goes round what is in the way)
	if (revive_target(want))
		return true;
	if (tt.item)
	{
		VectorSubtract(tt.item_org, tt.my_org, want);
		flat_norm(want);
		tt.mode = "collect";
		return true;
	}
	VectorSubtract(f->org, tt.my_org, want);
	flat_norm(want);
	tt.mode = "hunt";
	return true;
}

static void steer(double now)
{
	float own_f = cl.cmd.forwardmove / max(1.0f, cl_forwardspeed.value), own_s = cl.cmd.sidemove / max(1.0f, cl_sidespeed.value);
	qbool idle = own_f * own_f + own_s * own_s < 0.01f, pilot = cl_touch_practice_pilot.integer != 0, cover;
	vec3_t want, ang, fwd, right;
	float speed;

	tt.buttons = 0;
	tt.mode = NULL;
	if (pilot && now >= tt.items_next)
	{
		pick_item(now);
		tt.items_next = now + 0.5;
	}
	if (pilot)
		item_progress(now);
	if (pilot && cl_touch_practice_detour.integer && now >= tt.near_next)
	{
		if (tt.focus && tt.e[tt.focus].seen)
			pick_near(now);
		else
			tt.near_item = 0;
		tt.near_next = now + 0.3;
	}
	if (!idle || TouchHook_InUse() || TouchHook_Rescuing()
		|| !plan(now, pilot, cl_touch_practice_aimdodge.integer != 0, want, &cover))
	{
		tt.steering = false;
		tt.slow_since = 0;
		return;
	}

	if (VectorLength2(want) == 0)
	{
		// arrived (by a frozen mate): hold still, which is no unstuck case
		tt.steering = false;
		tt.slow_since = 0;
		if (cl_touch_aim_debug.integer && tt.mode != tt.mode_said)
			Con_Printf("touch tactics: %s (holding)\n", tt.mode);
		tt.mode_said = tt.mode;
		return;
	}

	// stuck against something the traces missed: somewhere else for a moment
	speed = sqrt(tt.my_vel[0] * tt.my_vel[0] + tt.my_vel[1] * tt.my_vel[1]);
	if (tt.steering && speed < 40)
	{
		if (!tt.slow_since)
			tt.slow_since = now;
		else if (now - tt.slow_since > 0.6 && now >= tt.stuck_until)
		{
			float a = lhrandom(0, 2 * M_PI);
			VectorSet(tt.stuck_dir, cos(a), sin(a), 0);
			tt.stuck_until = now + 0.6;
			tt.slow_since = 0;
			tt.stucks++;
		}
	}
	else
		tt.slow_since = 0;
	if (now < tt.stuck_until)
	{
		VectorCopy(tt.stuck_dir, want);
		tt.mode = "unstuck";
	}

	if (now >= tt.eval_next || !tt.steering)
	{
		choose(want, cover, tt.cover_skip);
		tt.eval_next = now + TT_EVAL;
	}
	tt.steering = true;

	VectorSet(ang, 0, cl.viewangles[YAW], 0);
	AngleVectors(ang, fwd, right, NULL);
	cl.cmd.forwardmove = DotProduct(tt.move_dir, fwd) * cl_forwardspeed.value;
	cl.cmd.sidemove = DotProduct(tt.move_dir, right) * cl_sidespeed.value;
	if (tt.jump_dir && cl.onground)
		tt.buttons |= 2;

	if (cl_touch_aim_debug.integer && tt.mode != tt.mode_said)
		Con_Printf("touch tactics: %s (focus %d, %d in sight, %d aiming)\n", tt.mode, tt.focus, tt.num_seen, tt.num_aiming);
	tt.mode_said = tt.mode;
}

// Pickup magnet (cl_touch_practice_magnet, offline only, behind the gate
// like the rest): a pickup within the radius and in clear view is touched by
// the player as the server's own touch would (SV_LinkEdict_TouchAreaGrid_Call).
// Only the player's own edict; bots never.
static void magnet(void)
{
	prvm_prog_t *prog = SVVM_prog;
	prvm_edict_t *me = PRVM_EDICT_NUM(tt.me);
	float rad = bound(0.0f, cl_touch_practice_magnet_radius.value, 512.0f);
	int i;
	if (!sv.active || PRVM_serveredictfloat(me, health) <= 0 || rad <= 0)
		return;
	for (i = svs.maxclients + 1; i < prog->num_edicts; i++)
	{
		prvm_edict_t *ed = PRVM_EDICT_NUM(i);
		const char *cname;
		vec3_t o;
		int old_self, old_other;
		if (ed->free || PRVM_serveredictfloat(ed, solid) != SOLID_TRIGGER || !PRVM_serveredictfunction(ed, touch))
			continue;
		VectorCopy(PRVM_serveredictvector(ed, origin), o);
		if (VectorDistance(o, tt.my_org) > rad)
			continue;
		cname = sv_string(prog, ed, prog->fieldoffsets.classname);
		if (strncmp(cname, "weapon_", 7) && strncmp(cname, "item_", 5))
			continue;
		o[2] += 16;
		if (!line_clear(tt.my_eye, o))
			continue;
		old_self = PRVM_serverglobaledict(self);
		old_other = PRVM_serverglobaledict(other);
		SV_LinkEdict_TouchAreaGrid_Call(ed, me);
		PRVM_serverglobaledict(self) = old_self;
		PRVM_serverglobaledict(other) = old_other;
		if (ed->free || PRVM_serveredictfloat(ed, solid) != SOLID_TRIGGER)
			tt.magnet_pickups++;
	}
}

void TouchTactics_Frame(void)
{
	double now = host.realtime;
	const char *why = TouchAim_BlockReason();
	int g = gate();
	qbool open = !why && g == TW_GATE_OPEN;
	if (cl_touch_aim_debug.integer && g != tt.gate_said)
		Con_Printf("touch tactics: %s%s\n", g == TW_GATE_OPEN ? "on: " : "off: ", gate_why(g));
	tt.gate_said = g;
	tt.open = open;
	tt.buttons = 0;
	if (!open)
	{
		// nothing outlives the frame that allowed it
		tt.focus = 0;
		tt.item = 0;
		tt.steering = false;
		tt.mode_said = NULL;
		tt.maxn = 0;
		return;
	}
	read_intel(now);
	if (!tt.me)
	{
		tt.open = false;
		return;
	}
	steer(now);
	if (cl_touch_practice_magnet.integer)
		magnet();
}

int TouchTactics_Buttons(void)
{
	return tt.open ? tt.buttons : 0;
}

void TouchTactics_Draw(void)
{
	vec3_t org, fwd, left, up, d;
	float w = vid_conwidth.value, h = vid_conheight.value, fx = r_refdef.view.frustum_x, fy = r_refdef.view.frustum_y;
	int n;
	if (!tt.open || !cl_touch_practice_intel.integer || gate() != TW_GATE_OPEN || fx <= 0 || fy <= 0)
		return;
	Matrix4x4_ToVectors(&r_refdef.view.matrix, fwd, left, up, org);
	for (n = 1; n <= tt.maxn; n++)
	{
		const intel_t *t = &tt.e[n];
		float df, x, y, cr, cg, cb, bw = 40, frac;
		vec3_t p;
		char txt[64];
		if (!t->live || !t->enemy)
			continue;
		VectorCopy(t->org, p);
		p[2] += 52;
		VectorSubtract(p, org, d);
		df = DotProduct(d, fwd);
		if (df <= 1)
			continue;
		x = 0.5f * w * (1 - DotProduct(d, left) / (df * fx));
		y = 0.5f * h * (1 - DotProduct(d, up) / (df * fy));
		if (x < 0 || x > w || y < 0 || y > h)
			continue;
		if (t->aiming)
			cr = 1, cg = 0.2f, cb = 0.2f;
		else if (!t->seen)
			cr = 1, cg = 0.6f, cb = 0.2f;
		else
			cr = 1, cg = 1, cb = 1;
		// health bar (green to red) with armour on top in blue
		frac = bound(0.0f, t->health / 100, 2.0f) / 2;
		DrawQ_Fill(x - bw * 0.5f, y, bw, 4, 0, 0, 0, 0.6f, 0);
		DrawQ_Fill(x - bw * 0.5f, y, bw * frac, 4, 1 - frac, frac, 0.1f, 0.9f, 0);
		if (t->armor > 0)
			DrawQ_Fill(x - bw * 0.5f, y - 2, bw * bound(0.0f, t->armor / 200, 1.0f), 2, 0.3f, 0.5f, 1, 0.9f, 0);
		dpsnprintf(txt, sizeof(txt), "%.0f/%.0f %s%s%s", t->health, t->armor, t->weapon, t->aiming ? " !" : "", n == tt.focus ? " *" : "");
		DrawQ_String(x - bw * 0.5f, y + 5, txt, 0, 11, 11, cr, cg, cb, 0.95f, 0, NULL, true, FONT_CHAT);
	}
}

static void TouchTactics_Status_f(cmd_state_t *cmd)
{
	int g = gate(), n;
	Con_Printf("touch tactics: gate %s; intel %d, aim dodge %d, pilot %d; mode %s; dodges %d, unstuck %d, pickups fetched %d, fight detours %d, magnet pickups %d\n",
		gate_why(g), cl_touch_practice_intel.integer, cl_touch_practice_aimdodge.integer, cl_touch_practice_pilot.integer,
		tt.mode ? tt.mode : "idle", tt.dodges, tt.stucks, tt.pickups_tried, tt.detours, tt.magnet_pickups);
	if (!tt.open)
		return;
	if (tt.num_flags)
		Con_Printf("  waypoints %d; route at %d of %d\n", wpg.n, wpg.pnext, wpg.plen);
	for (n = 0; n < tt.num_flags; n++)
	{
		char vabuf[32];
		Con_Printf("  flag team %.0f%s: %s%s, base %.0f %.0f %.0f\n", tt.flags[n].team, n == tt.my_flag ? " (ours)" : "",
			tt.flags[n].status == FLAG_AT_BASE ? "home" : tt.flags[n].status == FLAG_DROPPED ? "dropped" : tt.flags[n].status == FLAG_CARRIED ? "carried" : "passing",
			tt.flags[n].carrier ? va(vabuf, sizeof(vabuf), " by %d", tt.flags[n].carrier) : "", tt.flags[n].base[0], tt.flags[n].base[1], tt.flags[n].base[2]);
	}
	Con_Printf("  me %d: %.0f to kill, weapon %s\n", tt.me, tt.my_eff, tt.e[tt.me].weapon);
	for (n = 1; n <= tt.maxn; n++)
	{
		const intel_t *t = &tt.e[n];
		if (!t->live)
			continue;
		Con_Printf("  %c%d %s%s: %.0f health %.0f armour (%.0f to kill), %s, %.0f units, %s%s\n", n == tt.focus ? '*' : ' ', n,
			t->enemy ? "enemy" : "mate", t->frozen ? " (frozen)" : "", t->health, t->armor, t->eff, t->weapon[0] ? t->weapon : "?", t->dist,
			t->seen ? "in sight" : "hidden", t->aiming ? ", aiming at you" : "");
	}
}

// Testing: what the ground checks see at a point (map surveys).
static void TouchTactics_Probe_f(cmd_state_t *cmd)
{
	vec3_t p;
	int z;
	if (Cmd_Argc(cmd) < 4 || !cl.worldmodel)
	{
		Con_Printf("usage: touch_tactics_probe x y z (in a map)\n");
		return;
	}
	p[0] = atof(Cmd_Argv(cmd, 1));
	p[1] = atof(Cmd_Argv(cmd, 2));
	p[2] = atof(Cmd_Argv(cmd, 3));
	Con_Printf("probe %.0f %.0f %.0f: safe ground %d; contents below:", p[0], p[1], p[2], safe_ground(p));
	for (z = 0; z <= 160; z += 16)
	{
		vec3_t q;
		VectorSet(q, p[0], p[1], p[2] - z);
		Con_Printf(" %d:%x", -z, CL_PointSuperContents(q));
	}
	Con_Printf("\n");
}

void TouchTactics_Init(void)
{
	Cmd_AddCommand(CF_CLIENT, "touch_tactics_probe", TouchTactics_Probe_f, "Testing: the autopilot's ground check and the point contents below x y z");
	Cvar_RegisterVariable(&cl_touch_esp_enabled);
	Cvar_RegisterVariable(&cl_touch_practice_intel);
	Cvar_RegisterVariable(&cl_touch_practice_aimdodge);
	Cvar_RegisterVariable(&cl_touch_practice_pilot);
	Cvar_RegisterVariable(&cl_touch_practice_exact);
	Cvar_RegisterVariable(&cl_touch_practice_exposure);
	Cvar_RegisterVariable(&cl_touch_practice_threat);
	Cvar_RegisterVariable(&cl_touch_practice_detour);
	Cvar_RegisterVariable(&cl_touch_practice_objectives);
	Cvar_RegisterVariable(&cl_touch_practice_position);
	Cvar_RegisterVariable(&cl_touch_practice_magnet);
	Cvar_RegisterVariable(&cl_touch_practice_magnet_radius);
	Cmd_AddCommand(CF_CLIENT, "touch_tactics_status", TouchTactics_Status_f, "Offline tactics: the gate, the modes, and every player's health, armour, weapon and aim");
	tt.gate_said = -1;
}
