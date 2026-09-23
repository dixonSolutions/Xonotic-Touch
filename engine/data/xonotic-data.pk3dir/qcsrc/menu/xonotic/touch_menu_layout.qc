#include "touch_menu_layout.qh"

void TouchMenu_ConfigWriteFloat(float fh, string key)
{
	fputs(fh, strcat(key, " ", ftos(cvar(key)), "\n"));
}

void TouchMenu_ConfigWriteString(float fh, string key)
{
	fputs(fh, strcat(key, " \"", cvar_string(key), "\"\n"));
}

void TouchMenu_WriteLayoutFile(string path)
{
	float fh = fopen(path, FILE_WRITE);
	if (fh < 0)
		return;

	fputs(fh, "// Xonotic UT touch layout — saved from menu\n");
	TouchMenu_ConfigWriteString(fh, "touch_preset");
	TouchMenu_ConfigWriteFloat(fh, "touch_scale");
	TouchMenu_ConfigWriteFloat(fh, "touch_opacity");
	TouchMenu_ConfigWriteFloat(fh, "touch_move_x");
	TouchMenu_ConfigWriteFloat(fh, "touch_move_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_move_size");
	TouchMenu_ConfigWriteFloat(fh, "touch_fire_x");
	TouchMenu_ConfigWriteFloat(fh, "touch_fire_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_fire_size");
	TouchMenu_ConfigWriteFloat(fh, "touch_jump_x");
	TouchMenu_ConfigWriteFloat(fh, "touch_jump_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_jump_size");
	TouchMenu_ConfigWriteFloat(fh, "touch_crouch_x");
	TouchMenu_ConfigWriteFloat(fh, "touch_crouch_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_crouch_size");
	TouchMenu_ConfigWriteFloat(fh, "touch_weapon_x");
	TouchMenu_ConfigWriteFloat(fh, "touch_weapon_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_weapon_size");
	TouchMenu_ConfigWriteFloat(fh, "touch_zoom_x");
	TouchMenu_ConfigWriteFloat(fh, "touch_zoom_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_zoom_size");
	TouchMenu_ConfigWriteFloat(fh, "touch_dodge_x");
	TouchMenu_ConfigWriteFloat(fh, "touch_dodge_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_dodge_size");
	TouchMenu_ConfigWriteFloat(fh, "touch_dodge_visible");
	TouchMenu_ConfigWriteFloat(fh, "touch_look_zone_left");
	TouchMenu_ConfigWriteFloat(fh, "touch_look_zone_right");
	TouchMenu_ConfigWriteFloat(fh, "touch_edge_deadzone_px");
	TouchMenu_ConfigWriteFloat(fh, "touch_handedness");
	TouchMenu_ConfigWriteFloat(fh, "touch_sens_base");
	TouchMenu_ConfigWriteFloat(fh, "touch_sens_y_mult");
	TouchMenu_ConfigWriteFloat(fh, "touch_invert_y");
	TouchMenu_ConfigWriteFloat(fh, "touch_look_smoothing");
	TouchMenu_ConfigWriteFloat(fh, "touch_look_deadzone_px");
	TouchMenu_ConfigWriteFloat(fh, "touch_stick_deadzone");
	TouchMenu_ConfigWriteFloat(fh, "touch_stick_range");
	TouchMenu_ConfigWriteFloat(fh, "touch_aim_assist");
	TouchMenu_ConfigWriteFloat(fh, "touch_fire_mode");
	TouchMenu_ConfigWriteFloat(fh, "touch_zoom_mode");
	TouchMenu_ConfigWriteFloat(fh, "touch_weapon_mode");
	TouchMenu_ConfigWriteString(fh, "touch_weapon_slot1");
	TouchMenu_ConfigWriteString(fh, "touch_weapon_slot2");
	TouchMenu_ConfigWriteString(fh, "touch_weapon_slot3");
	TouchMenu_ConfigWriteFloat(fh, "touch_auto_fire");
	TouchMenu_ConfigWriteFloat(fh, "cl_touch_autoshoot");
	TouchMenu_ConfigWriteFloat(fh, "cl_touch_aimassist");
	TouchMenu_ConfigWriteFloat(fh, "cl_touch_aimassist_friction");
	TouchMenu_ConfigWriteFloat(fh, "cl_touch_aimassist_strength");
	TouchMenu_ConfigWriteFloat(fh, "touch_dodge_mode");
	TouchMenu_ConfigWriteFloat(fh, "touch_scoreboard_gesture");
	TouchMenu_ConfigWriteFloat(fh, "touch_chat_mode");
	TouchMenu_ConfigWriteFloat(fh, "touch_minimal_more");
	TouchMenu_ConfigWriteFloat(fh, "touch_hud_scale");
	TouchMenu_ConfigWriteString(fh, "touch_performance_profile");
	TouchMenu_ConfigWriteFloat(fh, "touch_setup_done");
	fclose(fh);
}
