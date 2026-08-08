#include "touch_init.qh"
#include "touch_config.qh"
#include "touch_look.qh"

void Touch_RegisterCvar(string name, string value)
{
	registercvar(name, value);
}

void Touch_Init(void)
{
	// Layout — sizes are millimetre-grounded, see docs/TOUCH_UX_REDESIGN.md §3
	// and §7. nsize = mm / 183.2 on the 13" 3:2 reference panel.
	Touch_RegisterCvar("touch_preset", "standard");
	Touch_RegisterCvar("touch_scale", "1.0");
	Touch_RegisterCvar("touch_opacity", "0.90");
	// MOVE and FIRE share a centre line: the old layout put them 0.04 apart,
	// which is exactly the misalignment §1 of the redesign doc complains about.
	Touch_RegisterCvar("touch_move_x", "0.170");
	Touch_RegisterCvar("touch_move_y", "0.680");
	Touch_RegisterCvar("touch_move_size", "0.229"); // 42 mm — spans the thumb arc
	Touch_RegisterCvar("touch_move_visible", "1");
	Touch_RegisterCvar("touch_fire_x", "0.855");
	Touch_RegisterCvar("touch_fire_y", "0.680");
	Touch_RegisterCvar("touch_fire_size", "0.140"); // 26 mm — hot button
	Touch_RegisterCvar("touch_fire_visible", "1");
	// Second fire button for claw grips: costs pixels, not fingers. Off by
	// default so the resting layout stays sparse.
	Touch_RegisterCvar("touch_fire2_x", "0.135");
	Touch_RegisterCvar("touch_fire2_y", "0.240");
	Touch_RegisterCvar("touch_fire2_size", "0.120");
	Touch_RegisterCvar("touch_fire2_visible", "0");
	Touch_RegisterCvar("touch_jump_x", "0.855");
	Touch_RegisterCvar("touch_jump_y", "0.885");
	Touch_RegisterCvar("touch_jump_size", "0.072"); // 13 mm tall capsule
	Touch_RegisterCvar("touch_jump_aspect", "2.6");
	Touch_RegisterCvar("touch_jump_visible", "1");
	Touch_RegisterCvar("touch_crouch_x", "0.700");
	Touch_RegisterCvar("touch_crouch_y", "0.885"); // shares the JUMP baseline
	Touch_RegisterCvar("touch_crouch_size", "0.066"); // 12 mm
	Touch_RegisterCvar("touch_crouch_visible", "1");
	Touch_RegisterCvar("touch_weapon_x", "0.540");
	Touch_RegisterCvar("touch_weapon_y", "0.860");
	Touch_RegisterCvar("touch_weapon_size", "0.100");
	Touch_RegisterCvar("touch_weapon_visible", "0"); // HUD weapons strip is the switcher
	Touch_RegisterCvar("touch_zoom_x", "0.860");
	Touch_RegisterCvar("touch_zoom_y", "0.480");
	Touch_RegisterCvar("touch_zoom_size", "0.100");
	Touch_RegisterCvar("touch_zoom_visible", "0");
	Touch_RegisterCvar("touch_dodge_x", "0.165");
	Touch_RegisterCvar("touch_dodge_y", "0.480");
	Touch_RegisterCvar("touch_dodge_size", "0.100");
	Touch_RegisterCvar("touch_dodge_visible", "0");
	Touch_RegisterCvar("touch_reload_x", "0.720");
	Touch_RegisterCvar("touch_reload_y", "0.480");
	Touch_RegisterCvar("touch_reload_size", "0.100");
	Touch_RegisterCvar("touch_reload_visible", "0");
	Touch_RegisterCvar("touch_con_x", "0.500");
	Touch_RegisterCvar("touch_con_y", "0.045");
	Touch_RegisterCvar("touch_con_size", "0.040");
	Touch_RegisterCvar("touch_con_aspect", "4.2");
	Touch_RegisterCvar("touch_con_visible", "1");
	Touch_RegisterCvar("touch_con_hold_ms", "350");
	Touch_RegisterCvar("touch_con_tap_slop_px", "10");
	Touch_RegisterCvar("touch_con_snap", "0.005");
	Touch_RegisterCvar("touch_con_margin_px", "4");
	Touch_RegisterCvar("touch_con_lock", "0");
	Touch_RegisterCvar("touch_pause_x", "0.320");
	Touch_RegisterCvar("touch_pause_y", "0.045");
	Touch_RegisterCvar("touch_pause_size", "0.040");
	Touch_RegisterCvar("touch_pause_aspect", "2.2");
	Touch_RegisterCvar("touch_pause_visible", "1");
	Touch_RegisterCvar("touch_pause_slop_px", "10");
	// Default off: engine pause can composite over CSQC and hide the sheet.
	// Sheet itself blocks gameplay input; enable freeze for singleplayer preference.
	Touch_RegisterCvar("touch_pause_freeze", "0");
	Touch_RegisterCvar("touch_pause_quit_confirm_ms", "3000");
	Touch_RegisterCvar("touch_menu_resume_bar", "0");
	Touch_RegisterCvar("touch_mobile_hud", "0");
	Touch_RegisterCvar("touch_hud_x", "0.02");
	Touch_RegisterCvar("touch_hud_y", "0.02");
	Touch_RegisterCvar("touch_hud_maxhealth", "100");
	Touch_RegisterCvar("touch_hud_maxarmor", "100");
	Touch_RegisterCvar("touch_ammo_x", "0.02");
	Touch_RegisterCvar("touch_ammo_y", "0.02");
	Touch_RegisterCvar("touch_move_zone_w", "0.32");
	Touch_RegisterCvar("touch_look_zone_left", "0.32");
	Touch_RegisterCvar("touch_look_zone_right", "1.0");
	Touch_RegisterCvar("touch_edge_deadzone_px", "10");
	Touch_RegisterCvar("touch_handedness", "0");

	// Feel
	Touch_RegisterCvar("touch_sens_base", "2.8");
	Touch_RegisterCvar("touch_sens_y_mult", "0.85");
	Touch_RegisterCvar("touch_invert_y", "0");
	Touch_RegisterCvar("touch_look_smoothing", "1");
	Touch_RegisterCvar("touch_look_deadzone_px", "4");
	// 1 euro filter: adaptive cutoff, so slow thumb movement is de-jittered and
	// fast movement stays lag-free. fcmin governs slow-speed jitter in Hz, beta
	// governs high-speed lag. 2 restores the old fixed exponential smoother.
	Touch_RegisterCvar("touch_look_filter", "1");
	Touch_RegisterCvar("touch_look_fcmin", "1.5");
	Touch_RegisterCvar("touch_look_beta", "0.03");
	// A tap in the look zone that never becomes a drag fires one shot.
	Touch_RegisterCvar("touch_look_tap_fire", "1");
	Touch_RegisterCvar("touch_look_tap_ms", "200");
	Touch_RegisterCvar("touch_look_escape_carry", "0.5");
	Touch_RegisterCvar("touch_look_max_deg_per_s", "900");
	Touch_RegisterCvar("touch_look_owns_view", "1");
	Touch_RegisterCvar("touch_look_glide", "0");
	Touch_RegisterCvar("touch_stick_deadzone", "0.18");
	Touch_RegisterCvar("touch_stick_range", "1.0");
	Touch_RegisterCvar("touch_aim_assist", "0");
	Touch_RegisterCvar("touch_gyro_enabled", "0");
	Touch_RegisterCvar("touch_gyro_sens", "0.5");
	Touch_RegisterCvar("touch_gyro_ads_only", "1");
	Touch_RegisterCvar("touch_finger_grace_ms", "120");

	// Behavior
	Touch_RegisterCvar("touch_fire_mode", "0");
	Touch_RegisterCvar("touch_fire_slide_release", "0");
	// Sliding off FIRE keeps shooting and starts aiming, so shoot-while-aiming is
	// one finger instead of two (docs/TOUCH_UX_REDESIGN.md §6.2).
	Touch_RegisterCvar("touch_fire_drag_look", "1");
	// Jump is a held modifier in this game; latch it instead of parking a finger
	// on the bar. 0 hold, 1 latch, 2 auto-while-moving.
	Touch_RegisterCvar("touch_hop_mode", "1");
	Touch_RegisterCvar("touch_zoom_mode", "0");
	Touch_RegisterCvar("touch_weapon_mode", "2");
	Touch_RegisterCvar("touch_weapon_slot1", "Laser");
	Touch_RegisterCvar("touch_weapon_slot2", "Shotgun");
	Touch_RegisterCvar("touch_weapon_slot3", "Grenade Launcher");
	Touch_RegisterCvar("touch_auto_fire", "0");
	Touch_RegisterCvar("touch_dodge_mode", "0");
	Touch_RegisterCvar("touch_scoreboard_gesture", "0"); // off until deliberate two-finger tap is verified (B1)
	Touch_RegisterCvar("touch_chat_mode", "1");
	Touch_RegisterCvar("touch_minimal_more", "0");
	Touch_RegisterCvar("touch_setup_done", "0");
	Touch_RegisterCvar("touch_profile_done", "0");
	Touch_RegisterCvar("touch_hud_scale", "0.85");
	Touch_RegisterCvar("touch_performance_profile", "thermal");
	Touch_RegisterCvar("touch_simple_draw", "0");
	Touch_RegisterCvar("touch_glass_quality", "1");
	Touch_RegisterCvar("touch_debug", "0");
	Touch_RegisterCvar("touch_customize", "0");
	Touch_RegisterCvar("touch_layout_file", "touch.layout.cfg");
	Touch_RegisterCvar("touch_autoscreenshot", "0");

	registercommand("touch_save");
	registercommand("touch_export");
	registercommand("touch_import");
	registercommand("touch_apply_preset");
	registercommand("touch_reset_preset");
	// touch_customize is a cvar; toggle via "touch_customize 1" / ConsoleCommand handler.
	registercommand("touch_customize_save");
	registercommand("touch_customize_cancel");
	registercommand("touch_pause");
	registercommand("touch_pause_exit");

	Touch_EnforceSquare2D();
	Touch_LoadStartupConfigs();
}

