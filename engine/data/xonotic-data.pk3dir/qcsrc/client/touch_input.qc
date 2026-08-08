#include "touch_input.qh"
#include "touch_customize.qh"
#include "touch_glass.qh"
#include "touch_hud.qh"
#include "touch_console.qh"
#include "touch_pause.qh"
#include "touch_look.qh"
#include "touch_layout.qh"
#include "touch_init.qh"
#include "touch_defs.qh"
#include "touch_weapons.qh"
#include <common/mapvoting/_mod.qh>
#include <client/hud/panel/scoreboard.qh>

bool touch_finger_used[TOUCH_MAX_FINGERS];
int touch_finger_src[TOUCH_MAX_FINGERS];
int touch_finger_role[TOUCH_MAX_FINGERS];
vector touch_finger_down_pos[TOUCH_MAX_FINGERS];
vector touch_finger_last_pos[TOUCH_MAX_FINGERS];
vector touch_finger_prev_pos[TOUCH_MAX_FINGERS];
vector touch_finger_accum[TOUCH_MAX_FINGERS];
float touch_finger_down_time[TOUCH_MAX_FINGERS];
float touch_finger_lost_time[TOUCH_MAX_FINGERS];
bool touch_finger_first[TOUCH_MAX_FINGERS];
bool touch_finger_live[TOUCH_MAX_FINGERS];
// Set once a FIRE finger has slid far enough to also steer the view, so the
// promotion is sticky for the rest of the gesture.
bool touch_finger_draglook[TOUCH_MAX_FINGERS];

vector touch_move_offset;
bool touch_fire_down;
bool touch_fire_edge;
bool touch_fire_toggle_state;
bool touch_zoom_edge;
bool touch_zoom_toggle_state;
bool touch_jump_edge;
bool touch_hop_latched;
float touch_hop_press_time;
bool touch_hop_cancelling;
int touch_last_hop_mode = -1;
// A tap in the look zone fires one shot; hold attack briefly so a single frame's
// press cannot be swallowed between +attack and -attack.
float touch_tapfire_until;
bool touch_weapon_wheel;
bool touch_mouse_down;
int touch_active_finger_count;
bool touch_ready;
int touch_last_fire_mode;
int touch_last_mobile_hud;
bool touch_autoscreenshot_done;
int touch_autoscreenshot_frames;

bool touch_key_forward;
bool touch_key_back;
bool touch_key_left;
bool touch_key_right;
bool touch_key_attack;
bool touch_key_jump;
bool touch_key_crouch;
bool touch_key_zoom;
bool touch_owns_forward;
bool touch_owns_back;
bool touch_owns_left;
bool touch_owns_right;
bool touch_owns_attack;
bool touch_owns_jump;
bool touch_owns_crouch;
bool touch_owns_zoom;

int touch_debug_fills;
int touch_debug_cmds;
float touch_debug_cmd_window;
int touch_debug_cmds_per_sec;

bool Touch_MouseDown(void)
{
	return touch_mouse_down;
}

void Touch_SetMouseDown(bool down)
{
	touch_mouse_down = down;
}

bool Touch_Active(void)
{
	if (Touch_CustomizeActive() || Touch_Chat_Active())
		return true;
	if (!cvar("vid_touchscreen"))
		return false;
	// TOUCH_LAYOUT_SPEC R7: hard-release during vote / intermission / team UI so
	// MapVote and scoreboard receive pointer events (Touch_InputEvent runs first).
	if (mv_active || intermission || scoreboard_ui_enabled)
		return false;
	// Normal: hide under full menu. Debug keeps overlay visible for remote verification.
	if (autocvar_touch_debug)
		return true;
	return autocvar__menu_alpha < 1;
}

