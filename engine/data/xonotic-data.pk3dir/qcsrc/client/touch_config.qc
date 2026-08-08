#include "touch_config.qh"
#include "touch_chat.qh"
#include "touch_pause.qh"
#include "touch_scores.qh"
#include "touch_init.qh"
#include "../dpdefs/ut_touchfinger.qc"

float touch_cmd_argc;

// Walks the same table Touch_Init registers from, writing each setting's
// current value. It used to be a hand-maintained list of ~100 fputs calls that
// had to agree with the registration list and with the profile files; it did
// not, and the disagreement was invisible until a default stopped taking
// effect.
void Touch_SaveLayoutFile(string path)
{
	float fh = fopen(path, FILE_WRITE);
	if (fh < 0)
	{
		print("Touch: could not write ", path, "\n");
		return;
	}

	fputs(fh, "// Xonotic Touch layout — saved by touch_save (seta for archive)\n");
	Touch_WriteAllCvars(fh);
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

// Reset to the code defaults first, then let the profile state only what it
// changes. Without the reset, switching from a profile that enables something to
// one that does not would leave the setting enabled, because a profile can only
// override what it mentions.
void Touch_ApplyPreset(string name)
{
	if (name == "")
		return;
	Touch_ResetDefaults();
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
	if (s == "touch_pause")
	{
		Touch_Pause_Enter();
		return true;
	}
	if (s == "touch_pause_exit")
	{
		Touch_Pause_Exit();
		return true;
	}
	// The sheet is reachable by pressing the CHAT pill, but a command is what
	// lets a player bind it to a hardware key when the tablet has a keyboard
	// attached, and it is the only way to reach it from the console.
	if (s == "touch_chat")
	{
		Touch_Chat_Open(argc >= 2 && argv(1) == "team");
		return true;
	}
	// Deliberately a toggle rather than a pass-through to +showscores: this is the
	// same action the pill performs, so a bound key and the pill cannot disagree
	// about whether the board is up.
	if (s == "touch_scores")
	{
		Touch_Scores_Toggle();
		return true;
	}
	return false;
}
