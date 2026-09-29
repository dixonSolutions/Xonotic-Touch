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

/*
What /proc/bus/input/devices says is attached: a touchscreen, a keyboard,
a mouse or touchpad, and the tablet-mode switch and screen pens worth
keeping a descriptor open on.

Only vid_touchdetect.c includes this in the engine. It is a header of plain
C (the includer supplies qbool, true and false) so that
scripts/test-input-presence.sh can feed it recorded listings without the
rest of the engine.
*/

#ifndef VID_INPUTSCAN_H
#define VID_INPUTSCAN_H

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

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

static void vid_scan_copy(char *dst, size_t size, const char *src)
{
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
}

// Linux input bits (linux/input-event-codes.h).
#define VID_KEY_A                 30
#define VID_KEY_Z                 44
#define VID_KEY_SPACE             57
#define VID_BTN_LEFT              0x110
#define VID_BTN_JOYSTICK          0x120
#define VID_BTN_GAMEPAD           0x130
#define VID_BTN_TOOL_PEN          0x140
#define VID_BTN_TOOL_FINGER       0x145
#define VID_REL_X                 0
#define VID_REL_Y                 1
#define VID_ABS_X                 0
#define VID_ABS_Y                 1
#define VID_ABS_MT_POSITION_X     53
#define VID_INPUT_PROP_POINTER    0
#define VID_INPUT_PROP_DIRECT     1
#define VID_SW_TABLET_MODE        1

// Linux input bus ids that mean "plugged in by the player".
#define VID_BUS_USB        0x03
#define VID_BUS_BLUETOOTH  0x05

// Devices that advertise keyboard or pointer bits but are nothing the player
// types or points with.
static qbool vid_name_is_ignored_device(const char *name)
{
	static const char *const ignored[] =
	{
		// Button sets and media controls that advertise KEY_A.
		"power button", "sleep button", "lid switch", "video bus", "gpio-keys",
		"headset", "hdmi", "sof-hda", "consumer control", "intel hid",
		"wmi hotkeys", "extra buttons",
		// The switch device itself.
		"tablet mode",
		// Virtual keyboards and pointers that remappers and automation (keyd,
		// ydotool, xdotool, uinput tools, remote desktop) keep permanently
		// plugged in.
		"keyd", "virtual", "uinput", "ydotool", "xdotool", "wlroots",
		"remote desktop",
		// Controllers whose keyboard and mouse interfaces are lizard-mode
		// emulation, not something anyone types or points with.
		"steam deck", "steam controller",
		NULL
	};
	int i;

	if (!name || !name[0])
		return true;
	for (i = 0; ignored[i]; i++)
		if (vid_strcasestr_has(name, ignored[i]))
			return true;
	return false;
}

// Keyboards that are part of the chassis even though they sit on USB: the
// Surface Type Cover is a USB device, and the tablet switch must still be
// able to fold it away.
static qbool vid_name_is_chassis_keyboard(const char *name)
{
	return vid_strcasestr_has(name, "type cover") || vid_strcasestr_has(name, "surface keyboard");
}

// /proc prints each capability bitmap as the kernel's longs, most significant
// first, without leading zero words. The kernel's long is what matters: a
// 32-bit (armhf) userspace on an arm64 kernel still reads 64-bit words.
static int vid_kernel_long_bits(void)
{
	static int bits;
	if (!bits)
	{
		struct utsname u;
		bits = 32;
		if (sizeof(long) == 8)
			bits = 64;
		else if (uname(&u) == 0 && strstr(u.machine, "64"))
			bits = 64;
	}
	return bits;
}

#define VID_BITMAP_WORDS 32
typedef struct vid_bitmap_s
{
	unsigned long long w[VID_BITMAP_WORDS]; // w[0] is the lowest word
	int n;
}
vid_bitmap_t;