void Touch_SetKey(string key, bool down)
{
	if (key == "forward")
	{
		if (down) { if (!touch_key_forward) { localcmd("+forward\n"); touch_key_forward = true; touch_owns_forward = true; touch_debug_cmds += 1; } }
		else if (touch_key_forward && touch_owns_forward) { localcmd("-forward\n"); touch_key_forward = false; touch_owns_forward = false; touch_debug_cmds += 1; }
	}
	else if (key == "back")
	{
		if (down) { if (!touch_key_back) { localcmd("+back\n"); touch_key_back = true; touch_owns_back = true; touch_debug_cmds += 1; } }
		else if (touch_key_back && touch_owns_back) { localcmd("-back\n"); touch_key_back = false; touch_owns_back = false; touch_debug_cmds += 1; }
	}
	else if (key == "moveleft")
	{
		if (down) { if (!touch_key_left) { localcmd("+moveleft\n"); touch_key_left = true; touch_owns_left = true; touch_debug_cmds += 1; } }
		else if (touch_key_left && touch_owns_left) { localcmd("-moveleft\n"); touch_key_left = false; touch_owns_left = false; touch_debug_cmds += 1; }
	}
	else if (key == "moveright")
	{
		if (down) { if (!touch_key_right) { localcmd("+moveright\n"); touch_key_right = true; touch_owns_right = true; touch_debug_cmds += 1; } }
		else if (touch_key_right && touch_owns_right) { localcmd("-moveright\n"); touch_key_right = false; touch_owns_right = false; touch_debug_cmds += 1; }
	}
	else if (key == "attack")
	{
		if (down) { if (!touch_key_attack) { localcmd("+attack\n"); touch_key_attack = true; touch_owns_attack = true; touch_debug_cmds += 1; } }
		else if (touch_key_attack && touch_owns_attack) { localcmd("-attack\n"); touch_key_attack = false; touch_owns_attack = false; touch_debug_cmds += 1; }
	}
	else if (key == "jump")
	{
		if (down) { if (!touch_key_jump) { localcmd("+jump\n"); touch_key_jump = true; touch_owns_jump = true; touch_debug_cmds += 1; } }
		else if (touch_key_jump && touch_owns_jump) { localcmd("-jump\n"); touch_key_jump = false; touch_owns_jump = false; touch_debug_cmds += 1; }
	}
	else if (key == "crouch")
	{
		if (down) { if (!touch_key_crouch) { localcmd("+crouch\n"); touch_key_crouch = true; touch_owns_crouch = true; touch_debug_cmds += 1; } }
		else if (touch_key_crouch && touch_owns_crouch) { localcmd("-crouch\n"); touch_key_crouch = false; touch_owns_crouch = false; touch_debug_cmds += 1; }
	}
	else if (key == "zoom")
	{
		if (down) { if (!touch_key_zoom) { localcmd("+zoom\n"); touch_key_zoom = true; touch_owns_zoom = true; touch_debug_cmds += 1; } }
		else if (touch_key_zoom && touch_owns_zoom) { localcmd("-zoom\n"); touch_key_zoom = false; touch_owns_zoom = false; touch_debug_cmds += 1; }
	}
}

void Touch_ReleaseAll(void)
{
	int i;
	Touch_SetKey("forward", false);
	Touch_SetKey("back", false);
	Touch_SetKey("moveleft", false);
	Touch_SetKey("moveright", false);
	Touch_SetKey("attack", false);
	Touch_SetKey("jump", false);
	Touch_SetKey("crouch", false);
	Touch_SetKey("zoom", false);
	touch_fire_down = false;
	touch_fire_toggle_state = false;
	touch_zoom_toggle_state = false;
	touch_move_offset = '0 0 0';
	touch_mouse_down = false;
	touch_active_finger_count = 0;
	// Same reason as the hop latch below: the overlay is gone at intermission and
	// mapvote, so a held +showscores would have nothing left on screen to say it
	// was held and no pill to release it.
	Touch_Scores_Hide();
	// The hop latch must not survive into a menu or across a map change, or the
	// player would spawn already holding jump with nothing on screen saying so.
	touch_hop_latched = false;
	touch_jump_edge = false;
	touch_hop_cancelling = false;
	touch_tapfire_until = 0;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		touch_finger_used[i] = false;
		touch_finger_role[i] = TOUCH_ROLE_NONE;
		touch_finger_live[i] = false;
		touch_finger_lost_time[i] = 0;
		touch_finger_draglook[i] = false;
	}
	Touch_Console_Reset();
	Touch_Pause_Reset();
	Touch_Look_EndGesture();
	Touch_Look_SetOwnership(false);
}

vector Touch_SampleEngineFinger(int src)
{
	// Prefer gettouchfinger for POINTER (slot 10 = SDL mouse). getmousepos() can
	// return 0,0 on press while -attack still needs consuming (B2).
	if (!checkextension("DP_UT_TOUCHFINGER"))
		return '0 0 0';
	vector f = gettouchfinger(src);
	if (f.z <= 0)
		return '0 0 0';
	f = Touch_NormalizeInput(f);
	f.z = 1;
	return f;
}

