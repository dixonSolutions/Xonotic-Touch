/*
Feeds recorded /proc/bus/input/devices listings to the engine's parser
(engine/darkplaces/vid_inputscan.h) and checks what it says is attached.

Run through scripts/test-input-presence.sh.
*/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef bool qbool;

#include "vid_inputscan.h"

typedef struct expect_s
{
	const char *fixture;
	int touch, keyboard, external_keyboard, mouse;
	int switches, pens;
}
expect_t;

static const expect_t expects[] =
{
	// This laptop: built-in keyboard and touchpad. keyd's and ydotool's
	// virtual keyboard and pointers, Intel HID, WMI hotkeys and the power
	// and lid buttons are none of those.
	{ "laptop-keyboard-touchpad.txt",        0, 1, 0, 1, 0, 0 },
	// Surface Pro 9: touchscreen, pen on the screen, Type Cover keyboard and
	// touchpad on USB (chassis, so not external), tablet-mode switch.
	{ "surface-pro-9.txt",                   1, 1, 0, 1, 1, 1 },
	// Phone: touchscreen and button sets only. A fingerprint reader that
	// registers as a keyboard is not one.
	{ "phone-touch-only.txt",                1, 0, 0, 0, 0, 0 },
	// Desktop: USB keyboard and mouse. The gamepad, the Steam Deck's
	// lizard-mode keyboard and mouse, and keyd's virtual devices add nothing.
	{ "desktop-usb-keyboard-mouse-pad.txt",  0, 1, 1, 1, 0, 0 },
	// Tablet with a Bluetooth mouse (uhid, under /devices/virtual/misc) and
	// no keyboard: a mouse says nothing about a keyboard.
	{ "tablet-touch-bluetooth-mouse.txt",    1, 0, 0, 1, 1, 0 },
};

static char *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = (char *)malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	buf[n] = 0;
	*len = (size_t)n;
	return buf;
}

static int check(const char *fixture, const char *what, int got, int want)
{
	if (got == want)
		return 0;
	fprintf(stderr, "FAIL %s: %s = %d, expected %d\n", fixture, what, got, want);
	return 1;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "fixtures";
	int failures = 0;
	size_t i;

	for (i = 0; i < sizeof(expects) / sizeof(expects[0]); i++)
	{
		const expect_t *e = &expects[i];
		char path[512];
		char *text;
		size_t len;
		vid_scan_t s;
		int switches = 0, pens = 0, j, before = failures;

		snprintf(path, sizeof(path), "%s/%s", dir, e->fixture);
		text = read_file(path, &len);
		if (!text)
		{
			fprintf(stderr, "FAIL %s: cannot read\n", path);
			failures++;
			continue;
		}
		vid_scan_text(&s, text, len);
		free(text);
		for (j = 0; j < s.numnodes; j++)
		{
			if (s.nodes[j].kind == VID_NODE_SWITCH)
				switches++;
			else if (s.nodes[j].kind == VID_NODE_PEN)
				pens++;
		}
		failures += check(e->fixture, "touch", s.touch, e->touch);
		failures += check(e->fixture, "keyboard", s.keyboard, e->keyboard);
		failures += check(e->fixture, "external_keyboard", s.external_keyboard, e->external_keyboard);
		failures += check(e->fixture, "mouse", s.mouse, e->mouse);
		failures += check(e->fixture, "tablet-mode switches", switches, e->switches);
		failures += check(e->fixture, "screen pens", pens, e->pens);
		printf("%s %s: touch=%d keyboard=%d external=%d mouse=%d switches=%d pens=%d\n",
			failures == before ? "ok  " : "FAIL", e->fixture,
			s.touch, s.keyboard, s.external_keyboard, s.mouse, switches, pens);
	}

	// Without the kernel long the bitmap words cannot be placed.
	if (vid_kernel_long_bits() != 64)
		printf("note: bitmaps read as 32-bit words; fixtures are from 64-bit kernels\n");

	if (failures)
	{
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("all input presence checks passed\n");
	return 0;
}
