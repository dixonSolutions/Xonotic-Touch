#include "touch_config.qh"
#include "touch_init.qh"

float touch_cmd_argc;

void Touch_ConfigWriteFloat(float fh, string key)
{
	fputs(fh, strcat(key, " ", ftos(cvar(key)), "\n"));
}

void Touch_ConfigWriteString(float fh, string key)
{
	fputs(fh, strcat(key, " \"", cvar_string(key), "\"\n"));
}

void Touch_SaveLayoutFile(string path)
{
	float fh = fopen(path, FILE_WRITE);
	if (fh < 0)
	{
		print("Touch: could not write ", path, "\n");
		return;
	}

	fputs(fh, "// Xonotic UT touch layout — saved by touch_save\n");

	Touch_ConfigWriteString(fh, "touch_preset");
	Touch_ConfigWriteFloat(fh, "touch_scale");
	Touch_ConfigWriteFloat(fh, "touch_opacity");
	Touch_ConfigWriteFloat(fh, "touch_move_x");
	Touch_ConfigWriteFloat(fh, "touch_move_y");
	Touch_ConfigWriteFloat(fh, "touch_move_size");
	Touch_ConfigWriteFloat(fh, "touch_fire_x");
	Touch_ConfigWriteFloat(fh, "touch_fire_y");
	Touch_ConfigWriteFloat(fh, "touch_fire_size");
	Touch_ConfigWriteFloat(fh, "touch_jump_x");
	Touch_ConfigWriteFloat(fh, "touch_jump_y");
	Touch_ConfigWriteFloat(fh, "touch_jump_size");
	Touch_ConfigWriteFloat(fh, "touch_crouch_x");
	Touch_ConfigWriteFloat(fh, "touch_crouch_y");
	Touch_ConfigWriteFloat(fh, "touch_crouch_size");
	Touch_ConfigWriteFloat(fh, "touch_weapon_x");
	Touch_ConfigWriteFloat(fh, "touch_weapon_y");
	Touch_ConfigWriteFloat(fh, "touch_weapon_size");
	Touch_ConfigWriteFloat(fh, "touch_zoom_x");
	Touch_ConfigWriteFloat(fh, "touch_zoom_y");
	Touch_ConfigWriteFloat(fh, "touch_zoom_size");
	Touch_ConfigWriteFloat(fh, "touch_dodge_x");
	Touch_ConfigWriteFloat(fh, "touch_dodge_y");
	Touch_ConfigWriteFloat(fh, "touch_dodge_size");
	Touch_ConfigWriteFloat(fh, "touch_dodge_visible");
	Touch_ConfigWriteFloat(fh, "touch_reload_x");
	Touch_ConfigWriteFloat(fh, "touch_reload_y");
	Touch_ConfigWriteFloat(fh, "touch_reload_size");
	Touch_ConfigWriteFloat(fh, "touch_mobile_hud");
	Touch_ConfigWriteFloat(fh, "touch_hud_x");
	Touch_ConfigWriteFloat(fh, "touch_hud_y");
	Touch_ConfigWriteFloat(fh, "touch_hud_maxhealth");
	Touch_ConfigWriteFloat(fh, "touch_hud_maxarmor");
	Touch_ConfigWriteFloat(fh, "touch_ammo_x");
	Touch_ConfigWriteFloat(fh, "touch_ammo_y");
	Touch_ConfigWriteFloat(fh, "touch_look_zone_left");
	Touch_ConfigWriteFloat(fh, "touch_look_zone_right");
	Touch_ConfigWriteFloat(fh, "touch_edge_deadzone_px");
	Touch_ConfigWriteFloat(fh, "touch_handedness");
	Touch_ConfigWriteFloat(fh, "touch_sens_base");
	Touch_ConfigWriteFloat(fh, "touch_sens_y_mult");
	Touch_ConfigWriteFloat(fh, "touch_invert_y");
	Touch_ConfigWriteFloat(fh, "touch_look_smoothing");
	Touch_ConfigWriteFloat(fh, "touch_look_deadzone_px");
	Touch_ConfigWriteFloat(fh, "touch_stick_deadzone");
	Touch_ConfigWriteFloat(fh, "touch_stick_range");
	Touch_ConfigWriteFloat(fh, "touch_aim_assist");
	Touch_ConfigWriteFloat(fh, "touch_fire_mode");
	Touch_ConfigWriteFloat(fh, "touch_zoom_mode");
	Touch_ConfigWriteFloat(fh, "touch_weapon_mode");
	Touch_ConfigWriteString(fh, "touch_weapon_slot1");
	Touch_ConfigWriteString(fh, "touch_weapon_slot2");
	Touch_ConfigWriteString(fh, "touch_weapon_slot3");
	Touch_ConfigWriteFloat(fh, "touch_auto_fire");
	Touch_ConfigWriteFloat(fh, "touch_dodge_mode");
	Touch_ConfigWriteFloat(fh, "touch_scoreboard_gesture");
	Touch_ConfigWriteFloat(fh, "touch_chat_mode");
	Touch_ConfigWriteFloat(fh, "touch_minimal_more");
	Touch_ConfigWriteFloat(fh, "touch_hud_scale");
	Touch_ConfigWriteString(fh, "touch_performance_profile");
	Touch_ConfigWriteFloat(fh, "touch_setup_done");

	fclose(fh);
}