int Touch_FindFingerBySrc(int src)
{
	int i;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
		if (touch_finger_used[i] && touch_finger_src[i] == src)
			return i;
	return -1;
}

bool Touch_RoleOwned(int role)
{
	int i;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
		if (touch_finger_used[i] && touch_finger_role[i] == role && touch_finger_live[i])
			return true;
	return false;
}

int Touch_AssignRole(vector p)
{
	if (Touch_Console_Hit(p))
		return TOUCH_ROLE_CON;

	if (Touch_Pause_PillHit(p))
		return TOUCH_ROLE_PAUSE;

	if (Touch_Chat_PillHit(p))
		return TOUCH_ROLE_CHAT;

	if (Touch_Scores_PillHit(p))
		return TOUCH_ROLE_SCORES;

	// Stock vertical weapons strip — tap icon to switch (replaces WEP button).
	if (Touch_Weapons_Hit(p))
		return TOUCH_ROLE_WEAPON;

	if (Touch_InEdgeDeadzone(p))
		return TOUCH_ROLE_IGNORED;

	if (autocvar_touch_fire_visible && Touch_HitCircle(p, autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size, 1.15))
		return TOUCH_ROLE_FIRE;
	if (autocvar_touch_fire2_visible && Touch_HitCircle(p, autocvar_touch_fire2_x, autocvar_touch_fire2_y, autocvar_touch_fire2_size, 1.15))
		return TOUCH_ROLE_FIRE2;
	if (autocvar_touch_jump_visible && Touch_HitRect(p, autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size, autocvar_touch_jump_aspect, 1.10))
		return TOUCH_ROLE_JUMP;
	if (autocvar_touch_crouch_visible && Touch_HitCircle(p, autocvar_touch_crouch_x, autocvar_touch_crouch_y, autocvar_touch_crouch_size, 1.10))
		return TOUCH_ROLE_CROUCH;
	if (autocvar_touch_weapon_visible && Touch_HitCircle(p, autocvar_touch_weapon_x, autocvar_touch_weapon_y, autocvar_touch_weapon_size, 1.10))
		return TOUCH_ROLE_WEAPON;
	if (autocvar_touch_zoom_visible && Touch_HitCircle(p, autocvar_touch_zoom_x, autocvar_touch_zoom_y, autocvar_touch_zoom_size, 1.10))
		return TOUCH_ROLE_ZOOM;
	if (autocvar_touch_reload_visible && Touch_HitCircle(p, autocvar_touch_reload_x, autocvar_touch_reload_y, autocvar_touch_reload_size, 1.10))
		return TOUCH_ROLE_RELOAD;
	if (autocvar_touch_dodge_visible && Touch_HitCircle(p, autocvar_touch_dodge_x, autocvar_touch_dodge_y, autocvar_touch_dodge_size, 1.10))
		return TOUCH_ROLE_DODGE;

	if (Touch_InMoveZone(p) || (autocvar_touch_move_visible && Touch_HitCircle(p, autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size, 1.0)))
	{
		if (Touch_RoleOwned(TOUCH_ROLE_MOVE))
			return TOUCH_ROLE_IGNORED;
		return TOUCH_ROLE_MOVE;
	}

	if (Touch_InLookZone(p))
	{
		if (Touch_RoleOwned(TOUCH_ROLE_LOOK))
			return TOUCH_ROLE_IGNORED;
		return TOUCH_ROLE_LOOK;
	}

	// Upper-left (and other residual gaps) would otherwise be dead: move needs
	// y>0.42 and look needs x≥look_zone_left (B5). Prefer LOOK for aim.
	if (Touch_RoleOwned(TOUCH_ROLE_LOOK))
		return TOUCH_ROLE_IGNORED;
	return TOUCH_ROLE_LOOK;
}