void Touch_EnforceSquare2D(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	// Touch geometry assumes 2D space has the same aspect as the display: a
	// circle must be round and a hit box must sit under what you can see.
	// The launcher can only guess the panel size (gdctl and /sys are outside
	// the Flatpak sandbox) and Wayland grants whatever fullscreen surface it
	// likes, so a pinned vid_conwidth ends up describing the wrong screen —
	// a 16:9 console on this 3:2 tablet stretched every control. With
	// vid_conwidthauto the engine derives conwidth from the real surface each
	// frame, so 2D stays square no matter what the launcher guessed.
	cvar_set("vid_conwidthauto", "1");
}

void Touch_ApplyTouchWeaponsStrip(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	// Prefer the stock vertical weapons HUD as the touch switcher.
	cvar_set("touch_weapon_visible", "0");
	cvar_set("hud_panel_weapons", "1");
	cvar_set("hud_panel_weapons_onlyowned", "1");
	cvar_set("hud_panel_weapons_timeout", "0");
	cvar_set("hud_panel_weapons_label", "1");
	cvar_set("hud_panel_weapons_aspect", "0.9");
	localcmd("seta hud_panel_weapons_pos \"0.915 0.18\"\n");
	localcmd("seta hud_panel_weapons_size \"0.075 0.58\"\n");
	localcmd("seta hud_panel_weapons_bg 1\n");
	localcmd("seta hud_panel_weapons_bg_alpha 0.35\n");
}