static void vid_bitmap_parse(vid_bitmap_t *b, const char *hex, const char *end)
{
	unsigned long long words[VID_BITMAP_WORDS];
	int n = 0, i;
	const char *p = hex;

	memset(b, 0, sizeof(*b));
	while (p < end && n < VID_BITMAP_WORDS)
	{
		unsigned long long v = 0;
		qbool any = false;
		while (p < end && (*p == ' ' || *p == '\t'))
			p++;
		while (p < end && isxdigit((unsigned char)*p))
		{
			int c = tolower((unsigned char)*p);
			v = (v << 4) | (unsigned long long)(c <= '9' ? c - '0' : c - 'a' + 10);
			any = true;
			p++;
		}
		if (!any)
			break;
		words[n++] = v;
	}
	b->n = n;
	for (i = 0; i < n; i++)
		b->w[i] = words[n - 1 - i];
}

static qbool vid_bitmap_has(const vid_bitmap_t *b, unsigned bit)
{
	unsigned wb = (unsigned)vid_kernel_long_bits();
	unsigned idx = bit / wb;
	if ((int)idx >= b->n)
		return false;
	return (b->w[idx] >> (bit % wb)) & 1;
}

// One reading of /proc/bus/input/devices.
#define VID_MAX_NODES 12
typedef enum vid_node_kind_e
{
	VID_NODE_SWITCH, // has SW_TABLET_MODE: read its events for fold / unfold
	VID_NODE_PEN     // a pen on the screen: its contacts count as touch use
}
vid_node_kind_t;

typedef struct vid_node_s
{
	vid_node_kind_t kind;
	char event[16];   // "event7"
	char sysfs[128];  // unique per registration, so a reused eventN is noticed
	int fd;           // -1 until opened
	qbool engaged;    // switch state
}
vid_node_t;

typedef struct vid_scan_s
{
	qbool touch;
	qbool keyboard;
	qbool external_keyboard;
	qbool mouse;      // a mouse, trackpoint or touchpad
	int numnodes;
	vid_node_t nodes[VID_MAX_NODES];
}
vid_scan_t;

typedef struct vid_block_s
{
	unsigned bus;
	char name[128];
	char event[16];
	char sysfs[128];
	unsigned prop;
	vid_bitmap_t key, abs, rel, sw;
	qbool has_sw;
}
vid_block_t;

static void vid_scan_block(vid_scan_t *s, const vid_block_t *b)
{
	qbool direct = (b->prop & (1u << VID_INPUT_PROP_DIRECT)) != 0;
	qbool pointer = (b->prop & (1u << VID_INPUT_PROP_POINTER)) != 0;
	qbool pen = vid_bitmap_has(&b->key, VID_BTN_TOOL_PEN);
	qbool abs_mt = vid_bitmap_has(&b->abs, VID_ABS_MT_POSITION_X);
	qbool controller = vid_bitmap_has(&b->key, VID_BTN_JOYSTICK) || vid_bitmap_has(&b->key, VID_BTN_GAMEPAD);

	if (!b->name[0] && !b->event[0])
		return;

	// Touch: a direct surface (touchscreens, and pens drawn on the screen),
	// or something calling itself one. A touchpad is INPUT_PROP_POINTER and
	// is a mouse, not a touchscreen.
	if (direct || vid_strcasestr_has(b->name, "touchscreen"))
		s->touch = true;
	else if (abs_mt && !pointer && vid_strcasestr_has(b->name, "touch"))
		s->touch = true;

	// Keyboard: letters and a space bar, not a controller (BTN_JOYSTICK /
	// BTN_GAMEPAD: a gamepad that also reports KEY_A is still a gamepad), not
	// a touch surface, and not one of the known button sets or virtual
	// devices.
	if (vid_bitmap_has(&b->key, VID_KEY_A) && vid_bitmap_has(&b->key, VID_KEY_Z) && vid_bitmap_has(&b->key, VID_KEY_SPACE)
		&& !controller && !direct && !vid_name_is_ignored_device(b->name))
	{
		s->keyboard = true;
		if ((b->bus == VID_BUS_USB || b->bus == VID_BUS_BLUETOOTH) && !vid_name_is_chassis_keyboard(b->name))
			s->external_keyboard = true;
	}

	// Mouse: something with a left button that moves a pointer, either by
	// relative motion (mice, trackballs, trackpoints) or as a touchpad
	// (INPUT_PROP_POINTER with a finger tool and a position). Not a direct
	// surface (a touchscreen or screen pen is touch), not a controller, and
	// not a virtual pointer that remappers keep plugged in.
	if (vid_bitmap_has(&b->key, VID_BTN_LEFT) && !direct && !controller && !vid_name_is_ignored_device(b->name))
	{
		qbool relative = vid_bitmap_has(&b->rel, VID_REL_X) && vid_bitmap_has(&b->rel, VID_REL_Y);
		qbool touchpad = pointer && vid_bitmap_has(&b->key, VID_BTN_TOOL_FINGER)
			&& vid_bitmap_has(&b->abs, VID_ABS_X) && vid_bitmap_has(&b->abs, VID_ABS_Y);
		if (relative || touchpad)
			s->mouse = true;
	}

	if (!b->event[0] || s->numnodes >= VID_MAX_NODES)
		return;
	if (b->has_sw && vid_bitmap_has(&b->sw, VID_SW_TABLET_MODE))
	{
		vid_node_t *n = &s->nodes[s->numnodes++];
		memset(n, 0, sizeof(*n));
		n->kind = VID_NODE_SWITCH;
		n->fd = -1;
		vid_scan_copy(n->event, sizeof(n->event), b->event);
		vid_scan_copy(n->sysfs, sizeof(n->sysfs), b->sysfs);
	}
	else if (pen && direct)
	{
		vid_node_t *n = &s->nodes[s->numnodes++];
		memset(n, 0, sizeof(*n));
		n->kind = VID_NODE_PEN;
		n->fd = -1;
		vid_scan_copy(n->event, sizeof(n->event), b->event);
		vid_scan_copy(n->sysfs, sizeof(n->sysfs), b->sysfs);
	}
}