int Touch_ClaimFinger(int src, vector pos)
{
	int i;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		if (touch_finger_used[i])
			continue;
		touch_finger_used[i] = true;
		touch_finger_src[i] = src;
		touch_finger_down_pos[i] = pos;
		touch_finger_last_pos[i] = pos;
		touch_finger_prev_pos[i] = pos;
		touch_finger_accum[i] = '0 0 0';
		touch_finger_down_time[i] = time;
		touch_finger_lost_time[i] = 0;
		touch_finger_first[i] = true;
		touch_finger_live[i] = true;
		touch_finger_draglook[i] = false;
		touch_finger_role[i] = Touch_AssignRole(pos);
		if (touch_finger_role[i] == TOUCH_ROLE_CON)
			Touch_Console_OnDown(pos);
		else if (touch_finger_role[i] == TOUCH_ROLE_PAUSE)
			Touch_Pause_OnDown(pos);
		else if (touch_finger_role[i] == TOUCH_ROLE_CHAT)
			Touch_Chat_OnDown(pos);
		else if (touch_finger_role[i] == TOUCH_ROLE_SCORES)
			Touch_Scores_OnDown(pos);
		else if (touch_finger_role[i] == TOUCH_ROLE_LOOK)
			Touch_Look_BeginGesture(pos);
		else if (touch_finger_role[i] == TOUCH_ROLE_WEAPON)
		{
			float imp = Touch_Weapons_HitImpulse(pos);
			if (imp >= 0)
				Touch_Weapons_SelectImpulse(imp);
			else if (autocvar_touch_weapon_mode == TOUCH_WEAPON_CYCLE && autocvar_touch_weapon_visible)
				localcmd("weapnext\n");
		}
		return i;
	}
	return -1;
}

void Touch_ApplyMove(vector deflection)
{
	float dz = autocvar_touch_stick_deadzone;
	float mag = vlen(deflection);
	if (mag < dz)
	{
		Touch_SetKey("forward", false);
		Touch_SetKey("back", false);
		Touch_SetKey("moveleft", false);
		Touch_SetKey("moveright", false);
		return;
	}
	vector d = deflection * (autocvar_touch_stick_range / max(mag, 0.001));
	Touch_SetKey("forward", d.y < -0.15);
	Touch_SetKey("back", d.y > 0.15);
	Touch_SetKey("moveleft", d.x < -0.15);
	Touch_SetKey("moveright", d.x > 0.15);
}

