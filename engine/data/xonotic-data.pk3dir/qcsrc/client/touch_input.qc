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

vector touch_move_offset;
bool touch_fire_down;
bool touch_fire_edge;
bool touch_fire_toggle_state;
bool touch_zoom_edge;
bool touch_zoom_toggle_state;
bool touch_weapon_wheel;
bool touch_mouse_down;
vector touch_pointer_pos;
float touch_two_finger_time;
int touch_active_finger_count;
bool touch_ready;
int touch_last_fire_mode;
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
	if (Touch_CustomizeActive())
		return true;
	if (!cvar("vid_touchscreen"))
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
	touch_two_finger_time = 0;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		touch_finger_used[i] = false;
		touch_finger_role[i] = TOUCH_ROLE_NONE;
		touch_finger_live[i] = false;
		touch_finger_lost_time[i] = 0;
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
	// Modal pause sheet owns every contact.
	if (Touch_Pause_SheetOpen())
		return TOUCH_ROLE_SHEET;

	if (Touch_Console_Hit(p))
		return TOUCH_ROLE_CON;

	if (Touch_Pause_PillHit(p))
		return TOUCH_ROLE_PAUSE;

	// Stock vertical weapons strip — tap icon to switch (replaces WEP button).
	if (Touch_Weapons_Hit(p))
		return TOUCH_ROLE_WEAPON;

	if (Touch_InEdgeDeadzone(p))
		return TOUCH_ROLE_IGNORED;

	if (Touch_Hud_HitPanel(p))
		return TOUCH_ROLE_IGNORED;

	if (autocvar_touch_fire_visible && Touch_HitCircle(p, autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size, 1.15))
		return TOUCH_ROLE_FIRE;
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
		touch_finger_role[i] = Touch_AssignRole(pos);
		if (touch_finger_role[i] == TOUCH_ROLE_CON)
			Touch_Console_OnDown(pos);
		else if (touch_finger_role[i] == TOUCH_ROLE_PAUSE || touch_finger_role[i] == TOUCH_ROLE_SHEET)
			Touch_Pause_OnDown(pos);
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
		// CON / PAUSE / SHEET must not use grace (B4 + pause realtime).
		if (touch_finger_role[i] == TOUCH_ROLE_CON)
		{
			Touch_Console_OnUp(touch_finger_last_pos[i]);
			touch_finger_used[i] = false;
			touch_finger_role[i] = TOUCH_ROLE_NONE;
			touch_finger_live[i] = false;
			touch_finger_lost_time[i] = 0;
			continue;
		}
		if (touch_finger_role[i] == TOUCH_ROLE_PAUSE || touch_finger_role[i] == TOUCH_ROLE_SHEET)
		{
			Touch_Pause_OnUp(touch_finger_last_pos[i]);
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
			Touch_Look_EndGesture();
		touch_finger_used[i] = false;
		touch_finger_role[i] = TOUCH_ROLE_NONE;
		touch_finger_live[i] = false;
	}
}

void Touch_EnsureReady(void)
{
	if (touch_ready)
		return;
	Touch_Look_Reset();
	Touch_Console_InitDefaults();
	Touch_Pause_InitDefaults();
	Touch_ApplyTouchWeaponsStrip();
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
	if (!Touch_Active() && !Touch_Pause_SheetOpen())
	{
		Touch_ReleaseAll();
		return;
	}

	Touch_Look_SetOwnership(Touch_Active());
	Touch_PollFingers();
	Touch_Console_Frame();
	Touch_Pause_Frame();

	if (Touch_Pause_SheetOpen())
	{
		int pi;
		for (pi = 0; pi < TOUCH_MAX_FINGERS; ++pi)
		{
			if (!touch_finger_used[pi] || !touch_finger_live[pi])
				continue;
			if (touch_finger_role[pi] == TOUCH_ROLE_SHEET || touch_finger_role[pi] == TOUCH_ROLE_PAUSE)
				Touch_Pause_OnMove(touch_finger_last_pos[pi]);
			touch_finger_first[pi] = false;
		}
		return;
	}

	if (!Touch_Active())
	{
		Touch_ReleaseAll();
		return;
	}

	if (autocvar_touch_fire_mode != touch_last_fire_mode)
	{
		if (autocvar_touch_fire_mode == TOUCH_FIRE_HOLD)
			touch_fire_toggle_state = false;
		touch_last_fire_mode = autocvar_touch_fire_mode;
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
	if (autocvar_touch_fire_mode == TOUCH_FIRE_TOGGLE)
	{
		if (pressed_fire && !touch_fire_edge)
			touch_fire_toggle_state = !touch_fire_toggle_state;
		touch_fire_edge = pressed_fire;
		Touch_SetKey("attack", touch_fire_toggle_state || autocvar_touch_auto_fire);
	}
	else
	{
		touch_fire_edge = pressed_fire;
		Touch_SetKey("attack", pressed_fire || autocvar_touch_auto_fire);
	}
	touch_fire_down = pressed_fire;

	Touch_SetKey("jump", jump_down);
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

	// Two-finger "scoreboard" previously called Scoreboard_UI_Enable(1) which is
	// team-select and Release_Common_Keys — breaks FIRE+LOOK (B1). Keep off by
	// default; only a future deliberate tap should enable this path.
	if (!autocvar_touch_scoreboard_gesture || touch_active_finger_count < 2)
		touch_two_finger_time = 0;
}

bool Touch_InputEvent(float bInputType, float nPrimary, float nSecondary)
{
	if (Touch_Customize_InputEvent(bInputType, nPrimary, nSecondary))
		return true;
	if (!Touch_Active() || Touch_CustomizeActive())
		return false;

	// Consume MOUSE1 symmetrically so engine +attack/-attack cannot desync (B2).
	// Positions come from gettouchfinger(10) in Touch_PollFingers, not getmousepos.
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
	{
		touch_pointer_pos = Touch_NormalizeInput(nPrimary * eX + nSecondary * eY);
		return true;
	}
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
	if (role == TOUCH_ROLE_JUMP) return "SPACE";
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