void Touch_ExportLayout(void)
{
	Touch_SaveLayoutFile("touch_export.cfg");
	print("Touch: exported to touch_export.cfg\n");
}

void Touch_ImportLayout(string path)
{
	if (path == "")
		path = autocvar_touch_layout_file;
	Touch_SaveLayoutFile("touch.layout.bak");
	localcmd(strcat("exec ", path, "\n"));
	Touch_ApplySensitivity();
	print("Touch: imported ", path, "\n");
}

void Touch_ApplyPreset(string name)
{
	if (name == "")
		return;
	localcmd(strcat("exec touch/profiles/", name, ".cfg\n"));
	cvar_set("touch_preset", name);
	Touch_ApplySensitivity();
}

void Touch_ResetPreset(void)
{
	Touch_ApplyPreset(cvar_string("touch_preset"));
}

void Touch_CmdSave(void)
{
	Touch_SaveLayoutFile(autocvar_touch_layout_file);
	print("Touch: saved to ", autocvar_touch_layout_file, "\n");
}

void Touch_CmdExport(void)
{
	Touch_ExportLayout();
}

void Touch_CmdImport(void)
{
	Touch_ImportLayout("");
}

void Touch_CmdApplyPreset(void)
{
	if (touch_cmd_argc >= 2)
		Touch_ApplyPreset(argv(1));
}

void Touch_CmdCustomize(void)
{
	float v = 1;
	if (touch_cmd_argc >= 2)
		v = stof(argv(1));
	else if (autocvar_touch_customize)
		v = 0;
	cvar_set("touch_customize", ftos(v));
}

void Touch_CmdCustomizeSave(void)
{
	Touch_CmdSave();
	cvar_set("touch_customize", "0");
}

void Touch_CmdCustomizeCancel(void)
{
	cvar_set("touch_customize", "0");
}

bool Touch_ConsoleCommand(string s, int argc)
{
	if (s == "touch_save")
	{
		Touch_CmdSave();
		return true;
	}
	if (s == "touch_export")
	{
		Touch_CmdExport();
		return true;
	}
	if (s == "touch_import")
	{
		if (argc >= 2)
			Touch_ImportLayout(argv(1));
		else
			Touch_CmdImport();
		return true;
	}
	if (s == "touch_apply_preset")
	{
		Touch_CmdApplyPreset();
		return true;
	}
	if (s == "touch_reset_preset")
	{
		Touch_ResetPreset();
		return true;
	}
	if (s == "touch_customize")
	{
		Touch_CmdCustomize();
		return true;
	}
	if (s == "touch_customize_save")
	{
		Touch_CmdCustomizeSave();
		return true;
	}
	if (s == "touch_customize_cancel")
	{
		Touch_CmdCustomizeCancel();
		return true;
	}
	return false;
}