void Touch_PollFingers(void)
{
	bool seen0 = false;
	bool seen1 = false;
	bool seen2 = false;
	bool seen3 = false;

	touch_active_finger_count = 0;

	if (checkextension("DP_UT_TOUCHFINGER"))
	{
		int s;
		for (s = 0; s < TOUCH_ENGINE_SLOTS; ++s)
		{
			vector f = Touch_SampleEngineFinger(s);
			if (f.z <= 0)
				continue;
			int idx = Touch_FindFingerBySrc(s);
			if (idx < 0)
				idx = Touch_ClaimFinger(s, f);
			if (idx < 0)
				continue;
			if (idx == 0) seen0 = true;
			else if (idx == 1) seen1 = true;
			else if (idx == 2) seen2 = true;
			else if (idx == 3) seen3 = true;
			touch_finger_live[idx] = true;
			touch_finger_lost_time[idx] = 0;
			touch_finger_prev_pos[idx] = touch_finger_last_pos[idx];
			touch_finger_last_pos[idx] = f;
			if (!touch_finger_first[idx])
				touch_finger_accum[idx] = f - touch_finger_prev_pos[idx];
			else
				touch_finger_accum[idx] = '0 0 0';
			touch_active_finger_count += 1;
		}
	}

	// SDL mouse lives in engine slot MAXFINGERS-1 (=10). Engine zeros it when
	// real fingers are active, so only poll when slots 0..9 were empty.
	if (touch_active_finger_count == 0)
	{
		vector f = Touch_SampleEngineFinger(TOUCH_SRC_POINTER);
		if (f.z > 0)
		{
			int idx = Touch_FindFingerBySrc(TOUCH_SRC_POINTER);
			if (idx < 0)
				idx = Touch_ClaimFinger(TOUCH_SRC_POINTER, f);
			if (idx >= 0)
			{
				if (idx == 0) seen0 = true;
				else if (idx == 1) seen1 = true;
				else if (idx == 2) seen2 = true;
				else if (idx == 3) seen3 = true;
				touch_finger_live[idx] = true;
				touch_finger_lost_time[idx] = 0;
				touch_finger_prev_pos[idx] = touch_finger_last_pos[idx];
				touch_finger_last_pos[idx] = f;
				if (!touch_finger_first[idx])
					touch_finger_accum[idx] = f - touch_finger_prev_pos[idx];
				else
					touch_finger_accum[idx] = '0 0 0';
				touch_active_finger_count += 1;
			}
		}
	}

	float grace = autocvar_touch_finger_grace_ms / 1000.0;
	int i;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		if (!touch_finger_used[i])
			continue;
		bool seen = false;
		if (i == 0) seen = seen0;
		else if (i == 1) seen = seen1;
		else if (i == 2) seen = seen2;
		else if (i == 3) seen = seen3;
		if (seen)
			continue;
		// Chrome pills must not use the lost-finger grace (B4 + pause realtime):
		// a pill is a discrete tap that commits on release, and keeping the
		// finger alive past the release would re-fire it on the next frame.
		int role = touch_finger_role[i];
		if (role == TOUCH_ROLE_CON || role == TOUCH_ROLE_PAUSE || role == TOUCH_ROLE_CHAT
			|| role == TOUCH_ROLE_SCORES)
		{
			vector up = touch_finger_last_pos[i];
			if (role == TOUCH_ROLE_CON)
				Touch_Console_OnUp(up);
			else if (role == TOUCH_ROLE_PAUSE)
				Touch_Pause_OnUp(up);
			else if (role == TOUCH_ROLE_SCORES)
				Touch_Scores_OnUp(up);
			else
				Touch_Chat_OnUp(up);
			touch_finger_used[i] = false;
			touch_finger_role[i] = TOUCH_ROLE_NONE;
			touch_finger_live[i] = false;
			touch_finger_lost_time[i] = 0;
			continue;
		}
		if (touch_finger_lost_time[i] <= 0)
			touch_finger_lost_time[i] = time;
		if (time - touch_finger_lost_time[i] < grace)
		{
			touch_finger_live[i] = true;
			touch_finger_accum[i] = '0 0 0';
			continue;
		}
		if (touch_finger_role[i] == TOUCH_ROLE_LOOK)
		{
			// A look gesture that never escaped the deadzone and lifted quickly
			// was a tap, not an aim: fire one shot. This is the right thumb's
			// fallback when it is away from the FIRE disc. We deliberately do not
			// aim at the touched point — a fingertip fully occludes its target.
			if (autocvar_touch_look_tap_fire && !Touch_Look_Escaped()
				&& touch_finger_lost_time[i] - touch_finger_down_time[i]
					<= autocvar_touch_look_tap_ms / 1000.0)
				touch_tapfire_until = time + 0.06;
			Touch_Look_EndGesture();
		}
		touch_finger_used[i] = false;
		touch_finger_role[i] = TOUCH_ROLE_NONE;
		touch_finger_live[i] = false;
		touch_finger_draglook[i] = false;
	}
}

void Touch_EnsureReady(void)
{
	if (touch_ready)
		return;
	Touch_Look_Reset();
	Touch_Console_InitDefaults();
	Touch_Pause_InitDefaults();
	Touch_Customize_Reset();
	Touch_Chat_Reset();
	Touch_Scores_Reset();
	Touch_Kb_Reset();
	// Before anything reads a touch cvar: a stale saved layout is still in force
	// at this point, and the migration is what replaces it with the new defaults.
	Touch_MigrateLayout();
	Touch_ApplyTouchWeaponsStrip();
	Touch_ApplyChatFeedSettings();
	Touch_ApplyTimerPanel();
	Touch_ApplyNotifyPanel();
	Touch_ApplyMobileHudEngineSettings();
	touch_last_mobile_hud = autocvar_touch_mobile_hud;
	// Migrate legacy simple_draw → glass_quality once.
	if (cvar("touch_simple_draw") && !cvar("touch_glass_quality"))
		cvar_set("touch_glass_quality", "0");
	touch_ready = true;
	touch_last_fire_mode = autocvar_touch_fire_mode;
}

void Touch_TickDebugCmds(void)
{
	if (touch_debug_cmd_window <= 0)
		touch_debug_cmd_window = time;
	if (time - touch_debug_cmd_window >= 1.0)
	{
		touch_debug_cmds_per_sec = touch_debug_cmds;
		touch_debug_cmds = 0;
		touch_debug_cmd_window = time;
	}
}

