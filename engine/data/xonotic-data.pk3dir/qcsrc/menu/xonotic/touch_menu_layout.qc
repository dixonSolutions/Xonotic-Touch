#include "touch_menu_layout.qh"

// What this tab shows. The in-game writer (Touch_SaveLayoutFile) saves the
// whole table in client/touch_init.qc, which the menu cannot see, so the
// rest of the keys come from the saved layout itself: see TouchMenu_LayoutKeys.
const string TOUCHMENU_KEYS =
	"touch_preset touch_scale touch_opacity touch_glass_hollow "
	"touch_move_x touch_move_y touch_move_size "
	"touch_fire_x touch_fire_y touch_fire_size "
	"touch_jump_x touch_jump_y touch_jump_size "
	"touch_crouch_x touch_crouch_y touch_crouch_size "
	"touch_weapon_x touch_weapon_y touch_weapon_size "
	"touch_zoom_x touch_zoom_y touch_zoom_size "
	"touch_dodge_x touch_dodge_y touch_dodge_size touch_dodge_visible "
	"touch_look_zone_left touch_look_zone_right touch_edge_deadzone_px touch_handedness "
	"touch_sens_base touch_sens_y_mult touch_invert_y touch_look_smoothing touch_look_deadzone_px "
	"touch_stick_deadzone touch_stick_range touch_aim_assist "
	"touch_fire_mode touch_zoom_mode touch_weapon_mode "
	"touch_weapon_slot1 touch_weapon_slot2 touch_weapon_slot3 touch_auto_fire "
	"cl_touch_autoshoot cl_touch_aimassist cl_touch_aimassist_friction cl_touch_aimassist_strength "
	"touch_dodge_mode touch_scoreboard_gesture touch_chat_mode touch_minimal_more "
	"touch_hud_scale touch_performance_profile touch_setup_done";

// TOUCHMENU_KEYS, then every other touch setting the saved layout carries,
// each once.
//
// Writing this tab's keys alone over a layout the game saved threw away
// everything else in it -- the look speed cap, smoothing, ALT's position --
// at the next launch.
string TouchMenu_LayoutKeys(void)
{
	// Padded so " key " matches whole names only.
	string keys = strcat(" ", TOUCHMENU_KEYS, " ");
	float fh = fopen("touch.layout.cfg", FILE_READ);
	if (fh >= 0)
	{
		string line;
		while ((line = fgets(fh)))
		{
			float n = tokenize_console(line);
			float k = (argv(0) == "seta" || argv(0) == "set") ? 1 : 0;
			if (n < k + 2)
				continue;
			string key = argv(k);
			if (substring(key, 0, 6) != "touch_" && substring(key, 0, 9) != "cl_touch_")
				continue;
			// Written separately, last.
			if (key == "touch_layout_version")
				continue;
			if (strstrofs(keys, strcat(" ", key, " "), 0) < 0)
				keys = strcat(keys, key, " ");
		}
		fclose(fh);
	}
	return keys;
}

void TouchMenu_WriteLayoutFile(string path)
{
	// Before fopen: path may be the layout itself, and opening it truncates.
	string keys = TouchMenu_LayoutKeys();
	float fh = fopen(path, FILE_WRITE);
	if (fh < 0)
		return;

	fputs(fh, "// Xonotic UT touch layout — saved from menu\n");
	// Live values, as seta like the in-game writer's, so a loaded layout is
	// archived like any other setting. The launcher exec'd the saved layout
	// at startup, so a key this tab does not show still holds what was saved
	// there, or what the game has changed since.
	//
	// A key with no cvar yet is skipped: before the first match nothing has
	// registered it, and a `seta key ""` would outlive the default the game
	// registers later, since registering never overwrites a cvar that exists.
	float n = tokenize_console(keys);
	for (float i = 0; i < n; ++i)
	{
		if (cvar_type(argv(i)) & CVAR_TYPEFLAG_EXISTS)
			fputs(fh, strcat("seta ", argv(i), " \"", cvar_string(argv(i)), "\"\n"));
	}
	// The schema the CSQC checks a layout against (Touch_MigrateLayout).
	// Without it every file saved here read as schema 0, and the next match
	// backed it up and reset to the defaults.
	fputs(fh, strcat("seta touch_layout_version ", ftos(cvar("touch_layout_version")), "\n"));
	fclose(fh);
}