void Touch_ApplyMobileHudEngineSettings(void)
{
	if (!cvar("touch_mobile_hud"))
		return;
	cvar_set("hud_panel_healtharmor", "0");
	cvar_set("hud_panel_ammo", "0");
}

void Touch_LoadStartupConfigs(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	if (whichpack("touch/startup.cfg"))
		localcmd("exec touch/startup.cfg\n");
	else
	{
		localcmd("exec touch/profiles/standard.cfg\n");
		localcmd("exec touch/profiles/battery.cfg\n");
	}
	Touch_ApplyMobileHudEngineSettings();
	Touch_ApplyTouchWeaponsStrip();
	Touch_ApplySensitivity();
}

void Touch_ApplySensitivity(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	// When CSQC owns look, never write sensitivity (D2). Ownership sets it to 0.
	if (autocvar_touch_look_owns_view)
	{
		cvar_set("m_pitch", "0.022");
		cvar_set("m_yaw", "0.022");
		return;
	}
	float dpi = max(160, cvar("vid_touchscreen_xdpi"));
	float effective = autocvar_touch_sens_base * (320.0 / dpi);
	float pitch = 0.022 * autocvar_touch_sens_y_mult;
	if (autocvar_touch_invert_y)
		pitch = -pitch;
	cvar_set("sensitivity", ftos(effective));
	cvar_set("m_pitch", ftos(pitch));
	cvar_set("m_yaw", "0.022");
}