void Touch_Frame(void)
{
	Touch_EnsureReady();
	touch_debug_fills = 0;
	Touch_TickDebugCmds();

	if (cvar("touch_autoscreenshot") && !touch_autoscreenshot_done)
	{
		touch_autoscreenshot_frames += 1;
		if (touch_autoscreenshot_frames > 90) // ~3s at 30fps
		{
			touch_autoscreenshot_done = true;
			localcmd("\nscreenshot\n");
			print("Touch: autoscreenshot fired\n");
			cvar_set("touch_autoscreenshot", "0");
		}
	}

	if (Touch_CustomizeActive())
	{
		Touch_Customize_Frame();
		return;
	}
	if (Touch_Chat_Active())
	{
		Touch_Chat_Frame();
		return;
	}
	if (!Touch_Active())
	{
		Touch_ReleaseAll();
		return;
	}

	Touch_Look_SetOwnership(Touch_Active());
	Touch_PollFingers();
	Touch_Console_Frame();
	Touch_Pause_Frame();

	if (autocvar_touch_fire_mode != touch_last_fire_mode)
	{
		if (autocvar_touch_fire_mode == TOUCH_FIRE_HOLD)
			touch_fire_toggle_state = false;
		touch_last_fire_mode = autocvar_touch_fire_mode;
	}

	// Turning the touch readouts off has to hand health and ammo back to the
	// stock panels, or the player is left with neither.
	if (autocvar_touch_mobile_hud != touch_last_mobile_hud)
	{
		Touch_ApplyMobileHudEngineSettings();
		touch_last_mobile_hud = autocvar_touch_mobile_hud;
	}

	touch_move_offset = '0 0 0';
	int fire_count = 0;
	bool jump_down = false;
	bool crouch_down = false;
	bool zoom_down = false;
	bool move_active = false;
	bool look_active = false;
	vector look_accum = '0 0 0';
	vector look_last = '0 0 0';
	bool look_first = false;

	int i;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		if (!touch_finger_used[i] || !touch_finger_live[i])
			continue;
		int role = touch_finger_role[i];
		vector pos = touch_finger_last_pos[i];

		if (role == TOUCH_ROLE_CON)
		{
			Touch_Console_PromoteHold();
			Touch_Console_OnMove(pos);
		}
		else if (role == TOUCH_ROLE_PAUSE)
			Touch_Pause_OnMove(pos);
		else if (role == TOUCH_ROLE_CHAT)
			Touch_Chat_OnMove(pos);
		else if (role == TOUCH_ROLE_SCORES)
			Touch_Scores_OnMove(pos);
		else if (role == TOUCH_ROLE_MOVE)
		{
			move_active = true;
			vector center = Touch_WidgetCenter(autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size);
			float radius = Touch_WidgetRadius(autocvar_touch_move_size);
			vector off = pos - center;
			off.z = 0;
			float mag = vlen(off);
			if (mag > radius)
				off = normalize(off) * radius;
			touch_move_offset = off * (1.0 / max(radius, 1));
			Touch_ApplyMove(touch_move_offset);
		}
		else if (role == TOUCH_ROLE_LOOK)
		{
			look_active = true;
			look_accum = touch_finger_accum[i];
			look_last = pos;
			look_first = touch_finger_first[i];
		}
		else if (role == TOUCH_ROLE_FIRE)
		{
			fire_count += 1;
			// CoD Mobile's "R-Fire BTN for camera rotation": the fire button is
			// also an aim surface, so shoot-while-aiming is one finger instead of
			// two. Fire is not released when the drag starts. A dedicated look
			// finger keeps priority.
			if (autocvar_touch_fire_drag_look && !look_active)
			{
				vector from_down = pos - touch_finger_down_pos[i];
				from_down.z = 0;
				if (!touch_finger_draglook[i]
					&& vlen(from_down) >= autocvar_touch_look_deadzone_px)
				{
					touch_finger_draglook[i] = true;
					// Start already escaped: the finger has demonstrably moved,
					// so re-running the deadzone would waste a frame of aim.
					Touch_Look_BeginGestureEscaped(pos);
				}
				if (touch_finger_draglook[i])
				{
					look_active = true;
					look_accum = touch_finger_accum[i];
					look_last = pos;
					look_first = false;
				}
			}
		}
		else if (role == TOUCH_ROLE_FIRE2)
			fire_count += 1;
		else if (role == TOUCH_ROLE_JUMP)
			jump_down = true;
		else if (role == TOUCH_ROLE_CROUCH)
			crouch_down = true;
		else if (role == TOUCH_ROLE_ZOOM)
			zoom_down = true;
		else if (role == TOUCH_ROLE_RELOAD && touch_finger_first[i])
			localcmd("weapon_reload\n");
		else if (role == TOUCH_ROLE_DODGE && touch_finger_first[i])
		{
			localcmd("+dodge\n");
			localcmd("-dodge\n");
		}

		touch_finger_first[i] = false;
	}

	// Demote cancelled CON / PAUSE fingers (ARMING→CANCEL→IDLE while finger still live)
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		if (!touch_finger_used[i])
			continue;
		if (touch_finger_role[i] == TOUCH_ROLE_CON && !Touch_Console_Busy())
			touch_finger_role[i] = TOUCH_ROLE_IGNORED;
		if (touch_finger_role[i] == TOUCH_ROLE_PAUSE && !Touch_Pause_Busy())
			touch_finger_role[i] = TOUCH_ROLE_IGNORED;
	}

	if (!move_active)
	{
		touch_move_offset = '0 0 0';
		Touch_ApplyMove('0 0 0');
	}

	Touch_Look_Frame(look_accum, look_last, look_first, look_active);

	bool pressed_fire = fire_count > 0;
	bool tap_fire = time < touch_tapfire_until;
	if (autocvar_touch_fire_mode == TOUCH_FIRE_TOGGLE)
	{
		if (pressed_fire && !touch_fire_edge)
			touch_fire_toggle_state = !touch_fire_toggle_state;
		touch_fire_edge = pressed_fire;
		Touch_SetKey("attack", touch_fire_toggle_state || tap_fire || autocvar_touch_auto_fire);
	}
	else
	{
		touch_fire_edge = pressed_fire;
		Touch_SetKey("attack", pressed_fire || tap_fire || autocvar_touch_auto_fire);
	}
	touch_fire_down = pressed_fire;

	// Jump is two different actions on this game. A single jump clears a gap or
	// a step, and needs to happen the instant the finger lands. Bunny-hopping
	// needs the key *held*, which on a tablet would cost one of only two thumbs.
	//
	// So the bar is a button first and a latch second: it jumps on press like
	// any button, and only latches if the finger stays down past
	// touch_hop_latch_ms. Tapping while latched stops the hop. A pure toggle
	// would be cheaper but takes the single jump away, which is the more common
	// of the two actions.
	// In latch mode the only thing that clears the latch is a deliberate tap, so a
	// latch set under a different mode would otherwise survive the switch and hold
	// jump down forever — a profile change mid-session left the player hopping with
	// no finger on the bar and no way to stop but another profile change.
	if (autocvar_touch_hop_mode != touch_last_hop_mode)
	{
		touch_last_hop_mode = autocvar_touch_hop_mode;
		touch_hop_latched = false;
		touch_hop_cancelling = false;
	}

	if (autocvar_touch_hop_mode == TOUCH_HOP_LATCH)
	{
		if (jump_down && !touch_jump_edge)
		{
			if (touch_hop_latched)
			{
				// This press means "stop". It must not also read as a jump, and
				// a lingering thumb must not immediately re-latch.
				touch_hop_latched = false;
				touch_hop_cancelling = true;
			}
			else
				touch_hop_press_time = time;
		}
		if (!jump_down)
			touch_hop_cancelling = false;
		if (jump_down && !touch_hop_cancelling && !touch_hop_latched
			&& time - touch_hop_press_time >= autocvar_touch_hop_latch_ms * 0.001)
			touch_hop_latched = true;
		Touch_SetKey("jump", touch_hop_latched || (jump_down && !touch_hop_cancelling));
	}
	else if (autocvar_touch_hop_mode == TOUCH_HOP_AUTO)
	{
		touch_hop_latched = vlen(touch_move_offset) >= autocvar_touch_stick_deadzone;
		Touch_SetKey("jump", jump_down || touch_hop_latched);
	}
	else
	{
		touch_hop_latched = false;
		Touch_SetKey("jump", jump_down);
	}
	touch_jump_edge = jump_down;

	Touch_SetKey("crouch", crouch_down);

	if (autocvar_touch_zoom_mode == TOUCH_ZOOM_TOGGLE)
	{
		if (zoom_down && !touch_zoom_edge)
			touch_zoom_toggle_state = !touch_zoom_toggle_state;
		touch_zoom_edge = zoom_down;
		Touch_SetKey("zoom", touch_zoom_toggle_state);
	}
	else
	{
		touch_zoom_edge = zoom_down;
		Touch_SetKey("zoom", zoom_down);
	}

}

