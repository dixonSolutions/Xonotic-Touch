#include "touch_init.qh"
#include "touch_config.qh"
#include "touch_look.qh"

// One table, three jobs.
//
// Registering the defaults, resetting to them, and saving the current values
// used to be three separate lists of the same ~100 cvars — here, in
// touch/profiles/standard.cfg, and in Touch_SaveLayoutFile. They drifted, and
// because the profile is exec'd *after* registration it silently won: the
// profile still carried pre-redesign geometry, so changing a default here did
// nothing at all on a real device. Now the list exists once and the mode
// globals below decide what walking it does. They stay private to this file;
// Touch_ResetDefaults and Touch_WriteAllCvars are the only way to set them.
float touch_def_fh = -1;  // >= 0 while saving a layout file
bool touch_def_force;     // true while resetting to defaults

void Touch_RegisterCvar(string name, string value)
{
	registercvar(name, value);
	if (touch_def_force)
		cvar_set(name, value);
	if (touch_def_fh >= 0)
		fputs(touch_def_fh, strcat("seta ", name, " \"", cvar_string(name), "\"\n"));
}

// Session state, not preference: registered so the code can read it, but never
// written to a layout file and never reset, because it has no meaningful default
// to go back to.
void Touch_RegisterTransientCvar(string name, string value)
{
	registercvar(name, value);
}

void Touch_DefineCvars(void)
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
	// Top band, left to right: vitals (0.02–0.37), match clock (centre), scores
	// (right), chrome pills under the scores.
	//
	// The three pills used to be one centred row spanning 0.44–0.98, which left
	// no centre for the clock and jammed CONSOLE against the score panel — four
	// unrelated things in one 11 mm strip. Three pills of this size simply do not
	// fit beside two readouts on a 3:2 panel, so the rare one moved out: MENU and
	// CHAT are what a player reaches for mid-match, while CONSOLE is a
	// power-user tool and now sits alone in the empty bottom-left corner.
	// Same size and shape as MENU and CHAT even though it sits apart: it is the
	// same kind of control, and the corner it moved to has the room. Position is
	// what de-emphasises it, not a different silhouette.
	Touch_RegisterCvar("touch_con_x", "0.090");
	Touch_RegisterCvar("touch_con_y", "0.948");
	Touch_RegisterCvar("touch_con_size", "0.060");
	Touch_RegisterCvar("touch_con_aspect", "2.9");
	Touch_RegisterCvar("touch_con_visible", "1");
	Touch_RegisterCvar("touch_con_hold_ms", "350");
	Touch_RegisterCvar("touch_con_tap_slop_px", "10");
	Touch_RegisterCvar("touch_con_snap", "0.005");
	Touch_RegisterCvar("touch_con_margin_px", "4");
	Touch_RegisterCvar("touch_con_lock", "0");
	// MENU and CHAT are a pair: same size and shape because they are the same
	// kind of thing (Law of Similarity), adjacent because they belong together
	// (Law of Proximity), and 0.060 puts them at 11 mm tall — the old 0.040 was
	// 7.3 mm, under the 9 mm minimum, on controls at the far edge of thumb reach
	// where Fitts argues for the opposite.
	Touch_RegisterCvar("touch_pause_x", "0.680");
	Touch_RegisterCvar("touch_pause_y", "0.088");
	Touch_RegisterCvar("touch_pause_size", "0.060");
	Touch_RegisterCvar("touch_pause_aspect", "2.9");
	Touch_RegisterCvar("touch_pause_visible", "1");
	Touch_RegisterCvar("touch_pause_slop_px", "10");
	// Outermost, because it is the one that matters in a match and the edge is
	// the easiest target to hit without looking.
	Touch_RegisterCvar("touch_chat_x", "0.862");
	Touch_RegisterCvar("touch_chat_y", "0.088");
	Touch_RegisterCvar("touch_chat_size", "0.060");
	Touch_RegisterCvar("touch_chat_aspect", "2.9");
	Touch_RegisterCvar("touch_chat_visible", "1");
	// Keyboard: 0.38 of the height gives 4 rows of ~15 mm keys, well over the
	// 9 mm floor, while leaving room for the backlog above it.
	Touch_RegisterCvar("touch_kb_height", "0.38");
	Touch_RegisterCvar("touch_kb_maxlen", "120");
	// Default off: engine pause can composite over CSQC and hide the sheet.
	// Sheet itself blocks gameplay input; enable freeze for singleplayer preference.
	Touch_RegisterCvar("touch_pause_freeze", "0");
	Touch_RegisterCvar("touch_pause_quit_confirm_ms", "3000");
	Touch_RegisterCvar("touch_menu_resume_bar", "0");
	// On by default. The shipped config disables the stock healtharmor and ammo
	// panels because this replaces them, so shipping this off meant the player
	// had no vitals readout at all.
	Touch_RegisterCvar("touch_mobile_hud", "1");
	// Top-left: first place the eye lands, never under a thumb, and outside both
	// the move zone (needs y > 0.42) and the look zone (starts at x 0.32).
	Touch_RegisterCvar("touch_hud_x", "0.150");
	Touch_RegisterCvar("touch_hud_y", "0.075");
	Touch_RegisterCvar("touch_hud_maxhealth", "100");
	Touch_RegisterCvar("touch_hud_maxarmor", "100");
	// Ammo belongs to FIRE, so it sits in the gap between FIRE and HOP on the
	// same centre line — below the weapons strip, which ends at y 0.76.
	Touch_RegisterCvar("touch_ammo_x", "0.855");
	Touch_RegisterCvar("touch_ammo_y", "0.795");
	// Right edge, mid-height: clear of the scores above and FIRE below, and on the
	// same side as the hand that switches weapons. It is a readout, so sitting
	// inside the look zone costs nothing.
	Touch_RegisterCvar("touch_weplist_x", "0.962");
	Touch_RegisterCvar("touch_weplist_y", "0.430");
	Touch_RegisterCvar("touch_weplist_visible", "1");
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
	// 0 plain button, 1 button that latches when held, 2 auto-hop while moving.
	Touch_RegisterCvar("touch_hop_mode", "1");
	// How long the bar must be held before it latches. Long enough that a jump
	// never latches by accident, short enough to feel like a decision rather
	// than a wait. Below a hop cycle (~400 ms) so holding for two hops latches.
	Touch_RegisterCvar("touch_hop_latch_ms", "350");
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

	Touch_RegisterTransientCvar("touch_debug", "0");
	Touch_RegisterTransientCvar("touch_customize", "0");
	Touch_RegisterTransientCvar("touch_layout_file", "touch.layout.cfg");
	Touch_RegisterTransientCvar("touch_autoscreenshot", "0");
	// Registered here so a saved layout can carry it, but written by
	// Touch_WriteAllCvars rather than from the table — its value is the schema's,
	// not a preference, and resetting it would defeat the migration check.
	registercvar("touch_layout_version", "0");
}

