/*
Copyright (C) 2026 Xonotic Touch contributors

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

#include "quakedef.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#if defined(__linux__) && !defined(__ANDROID__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#endif

// Linux input bits (from linux/input-event-codes.h) — avoid depending on kernel headers.
#define VID_KEY_A                 30
#define VID_ABS_MT_POSITION_X     53
#define VID_INPUT_PROP_DIRECT     1

static qbool vid_touch_applying;
static qbool vid_touch_finger_seen;

extern cvar_t vid_touchscreen;
extern cvar_t vid_touchscreen_mode;
extern cvar_t vid_touchscreen_touchonly;
extern cvar_t vid_touchscreen_detected;
extern cvar_t vid_touchscreen_touchonly_detected;

qbool VID_SDL_HasTouchDevices(void);

static qbool vid_strcasestr_has(const char *hay, const char *needle)
{
	size_t nlen, hlen, i, j;

	if (!hay || !needle || !needle[0])
		return false;
	nlen = strlen(needle);
	hlen = strlen(hay);
	if (nlen > hlen)
		return false;
	for (i = 0; i + nlen <= hlen; i++)
	{
		for (j = 0; j < nlen; j++)
		{
			if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
				break;
		}
		if (j == nlen)
			return true;
	}
	return false;
}

static qbool vid_name_is_ignored_keyboard(const char *name)
{
	if (!name || !name[0])
		return true;
	if (vid_strcasestr_has(name, "power button"))
		return true;
	if (vid_strcasestr_has(name, "sleep button"))
		return true;
	if (vid_strcasestr_has(name, "lid switch"))
		return true;
	if (vid_strcasestr_has(name, "video bus"))
		return true;
	if (vid_strcasestr_has(name, "gpio-keys"))
		return true;
	if (vid_strcasestr_has(name, "headset"))
		return true;
	if (vid_strcasestr_has(name, "hdmi"))
		return true;
	if (vid_strcasestr_has(name, "sof-hda"))
		return true;
	if (vid_strcasestr_has(name, "consumer control"))
		return true;
	// Not a keyboard anyone types on: the switch device itself, and the
	// virtual keyboards that remappers (keyd, ydotool, xdotool, uinput
	// tools, remote desktop) keep permanently plugged in.
	if (vid_strcasestr_has(name, "tablet mode"))
		return true;
	if (vid_strcasestr_has(name, "keyd"))
		return true;
	if (vid_strcasestr_has(name, "virtual"))
		return true;
	if (vid_strcasestr_has(name, "uinput"))
		return true;
	if (vid_strcasestr_has(name, "ydotool"))
		return true;
	if (vid_strcasestr_has(name, "xdotool"))
		return true;
	if (vid_strcasestr_has(name, "wlroots"))
		return true;
	if (vid_strcasestr_has(name, "remote desktop"))
		return true;
	return false;
}

// SW_TABLET_MODE: a detachable or convertible reports whether its keyboard is
// folded away. The keyboard stays listed in /proc while it is, so the switch
// has to win. Needs /dev/input readable (Flatpak: --device=input).
//
// Opening every /dev/input node costs about 0.4 s on a Surface (the IPTS
// virtual devices are slow to open) -- a visible hitch when done per poll.
// The switch fds are found once and kept; they are re-found only when the
// procfs device list changes (vid_tablet_switch_invalidate). Reading their
// state is one ioctl each, well under a microsecond.
#define VID_MAX_SWITCH_FDS 8
static int vid_switch_fds[VID_MAX_SWITCH_FDS];
static int vid_switch_nfds = -1; // -1: never scanned

static void vid_tablet_switch_invalidate(void)
{
#if defined(__linux__) && !defined(__ANDROID__)
	int i;
	for (i = 0; i < vid_switch_nfds; i++)
		close(vid_switch_fds[i]);
#endif
	vid_switch_nfds = -1;
}

static void vid_tablet_switch_rescan(void)
{
	vid_tablet_switch_invalidate();
	vid_switch_nfds = 0;
#if defined(__linux__) && !defined(__ANDROID__)
	{
		DIR *dir;
		struct dirent *ent;
		dir = opendir("/dev/input");
		if (!dir)
			return;
		while ((ent = readdir(dir)) != NULL)
		{
			char path[64];
			int fd;
			unsigned long caps[(SW_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
			if (strncmp(ent->d_name, "event", 5))
				continue;
			dpsnprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
			fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0)
				continue;
			memset(caps, 0, sizeof(caps));
			if (vid_switch_nfds < VID_MAX_SWITCH_FDS
				&& ioctl(fd, EVIOCGBIT(EV_SW, sizeof(caps)), caps) >= 0
				&& (caps[SW_TABLET_MODE / (8 * sizeof(long))] & (1UL << (SW_TABLET_MODE % (8 * sizeof(long))))))
				vid_switch_fds[vid_switch_nfds++] = fd;
			else
				close(fd);
		}
		closedir(dir);
	}
#endif
}

static qbool vid_read_tablet_mode(qbool *found)
{
	*found = false;
	if (vid_switch_nfds < 0)
		vid_tablet_switch_rescan();
	*found = vid_switch_nfds > 0;
#if defined(__linux__) && !defined(__ANDROID__)
	{
		int i;
		for (i = 0; i < vid_switch_nfds; i++)
		{
			unsigned long state[(SW_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
			memset(state, 0, sizeof(state));
			if (ioctl(vid_switch_fds[i], EVIOCGSW(sizeof(state)), state) >= 0
				&& (state[SW_TABLET_MODE / (8 * sizeof(long))] & (1UL << (SW_TABLET_MODE % (8 * sizeof(long))))))
				return true;
		}
	}
#endif
	return false;
}

// The whole of /proc/bus/input/devices: about 0.1 ms to read, and its text
// changing is the only reason to walk /dev/input again.
static size_t vid_read_proc_bus_input(char *buf, size_t size)
{
	FILE *f;
	size_t n = 0;
	buf[0] = 0;
	f = fopen("/proc/bus/input/devices", "r");
	if (!f)
		return 0;
	n = fread(buf, 1, size - 1, f);
	buf[n] = 0;
	fclose(f);
	return n;
}

// Parse space-separated hex capability bitmaps from /proc/bus/input/devices.
// Words are printed most-significant first.
static qbool vid_bitmap_has_bit(const char *hex, unsigned bit)
{
	unsigned long words[32];
	int n = 0;
	int word_bits = 32;
	int idx;
	unsigned word_from_low;
	unsigned bit_in_word;
	const char *p = hex;
	char *end;

	memset(words, 0, sizeof(words));
	while (p && *p && n < 32)
	{
		const char *start;

		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;
		start = p;
		words[n] = strtoul(p, &end, 16);
		if (end == p)
			break;
		if ((int)(end - start) > 8)
			word_bits = 64;
		n++;
		p = end;
	}
	if (n == 0)
		return false;

	word_from_low = bit / (unsigned)word_bits;
	bit_in_word = bit % (unsigned)word_bits;
	idx = n - 1 - (int)word_from_low;
	if (idx < 0 || idx >= n)
		return false;
	return (words[idx] & (1UL << bit_in_word)) != 0;
}

// Linux input bus ids that mean "plugged in by the player".
#define VID_BUS_USB        0x03
#define VID_BUS_BLUETOOTH  0x05

static void vid_scan_proc_bus_input(qbool *has_touch, qbool *has_keyboard, qbool *has_external_keyboard)
{
	FILE *f;
	char line[512];
	char name[256];
	unsigned prop = 0;
	unsigned bus = 0;
	qbool key_a = false;
	qbool abs_mt = false;

	name[0] = 0;
	f = fopen("/proc/bus/input/devices", "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f))
	{
		if (!strncmp(line, "I: Bus=", 7))
			bus = (unsigned)strtoul(line + 7, NULL, 16);
		else if (!strncmp(line, "N: Name=\"", 9))
		{
			name[0] = 0;
			sscanf(line, "N: Name=\"%255[^\"]\"", name);
		}
		else if (!strncmp(line, "B: PROP=", 8))
			prop = (unsigned)strtoul(line + 8, NULL, 16);
		else if (!strncmp(line, "B: KEY=", 7))
			key_a = vid_bitmap_has_bit(line + 7, VID_KEY_A);
		else if (!strncmp(line, "B: ABS=", 7))
			abs_mt = vid_bitmap_has_bit(line + 7, VID_ABS_MT_POSITION_X);
		else if (line[0] == '\n' || line[0] == '\r' || line[0] == 0)
		{
			if ((prop & (1u << VID_INPUT_PROP_DIRECT)) || vid_strcasestr_has(name, "touchscreen"))
				*has_touch = true;
			else if (abs_mt && !(prop & 1u) && vid_strcasestr_has(name, "touch"))
				*has_touch = true;
			if (key_a && !vid_name_is_ignored_keyboard(name))
			{
				*has_keyboard = true;
				if (has_external_keyboard && (bus == VID_BUS_USB || bus == VID_BUS_BLUETOOTH))
					*has_external_keyboard = true;
			}
			name[0] = 0;
			prop = 0;
			bus = 0;
			key_a = false;
			abs_mt = false;
		}
	}
	fclose(f);
}

static int vid_read_chassis_type(void)
{
	FILE *f;
	int type = -1;

	f = fopen("/sys/class/dmi/id/chassis_type", "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", &type) != 1)
		type = -1;
	fclose(f);
	return type;
}

static qbool vid_os_release_has(const char *needle)
{
	FILE *f;
	char line[512];

	f = fopen("/etc/os-release", "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f))
	{
		if (vid_strcasestr_has(line, needle))
		{
			fclose(f);
			return true;
		}
	}
	fclose(f);
	return false;
}

static qbool vid_is_ubuntu_touch(void)
{
	const char *desktop;

	if (vid_os_release_has("Ubuntu Touch") || vid_os_release_has("UBUNTU_TOUCH")
		|| vid_os_release_has("VARIANT_ID=touch") || vid_os_release_has("lomiri"))
		return true;
#ifndef WIN32
	if (access("/usr/share/ubports", F_OK) == 0)
		return true;
#endif
	desktop = getenv("XDG_CURRENT_DESKTOP");
	if (desktop && (vid_strcasestr_has(desktop, "Lomiri") || vid_strcasestr_has(desktop, "Unity8")))
		return true;
	if (getenv("CLICK_FRAMEWORK") && getenv("CLICK_FRAMEWORK")[0])
		return true;
	return false;
}

static qbool vid_chassis_is_handheld_or_tablet(int chassis)
{
	// SMBIOS chassis: 11 Hand Held, 30 Tablet.
	// 31 Convertible / 32 Detachable still have a keyboard when docked.
	return chassis == 11 || chassis == 30;
}

static void vid_detect_hardware(qbool *has_touchscreen, qbool *has_keyboard, qbool *tablet_mode)
{
	qbool touch = false;
	qbool keyboard = false;
	qbool external = false;
	qbool switch_found = false;
	qbool tablet = false;
	int chassis;

	if (VID_SDL_HasTouchDevices() || vid_touch_finger_seen)
		touch = true;

	vid_scan_proc_bus_input(&touch, &keyboard, &external);
	tablet = vid_read_tablet_mode(&switch_found);
	// A tablet-mode switch saying "tablet" overrides the built-in keyboard
	// (a folded-back Type Cover stays listed). A USB or Bluetooth keyboard
	// is not part of the chassis, so it counts whatever the switch says.
	if (switch_found && tablet)
		keyboard = external;

	chassis = vid_read_chassis_type();
	if (vid_chassis_is_handheld_or_tablet(chassis) && touch)
		keyboard = external;
	if (vid_is_ubuntu_touch())
	{
		touch = true;
		keyboard = external;
	}

#ifdef DP_MOBILETOUCH
	touch = true;
	keyboard = false;
#endif

	*has_touchscreen = touch;
	*has_keyboard = keyboard;
	*tablet_mode = switch_found && tablet;
}

void VID_DetectTouchHardware(qbool *has_touchscreen, qbool *is_touch_only)
{
	qbool touch = false;
	qbool keyboard = false;
	qbool tablet = false;

	vid_detect_hardware(&touch, &keyboard, &tablet);
	*has_touchscreen = touch;
	*is_touch_only = touch && !keyboard;
}

// ---------------------------------------------------------------------------
// Live hot-plug: keyboards come and go while the game runs (a Type Cover
// clicks on, a Bluetooth keyboard pairs, a convertible folds). SDL2 has no
// keyboard hot-plug event, so re-read the hardware once a second and re-apply
// the mode when the answer changes. In Auto mode that shows or hides the
// on-screen controls in place, in menus and mid-match, and says so.

static double vid_touch_hotplug_next;
static qbool vid_touch_hotplug_init;
static qbool vid_touch_hotplug_keyboard;
static qbool vid_touch_hotplug_touch;
#define VID_PROC_INPUT_MAX 16384
static char vid_touch_hotplug_proc[VID_PROC_INPUT_MAX];

void VID_TouchHotplugFrame(void)
{
	qbool touch, keyboard, tablet;
	qbool was_on, is_on;
	const char *msg = NULL;
	static char proc[VID_PROC_INPUT_MAX];

#ifdef DP_MOBILETOUCH
	return;
#endif
	if (host.realtime < vid_touch_hotplug_next)
		return;
	vid_touch_hotplug_next = host.realtime + 1.0;

	// A device came or went: the switch fds may have moved, re-find them.
	// Otherwise the per-second work is this read plus one ioctl per switch.
	vid_read_proc_bus_input(proc, sizeof(proc));
	if (strcmp(proc, vid_touch_hotplug_proc))
	{
		memcpy(vid_touch_hotplug_proc, proc, sizeof(vid_touch_hotplug_proc));
		vid_tablet_switch_invalidate();
	}

	vid_detect_hardware(&touch, &keyboard, &tablet);
	if (!vid_touch_hotplug_init)
	{
		vid_touch_hotplug_init = true;
		vid_touch_hotplug_keyboard = keyboard;
		vid_touch_hotplug_touch = touch;
		Con_Printf("Input hot-plug: startup keyboard=%d touch=%d tablet_mode=%d, touch controls %s\n",
			keyboard, touch, tablet, vid_touchscreen.integer ? "on" : "off");
		return;
	}
	if (keyboard == vid_touch_hotplug_keyboard && touch == vid_touch_hotplug_touch)
		return;

	was_on = vid_touchscreen.integer != 0;
	VID_ApplyTouchscreenMode();
	is_on = vid_touchscreen.integer != 0;

	if (keyboard != vid_touch_hotplug_keyboard)
	{
		if (keyboard)
			msg = (was_on && !is_on) ? "Keyboard connected: touch controls hidden" : "Keyboard connected";
		else
			msg = (!was_on && is_on) ? "Keyboard disconnected: touch controls shown" : "Keyboard disconnected";
	}
	else if (touch != vid_touch_hotplug_touch)
	{
		if (touch)
			msg = (!was_on && is_on) ? "Touchscreen detected: touch controls shown" : "Touchscreen detected";
		else
			msg = "Touchscreen removed";
	}
	vid_touch_hotplug_keyboard = keyboard;
	vid_touch_hotplug_touch = touch;

	Con_Printf("Input hot-plug: keyboard=%d touch=%d tablet_mode=%d -> touch controls %s\n",
		keyboard, touch, tablet, is_on ? "on" : "off");
	if (msg)
		SCR_Toast(msg);
}

void VID_ApplyTouchscreenMode(void)
{
	qbool has_touch = false;
	qbool touch_only = false;
	int mode;
	int want;

	VID_DetectTouchHardware(&has_touch, &touch_only);

	vid_touch_applying = true;
	Cvar_SetValueQuick(&vid_touchscreen_detected, has_touch ? 1 : 0);
	Cvar_SetValueQuick(&vid_touchscreen_touchonly_detected, touch_only ? 1 : 0);

	mode = vid_touchscreen_mode.integer;
	if (mode <= 0)
		want = 0;
	else if (mode >= 2)
		want = 1;
	else if (vid_touchscreen_touchonly.integer)
		want = touch_only ? 1 : 0;
	else
		want = has_touch ? 1 : 0;

	if (vid_touchscreen.integer != want)
	{
		Cvar_SetValueQuick(&vid_touchscreen, want);
		Con_Printf("Touch controls: %s (mode=%s, hardware touch=%s, touch-only=%s)\n",
			want ? "on" : "off",
			mode <= 0 ? "off" : (mode >= 2 ? "always" : "auto"),
			has_touch ? "yes" : "no",
			touch_only ? "yes" : "no");
	}
	vid_touch_applying = false;
}

void VID_NoteTouchFingerSeen(void)
{
	if (vid_touch_finger_seen)
		return;
	vid_touch_finger_seen = true;
	if (vid_touchscreen_mode.integer == 1 && !vid_touchscreen.integer)
		VID_ApplyTouchscreenMode();
}

void VID_TouchscreenMode_c(cvar_t *var)
{
	(void)var;
	if (vid_touch_applying)
		return;
	VID_ApplyTouchscreenMode();
}

void VID_Touchscreen_c(cvar_t *var)
{
	if (vid_touch_applying)
		return;
	// Direct console / config writes to vid_touchscreen become an explicit mode.
	vid_touch_applying = true;
	if (var->integer)
		Cvar_SetValueQuick(&vid_touchscreen_mode, 2);
	else
		Cvar_SetValueQuick(&vid_touchscreen_mode, 0);
	vid_touch_applying = false;
}

void VID_TouchscreenRescan_f(cmd_state_t *cmd)
{
	(void)cmd;
	VID_ApplyTouchscreenMode();
	Con_Printf("vid_touchscreen_mode %d, vid_touchscreen %d, detected %d, touch-only %d\n",
		vid_touchscreen_mode.integer,
		vid_touchscreen.integer,
		vid_touchscreen_detected.integer,
		vid_touchscreen_touchonly_detected.integer);
}