bool Touch_InputEvent(float bInputType, float nPrimary, float nSecondary)
{
	if (Touch_Customize_InputEvent(bInputType, nPrimary, nSecondary))
		return true;
	if (!Touch_Active() || Touch_CustomizeActive())
		return false;

	// The chat sheet is modal and covers the controls, so Escape has to close it
	// rather than fall through and open the game menu behind it.
	if (Touch_Chat_Active() && bInputType == 0 && nPrimary == K_ESCAPE)
	{
		Touch_Chat_Close();
		return true;
	}

	// Escape clears the board before it reaches the game menu: the board is the
	// thing on screen, so it is what Escape is aimed at.
	if (Touch_Scores_Shown() && bInputType == 0 && nPrimary == K_ESCAPE)
	{
		Touch_Scores_Hide();
		return true;
	}

	// Consume MOUSE1 symmetrically so engine +attack/-attack cannot desync (B2).
	// Positions come from gettouchfinger(10) in Touch_PollFingers, or getmousepos
	// in Touch_Pointer_Frame for the modal sheets — never from the event itself.
	if (bInputType == 0 && nPrimary == K_MOUSE1)
	{
		Touch_SetMouseDown(true);
		return true;
	}
	if (bInputType == 1 && nPrimary == K_MOUSE1)
	{
		Touch_SetMouseDown(false);
		return true;
	}
	if (bInputType == 3)
		return true;
	return false;
}

