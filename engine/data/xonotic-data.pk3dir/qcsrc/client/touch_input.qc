#include "touch_input.qh"
#include "touch_customize.qh"
#include "touch_glass.qh"
#include "touch_hud.qh"
#include <client/hud/panel/scoreboard.qh>
#include "../dpdefs/ut_touchfinger.qc"

int touch_finger_engine_slot[TOUCH_MAX_FINGERS];
int touch_finger_role[TOUCH_MAX_FINGERS];
vector touch_finger_last_pos[TOUCH_MAX_FINGERS];
vector touch_finger_prev_pos[TOUCH_MAX_FINGERS];
bool touch_finger_active[TOUCH_MAX_FINGERS];

vector touch_move_offset;
bool touch_fire_down;
bool touch_fire_edge;
bool touch_fire_toggle_state;
bool touch_weapon_wheel;
bool touch_zoom_toggle_state;
bool touch_mouse_down;
float touch_two_finger_time;
int touch_active_finger_count;

bool touch_btn_jump;
bool touch_btn_crouch;
bool touch_btn_zoom;
bool touch_btn_dodge;
bool touch_btn_reload;

bool Touch_Active(void)
{
	if (Touch_CustomizeActive())
		return true;
	return cvar("vid_touchscreen") && autocvar__menu_alpha < 1;
}

vector Touch_GetFinger(int engine_slot)
{
	if (checkextension("DP_UT_TOUCHFINGER"))
	{
		vector f = gettouchfinger(engine_slot);
		if (f.z > 0)
			return f;
		if (engine_slot == 0)
			return gettouchfinger(10);
		return '0 0 0';
	}
	if (engine_slot == 0)
	{
		vector p = getmousepos();
		p.z = touch_mouse_down;
		return p;
	}
	return '0 0 0';
}

bool Touch_HitButton(vector p, float nx, float ny, float nsize)
{
	vector c = Touch_WidgetCenter(nx, ny, nsize);
	return Touch_PointInCircle(p, c, Touch_WidgetRadius(nsize));
}

int Touch_AssignRole(vector p)
{
	if (Touch_InEdgeDeadzone(p))
		return TOUCH_SLOT_NONE;

	if (Touch_Hud_HitPanel(p))
		return TOUCH_SLOT_NONE;

	if (Touch_HitButton(p, autocvar_touch_fire_x, autocvar_touch_fire_y, autocvar_touch_fire_size))
		return TOUCH_SLOT_FIRE;
	if (Touch_HitButton(p, autocvar_touch_jump_x, autocvar_touch_jump_y, autocvar_touch_jump_size))
		return 100;
	if (Touch_HitButton(p, autocvar_touch_crouch_x, autocvar_touch_crouch_y, autocvar_touch_crouch_size))
		return 101;
	if (Touch_HitButton(p, autocvar_touch_weapon_x, autocvar_touch_weapon_y, autocvar_touch_weapon_size))
		return 102;
	if (Touch_HitButton(p, autocvar_touch_zoom_x, autocvar_touch_zoom_y, autocvar_touch_zoom_size))
		return 103;
	if (Touch_HitButton(p, autocvar_touch_reload_x, autocvar_touch_reload_y, autocvar_touch_reload_size))
		return 105;
	if (autocvar_touch_dodge_visible && Touch_HitButton(p, autocvar_touch_dodge_x, autocvar_touch_dodge_y, autocvar_touch_dodge_size))
		return 104;
	if (Touch_HitButton(p, autocvar_touch_move_x, autocvar_touch_move_y, autocvar_touch_move_size))
		return TOUCH_SLOT_MOVE;
	if (Touch_InLookZone(p))
		return TOUCH_SLOT_LOOK;
	return TOUCH_SLOT_NONE;
}

void Touch_SetKey(string key, bool down)
{
	if (down)
		localcmd(strcat("+", key, "\n"));
	else
		localcmd(strcat("-", key, "\n"));
}