void Touch_ResetDefaults(void)
{
	touch_def_force = true;
	Touch_DefineCvars();
	touch_def_force = false;
}

// Writes every persistent setting's *current* value as a seta line. Transient
// cvars are skipped by construction, since they are registered through the other
// door and a saved layout that restored them would resurrect a stale session.
void Touch_WriteAllCvars(float fh)
{
	fputs(fh, strcat("seta touch_layout_version ", ftos(TOUCH_LAYOUT_VERSION), "\n"));
	touch_def_fh = fh;
	Touch_DefineCvars();
	touch_def_fh = -1;
}

// A saved layout is exec'd last in the startup chain, so every seta in it
// outranks the defaults above — including settings the player never touched. A
// layout written before this redesign therefore pinned the old geometry forever,
// and no amount of changing a default here could be seen on a device that had
// ever pressed Save. Once the schema moves on, the stale file is backed up and
// the defaults are taken instead.
//
// This runs from Touch_EnsureReady, not Touch_Init: localcmd queues, so at init
// time the layout file has not been exec'd yet and its version is not readable.
void Touch_MigrateLayout(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	float saved = cvar("touch_layout_version");
	if (saved >= TOUCH_LAYOUT_VERSION)
		return;

	print("Touch: layout file is schema ", ftos(saved), ", this build wants ",
		ftos(TOUCH_LAYOUT_VERSION), " — backing it up and taking the new defaults.\n");
	Touch_SaveLayoutFile("touch.layout.bak");
	Touch_ResetDefaults();
	cvar_set("touch_layout_version", ftos(TOUCH_LAYOUT_VERSION));
	// Re-stating the chosen preset keeps a player who picked "competitive" on it.
	localcmd(strcat("exec touch/profiles/", autocvar_touch_preset, ".cfg\n"));
	Touch_SaveLayoutFile(autocvar_touch_layout_file);
}

