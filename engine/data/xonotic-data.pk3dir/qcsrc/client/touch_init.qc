#include "touch_init.qh"
#include "touch_config.qh"

void Touch_RegisterCvar(string name, string value)
{
	registercvar(name, value);
}

void Touch_Init(void)
{
	// Layout
	Touch_RegisterCvar("touch_preset", "standard");
	Touch_RegisterCvar("touch_scale", "1.0");
	Touch_RegisterCvar("touch_opacity", "0.65");
	Touch_RegisterCvar("touch_move_x", "0.12");
	Touch_RegisterCvar("touch_move_y", "0.72");
	Touch_RegisterCvar("touch_move_size", "0.18");
	Touch_RegisterCvar("touch_fire_x", "0.88");
	Touch_RegisterCvar("touch_fire_y", "0.72");
	Touch_RegisterCvar("touch_fire_size", "0.14");
	Touch_RegisterCvar("touch_jump_x", "0.72");
	Touch_RegisterCvar("touch_jump_y", "0.82");
	Touch_RegisterCvar("touch_jump_size", "0.11");
	Touch_RegisterCvar("touch_crouch_x", "0.62");
	Touch_RegisterCvar("touch_crouch_y", "0.82");
	Touch_RegisterCvar("touch_crouch_size", "0.10");
	Touch_RegisterCvar("touch_weapon_x", "0.52");
	Touch_RegisterCvar("touch_weapon_y", "0.82");
	Touch_RegisterCvar("touch_weapon_size", "0.11");
	Touch_RegisterCvar("touch_zoom_x", "0.42");
	Touch_RegisterCvar("touch_zoom_y", "0.82");
	Touch_RegisterCvar("touch_zoom_size", "0.10");
	Touch_RegisterCvar("touch_dodge_x", "0.12");
	Touch_RegisterCvar("touch_dodge_y", "0.58");
	Touch_RegisterCvar("touch_dodge_size", "0.10");
	Touch_RegisterCvar("touch_dodge_visible", "1");
	Touch_RegisterCvar("touch_reload_x", "0.76");
	Touch_RegisterCvar("touch_reload_y", "0.58");
	Touch_RegisterCvar("touch_reload_size", "0.10");
	Touch_RegisterCvar("touch_mobile_hud", "1");
	Touch_RegisterCvar("touch_hud_x", "0.02");
	Touch_RegisterCvar("touch_hud_y", "0.02");
	Touch_RegisterCvar("touch_hud_maxhealth", "100");
	Touch_RegisterCvar("touch_hud_maxarmor", "100");
	Touch_RegisterCvar("touch_ammo_x", "0.02");
	Touch_RegisterCvar("touch_ammo_y", "0.02");
	Touch_RegisterCvar("touch_look_zone_left", "0.45");
	Touch_RegisterCvar("touch_look_zone_right", "1.0");
	Touch_RegisterCvar("touch_edge_deadzone_px", "16");
	Touch_RegisterCvar("touch_handedness", "0");

	// Feel
	Touch_RegisterCvar("touch_sens_base", "3.5");
	Touch_RegisterCvar("touch_sens_y_mult", "1.0");
	Touch_RegisterCvar("touch_invert_y", "0");
	Touch_RegisterCvar("touch_look_smoothing", "1");
	Touch_RegisterCvar("touch_look_deadzone_px", "8");
	Touch_RegisterCvar("touch_stick_deadzone", "0.15");
	Touch_RegisterCvar("touch_stick_range", "1.0");
	Touch_RegisterCvar("touch_aim_assist", "0");
	Touch_RegisterCvar("touch_gyro_enabled", "0");
	Touch_RegisterCvar("touch_gyro_sens", "0.5");
	Touch_RegisterCvar("touch_gyro_ads_only", "1");

	// Behavior
	Touch_RegisterCvar("touch_fire_mode", "0");
	Touch_RegisterCvar("touch_zoom_mode", "0");
	Touch_RegisterCvar("touch_weapon_mode", "0");
	Touch_RegisterCvar("touch_weapon_slot1", "Laser");
	Touch_RegisterCvar("touch_weapon_slot2", "Shotgun");
	Touch_RegisterCvar("touch_weapon_slot3", "Grenade Launcher");
	Touch_RegisterCvar("touch_auto_fire", "0");
	Touch_RegisterCvar("touch_dodge_mode", "0");
	Touch_RegisterCvar("touch_scoreboard_gesture", "1");
	Touch_RegisterCvar("touch_chat_mode", "1");
	Touch_RegisterCvar("touch_minimal_more", "0");
	Touch_RegisterCvar("touch_setup_done", "0");
	Touch_RegisterCvar("touch_hud_scale", "1.0");
	Touch_RegisterCvar("touch_performance_profile", "balanced");
	Touch_RegisterCvar("touch_customize", "0");
	Touch_RegisterCvar("touch_layout_file", "touch.layout.cfg");

	registercommand("touch_save");
	registercommand("touch_export");
	registercommand("touch_import");
	registercommand("touch_apply_preset");
	registercommand("touch_reset_preset");
	registercommand("touch_customize");
	registercommand("touch_customize_save");
	registercommand("touch_customize_cancel");

	Touch_ApplySensitivity();
}

void Touch_ApplySensitivity(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	float dpi = max(160, cvar("vid_touchscreen_xdpi"));
	float effective = autocvar_touch_sens_base * (320.0 / dpi);
	float pitch = 0.022 * autocvar_touch_sens_y_mult * (autocvar_touch_invert_y ? -1 : 1);
	cvar_set("sensitivity", ftos(effective));
	cvar_set("m_pitch", ftos(pitch));
	cvar_set("m_yaw", "0.022");
}