void Touch_ReleaseAll(void)
{
	Touch_SetKey("forward", false);
	Touch_SetKey("back", false);
	Touch_SetKey("moveleft", false);
	Touch_SetKey("moveright", false);
	Touch_SetKey("attack", false);
	Touch_SetKey("jump", false);
	Touch_SetKey("crouch", false);
	Touch_SetKey("zoom", false);
	touch_fire_down = false;
	touch_move_offset = '0 0 0';
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
	Touch_SetKey("forward", d.y > 0.15);
	Touch_SetKey("back", d.y < -0.15);
	Touch_SetKey("moveleft", d.x < -0.15);
	Touch_SetKey("moveright", d.x > 0.15);
}

void Touch_WeaponImpulseByName(string wepname)
{
	int impulse = 0;
	if (wepname == "Laser" || wepname == "Blaster" || wepname == "laser")
		impulse = 1;
	else if (wepname == "Shotgun" || wepname == "shotgun")
		impulse = 2;
	else if (wepname == "Uzi" || wepname == "uzi")
		impulse = 3;
	else if (wepname == "Mortar" || wepname == "mortar")
		impulse = 4;
	else if (wepname == "Crylink" || wepname == "crylink")
		impulse = 5;
	else if (wepname == "Grenade Launcher" || wepname == "grenade")
		impulse = 6;
	else if (wepname == "Rocket Launcher" || wepname == "rocket")
		impulse = 7;
	else if (wepname == "Electro" || wepname == "electro")
		impulse = 8;
	else if (wepname == "Hagar" || wepname == "hagar")
		impulse = 9;
	if (impulse)
		localcmd(strcat("impulse ", itos(impulse), "\n"));
}

void Touch_WeaponCycle(void)
{
	localcmd("weapnext\n");
}

void Touch_Dodge(void)
{
	localcmd("+dodge\n");
	localcmd("-dodge\n");
}

void Touch_PollFingers(void)
{
	int i;
	touch_active_finger_count = 0;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		touch_finger_prev_pos[i] = touch_finger_last_pos[i];
		vector f = Touch_GetFinger(touch_finger_engine_slot[i]);
		bool was = touch_finger_active[i];
		touch_finger_active[i] = f.z > 0;
		touch_finger_last_pos[i] = f;
		f.z = 0;
		if (touch_finger_active[i])
			touch_active_finger_count += 1;
		if (!was && touch_finger_active[i])
		{
			int role = Touch_AssignRole(touch_finger_last_pos[i]);
			touch_finger_role[i] = role;
			// Edge-triggered buttons (tap once on finger down)
			if (role == 100)
				touch_btn_jump = true;
			else if (role == 101)
				touch_btn_crouch = true;
			else if (role == 102)
			{
				if (autocvar_touch_weapon_mode == TOUCH_WEAPON_WHEEL)
					touch_weapon_wheel = !touch_weapon_wheel;
				else if (autocvar_touch_weapon_mode == TOUCH_WEAPON_3SLOT)
				{
					vector c = Touch_WidgetCenter(autocvar_touch_weapon_x, autocvar_touch_weapon_y, autocvar_touch_weapon_size);
					float r = max(Touch_WidgetRadius(autocvar_touch_weapon_size) * 2, 1);
					float third = (touch_finger_last_pos[i].x - (c.x - Touch_WidgetRadius(autocvar_touch_weapon_size))) / r;
					if (third < 0.33)
						Touch_WeaponImpulseByName(autocvar_touch_weapon_slot1);
					else if (third < 0.66)
						Touch_WeaponImpulseByName(autocvar_touch_weapon_slot2);
					else
						Touch_WeaponImpulseByName(autocvar_touch_weapon_slot3);
				}
				else
					Touch_WeaponCycle();
			}
			else if (role == 103)
				touch_btn_zoom = true;
			else if (role == 104)
				touch_btn_dodge = true;
			else if (role == 105)
				touch_btn_reload = true;
		}
		if (was && !touch_finger_active[i])
			touch_finger_role[i] = TOUCH_SLOT_NONE;
	}
}