vector Touch_MoveStickOffset(void)
{
	return touch_move_offset;
}

bool Touch_FireHeld(void)
{
	return touch_fire_toggle_state || touch_fire_down;
}

bool Touch_WeaponWheelOpen(void)
{
	return touch_weapon_wheel;
}

bool Touch_JumpHeld(void)
{
	return touch_key_jump;
}

bool Touch_HopLatched(void)
{
	return touch_hop_latched && autocvar_touch_hop_mode != TOUCH_HOP_HOLD;
}

bool Touch_Fire2Held(void)
{
	return Touch_RoleOwned(TOUCH_ROLE_FIRE2);
}

bool Touch_CrouchHeld(void)
{
	return touch_key_crouch;
}

int Touch_DebugFills(void)
{
	return touch_debug_fills;
}

void Touch_DebugAddFills(int n)
{
	touch_debug_fills += n;
}

int Touch_DebugCmdsPerSec(void)
{
	return touch_debug_cmds_per_sec;
}

string Touch_DebugRoleName(int role)
{
	if (role == TOUCH_ROLE_MOVE) return "MOVE";
	if (role == TOUCH_ROLE_LOOK) return "LOOK";
	if (role == TOUCH_ROLE_FIRE) return "FIRE";
	if (role == TOUCH_ROLE_FIRE2) return "FIRE2";
	if (role == TOUCH_ROLE_JUMP) return "HOP";
	if (role == TOUCH_ROLE_CROUCH) return "CR";
	if (role == TOUCH_ROLE_WEAPON) return "WEP";
	if (role == TOUCH_ROLE_ZOOM) return "ZOOM";
	if (role == TOUCH_ROLE_DODGE) return "DODGE";
	if (role == TOUCH_ROLE_RELOAD) return "R";
	if (role == TOUCH_ROLE_CON) return "CON";
	if (role == TOUCH_ROLE_PAUSE) return "PAUSE";
	if (role == TOUCH_ROLE_SHEET) return "SHEET";
	if (role == TOUCH_ROLE_IGNORED) return "IGN";
	return "-";
}

int Touch_DebugFingerRole(int i)
{
	if (i < 0 || i >= TOUCH_MAX_FINGERS)
		return TOUCH_ROLE_NONE;
	if (!touch_finger_used[i])
		return TOUCH_ROLE_NONE;
	return touch_finger_role[i];
}