void Touch_Init(void)
{
	Touch_DefineCvars();

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
	registercommand("touch_chat");

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

// Geometry for the *stock* strip, which is the fallback when the touch readouts
// are switched off. Touch_ApplyMobileHudEngineSettings decides which one draws.
void Touch_ApplyTouchWeaponsStrip(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	cvar_set("touch_weapon_visible", "0");
	cvar_set("hud_panel_weapons_onlyowned", "1");
	cvar_set("hud_panel_weapons_timeout", "0");
	cvar_set("hud_panel_weapons_label", "1");
	cvar_set("hud_panel_weapons_aspect", "0.9");
	localcmd("seta hud_panel_weapons_pos \"0.915 0.18\"\n");
	localcmd("seta hud_panel_weapons_size \"0.075 0.58\"\n");
	localcmd("seta hud_panel_weapons_bg 1\n");
	localcmd("seta hud_panel_weapons_bg_alpha 0.35\n");
}

// The touch readouts and the stock panels are alternatives, so decide between
// them here rather than trusting a shipped config to agree with a cvar default.
// Previously touch/xonotic.cfg disabled the stock panels and touch_mobile_hud
// defaulted off, which left the player with no health readout whatsoever.
void Touch_ApplyMobileHudEngineSettings(void)
{
	bool touch_hud = boolean(cvar("touch_mobile_hud"));
	cvar_set("hud_panel_healtharmor", touch_hud ? "0" : "1");
	cvar_set("hud_panel_ammo", touch_hud ? "0" : "1");
	// One switch for all the readouts rather than one per panel: they are the same
	// decision — does the touch layer own the HUD — and three knobs would let a
	// player end up with two weapon strips or none.
	cvar_set("hud_panel_weapons", touch_hud ? "0" : "1");
}

// The match clock keeps the centre of the top band now that the chrome pills
// have moved off it. Its logic (warmup, overtime, rounds, timeouts) stays in the
// stock panel rather than being reimplemented here — that would be a second copy
// of rules that already exist and would drift from them. Only its placement and
// weight are ours.
void Touch_ApplyTimerPanel(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	cvar_set("hud_panel_timer", "1");
	// Centred on the screen's vertical axis, at the very top edge: match state,
	// symmetric, and clear of both the vitals on the left and the scores on the
	// right (rule of thirds — one thing per third).
	localcmd("seta hud_panel_timer_pos \"0.440 0.004\"\n");
	localcmd("seta hud_panel_timer_size \"0.120 0.052\"\n");
	// The stock plate is fully opaque black with hard corners, which is exactly
	// the look the touch overlay moved away from. Matching the weapons strip's
	// alpha lets it read as the same family without losing the contrast that
	// keeps white numerals legible over a bright skybox.
	localcmd("seta hud_panel_timer_bg_alpha 0.35\n");
}

// The kill feed shipped in the bottom-right corner, which on this layout is
// exactly where FIRE, HOP and DUCK live — every frag printed a line of text
// across the controls. It moves to the right band under the chrome pills: the
// placement players already expect from other shooters (Jakob), above the
// weapons strip and clear of every control.
void Touch_ApplyNotifyPanel(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	cvar_set("hud_panel_notify", "1");
	localcmd("seta hud_panel_notify_pos \"0.620 0.140\"\n");
	localcmd("seta hud_panel_notify_size \"0.360 0.190\"\n");
	localcmd("seta hud_panel_notify_bg 0\n");
	// Newest at the top, next to where the eye already goes for the scores.
	cvar_set("hud_panel_notify_flip", "1");
	// Long enough to read while moving, short enough that the band is usually
	// empty: this sits over the look zone, and stale text there is just noise.
	cvar_set("hud_panel_notify_time", "8");
	cvar_set("hud_panel_notify_fadetime", "1");
}

// The transient chat feed is drawn by the engine, and the stock HUD_Chat panel
// translates its own pos/size into con_chatrect for it. So the feed is fixed
// with geometry rather than code — but it does have to be told where to go:
// it shipped at 2.7 mm type, bottom-left, directly under the move stick, where
// the player's own thumb covered it.
void Touch_ApplyChatFeedSettings(void)
{
	if (!cvar("vid_touchscreen"))
		return;
	cvar_set("hud_panel_chat", "1");
	// Left band above the move stick: outside the look zone (x < 0.32) and above
	// the move zone (y < 0.42), so the feed sits in genuinely spare screen.
	localcmd("seta hud_panel_chat_pos \"0.020 0.150\"\n");
	localcmd("seta hud_panel_chat_size \"0.290 0.250\"\n");
	localcmd("seta hud_panel_chat_bg 0\n");
	// 14 console units is 4.7 mm on the 13" target, against the 8-unit default's
	// 2.7 mm. con_chat (line count) is derived from this by the stock panel.
	cvar_set("con_chatsize", "14");
	cvar_set("con_chattime", "12");
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
	Touch_ApplyTouchWeaponsStrip();
	Touch_ApplySensitivity();
	// Anything *derived* from a touch cvar is applied from Touch_EnsureReady
	// instead of here. localcmd queues, so the execs above have not run yet at
	// this point and would be read before they happened.
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