static void vid_scan_text(vid_scan_t *s, const char *text, size_t len)
{
	const char *p = text, *end = text + len;
	vid_block_t b;

	memset(s, 0, sizeof(*s));
	memset(&b, 0, sizeof(b));
	while (p < end)
	{
		const char *eol = (const char *)memchr(p, '\n', end - p);
		size_t l;
		if (!eol)
			eol = end;
		l = eol - p;
		if (l == 0)
		{
			vid_scan_block(s, &b);
			memset(&b, 0, sizeof(b));
		}
		else if (l > 7 && !strncmp(p, "I: Bus=", 7))
			b.bus = (unsigned)strtoul(p + 7, NULL, 16);
		else if (l > 9 && !strncmp(p, "N: Name=\"", 9))
		{
			size_t n = l - 9;
			if (n > 0 && p[9 + n - 1] == '"')
				n--;
			if (n >= sizeof(b.name))
				n = sizeof(b.name) - 1;
			memcpy(b.name, p + 9, n);
			b.name[n] = 0;
		}
		else if (l > 9 && !strncmp(p, "S: Sysfs=", 9))
		{
			size_t n = l - 9;
			if (n >= sizeof(b.sysfs))
				n = sizeof(b.sysfs) - 1;
			memcpy(b.sysfs, p + 9, n);
			b.sysfs[n] = 0;
		}
		else if (l > 12 && !strncmp(p, "H: Handlers=", 12))
		{
			const char *q = p + 12;
			while (q < eol)
			{
				const char *t = q;
				while (q < eol && *q != ' ')
					q++;
				if (q - t > 5 && q - t < (ptrdiff_t)sizeof(b.event) && !strncmp(t, "event", 5))
				{
					memcpy(b.event, t, q - t);
					b.event[q - t] = 0;
				}
				while (q < eol && *q == ' ')
					q++;
			}
		}
		else if (l > 8 && !strncmp(p, "B: PROP=", 8))
			b.prop = (unsigned)strtoul(p + 8, NULL, 16);
		else if (l > 7 && !strncmp(p, "B: KEY=", 7))
			vid_bitmap_parse(&b.key, p + 7, eol);
		else if (l > 7 && !strncmp(p, "B: ABS=", 7))
			vid_bitmap_parse(&b.abs, p + 7, eol);
		else if (l > 7 && !strncmp(p, "B: REL=", 7))
			vid_bitmap_parse(&b.rel, p + 7, eol);
		else if (l > 6 && !strncmp(p, "B: SW=", 6))
		{
			vid_bitmap_parse(&b.sw, p + 6, eol);
			b.has_sw = true;
		}
		p = eol + 1;
	}
	vid_scan_block(s, &b);
}

#endif // VID_INPUTSCAN_H