bool touch_ready;

STATIC_INIT(touch_slots) {
	int i;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
		touch_finger_engine_slot[i] = i;
}

void Touch_EnsureReady(void)
{
	if (touch_ready)
		return;
	Touch_Look_Reset();
	touch_ready = true;
}

void Touch_Frame(void)
{
	Touch_EnsureReady();
	if (Touch_CustomizeActive())
	{
		Touch_Customize_Frame();
		return;
	}
	if (!Touch_Active())
	{
		Touch_ReleaseAll();
		return;
	}

	Touch_ApplySensitivity();
	Touch_PollFingers();

	touch_move_offset = '0 0 0';
	touch_fire_down = false;
	int i;
	for (i = 0; i < TOUCH_MAX_FINGERS; ++i)
	{
		if (!touch_finger_active[i])
			continue;
		vector pos = touch_finger_last_pos[i];
		vector delta = pos - touch_finger_prev_pos[i];
		int role = touch_finger_role[i];

		if (role == TOUCH_SLOT_MOVE)
		{
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
		else if (role == TOUCH_SLOT_LOOK)
			Touch_Look_ApplyDelta(delta.x, delta.y);
		else if (role == TOUCH_SLOT_FIRE)
			touch_fire_down = true;
	}

	if (autocvar_touch_fire_mode == TOUCH_FIRE_TOGGLE)
	{
		if (touch_fire_down && !touch_fire_edge)
			touch_fire_toggle_state = !touch_fire_toggle_state;
		touch_fire_edge = touch_fire_down;
		Touch_SetKey("attack", touch_fire_toggle_state || autocvar_touch_auto_fire);
	}
	else
		Touch_SetKey("attack", touch_fire_down || autocvar_touch_auto_fire);

	Touch_SetKey("jump", touch_btn_jump);
	Touch_SetKey("crouch", touch_btn_crouch);
	if (autocvar_touch_zoom_mode == TOUCH_ZOOM_TOGGLE)
	{
		if (touch_btn_zoom)
			touch_zoom_toggle_state = !touch_zoom_toggle_state;
		Touch_SetKey("zoom", touch_zoom_toggle_state);
	}
	else
		Touch_SetKey("zoom", touch_btn_zoom);

	touch_btn_jump = false;
	touch_btn_crouch = false;
	touch_btn_zoom = false;

	if (touch_btn_reload)
		localcmd("weapon_reload\n");
	touch_btn_reload = false;

	if (autocvar_touch_scoreboard_gesture && touch_active_finger_count >= 2)
	{
		if (!touch_two_finger_time)
			touch_two_finger_time = time;
		else if (time - touch_two_finger_time < 0.25)
			Scoreboard_UI_Enable(1);
	}
	else
		touch_two_finger_time = 0;

	Touch_Look_Frame();
}

bool Touch_InputEvent(float bInputType, float nPrimary, float nSecondary)
{
	if (Touch_Customize_InputEvent(bInputType, nPrimary, nSecondary))
		return true;
	if (!Touch_Active() || Touch_CustomizeActive())
		return false;

	if (bInputType == 0 && nPrimary == K_MOUSE1)
	{
		touch_mouse_down = true;
		touch_finger_engine_slot[0] = 0;
		return true;
	}
	if (bInputType == 1 && nPrimary == K_MOUSE1)
	{
		touch_mouse_down = false;
		return true;
	}
	if (bInputType == 3)
	{
		touch_finger_last_pos[0] = nPrimary * eX + nSecondary * eY;
		return true;
	}
	if (bInputType == 2 && touch_mouse_down)
	{
		if (Touch_InLookZone(touch_finger_last_pos[0]))
		{
			Touch_Look_ApplyDelta(nPrimary, nSecondary);
			return true;
		}
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
