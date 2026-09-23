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
Whether the on-screen touch controls run (vid_touchscreen) is decided here,
in two layers:

1. Presence: what hardware is attached. A touchscreen, a keyboard the player
   can type on, whether a convertible is folded into a tablet. On Linux this
   is read from /proc/bus/input/devices and the SW_TABLET_MODE switch; on
   Android the activity reports it over JNI from InputManager. It is re-read
   only when the kernel says a device came or went (inotify on /dev/input, a
   kernel uevent socket), so an idle frame costs one poll() with a zero
   timeout.

2. Last active input: what the player is actually using. A finger (or pen) on
   the screen shows the controls at once, even with a keyboard attached -- a
   Surface with its Type Cover on, tapped. Keyboard and mouse hide them again,
   but only after sustained use with no finger on the glass, so a stray key
   or a bumped mouse does not take the controls away.

Presence is the answer until something has been used, and a keyboard being
plugged in or removed starts over from presence.
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

// Everything that looks at the host (procfs, sysfs, /dev/input, os-release,
// DMI, Click/Lomiri) is desktop-Linux only. Android answers through
// InputManager instead; probing the filesystem there would be pointless.
#if defined(__linux__) && !defined(__ANDROID__)
#define VID_LINUX_PROBES 1
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#endif

#ifdef __ANDROID__
#include <jni.h>
#endif

extern cvar_t vid_touchscreen;
extern cvar_t vid_touchscreen_mode;
extern cvar_t vid_touchscreen_touchonly;
extern cvar_t vid_touchscreen_detected;
extern cvar_t vid_touchscreen_touchonly_detected;

// Android used to force Always on every start, and the archived config kept
// that 2. This records that the one-off move back to Auto has happened, so a
// player who picks Always afterwards keeps it.
static cvar_t vid_touchscreen_mode_rev = {CF_CLIENT | CF_ARCHIVE, "vid_touchscreen_mode_rev", "0", "internal: default-mode migrations already applied to this config"};

qbool VID_SDL_HasTouchDevices(void);

static qbool vid_touch_applying;
static qbool vid_touch_finger_seen;

// ---------------------------------------------------------------------------
// Presence

typedef struct vid_presence_s
{
	qbool touch;        // a direct touch surface (touchscreen or screen pen)
	qbool keyboard;     // a keyboard the player can type on right now
	qbool mouse;        // an external pointing device (Android only)
	qbool tablet_mode;  // SW_TABLET_MODE engaged
}
vid_presence_t;

static vid_presence_t vid_presence;
static qbool vid_presence_valid;

#ifdef VID_LINUX_PROBES

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

// Linux input bits (linux/input-event-codes.h).
#define VID_KEY_Z                 44
#define VID_KEY_SPACE             57
#define VID_BTN_JOYSTICK          0x120
#define VID_BTN_GAMEPAD           0x130
#define VID_BTN_TOOL_PEN          0x140
#define VID_ABS_MT_POSITION_X     53
#define VID_INPUT_PROP_POINTER    0
#define VID_INPUT_PROP_DIRECT     1
#define VID_SW_TABLET_MODE        1

// Linux input bus ids that mean "plugged in by the player".
#define VID_BUS_USB        0x03
#define VID_BUS_BLUETOOTH  0x05

static qbool vid_name_is_ignored_keyboard(const char *name)
{
	static const char *const ignored[] =
	{
		// Button sets and media controls that advertise KEY_A.
		"power button", "sleep button", "lid switch", "video bus", "gpio-keys",
		"headset", "hdmi", "sof-hda", "consumer control", "intel hid",
		"wmi hotkeys", "extra buttons",
		// The switch device itself.
		"tablet mode",
		// Virtual keyboards that remappers and automation (keyd, ydotool,
		// xdotool, uinput tools, remote desktop) keep permanently plugged in.
		"keyd", "virtual", "uinput", "ydotool", "xdotool", "wlroots",
		"remote desktop",
		// Controllers whose keyboard interface is lizard-mode emulation, not
		// something anyone types on.
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
	vid_bitmap_t key, abs, sw;
	qbool has_sw;
}
vid_block_t;

static void vid_scan_block(vid_scan_t *s, const vid_block_t *b)
{
	qbool direct = (b->prop & (1u << VID_INPUT_PROP_DIRECT)) != 0;
	qbool pointer = (b->prop & (1u << VID_INPUT_PROP_POINTER)) != 0;
	qbool pen = vid_bitmap_has(&b->key, VID_BTN_TOOL_PEN);
	qbool abs_mt = vid_bitmap_has(&b->abs, VID_ABS_MT_POSITION_X);

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
	if (vid_bitmap_has(&b->key, KEY_A) && vid_bitmap_has(&b->key, VID_KEY_Z) && vid_bitmap_has(&b->key, VID_KEY_SPACE)
		&& !vid_bitmap_has(&b->key, VID_BTN_JOYSTICK) && !vid_bitmap_has(&b->key, VID_BTN_GAMEPAD)
		&& !direct && !vid_name_is_ignored_keyboard(b->name))
	{
		s->keyboard = true;
		if ((b->bus == VID_BUS_USB || b->bus == VID_BUS_BLUETOOTH) && !vid_name_is_chassis_keyboard(b->name))
			s->external_keyboard = true;
	}

	if (!b->event[0] || s->numnodes >= VID_MAX_NODES)
		return;
	if (b->has_sw && vid_bitmap_has(&b->sw, VID_SW_TABLET_MODE))
	{
		vid_node_t *n = &s->nodes[s->numnodes++];
		memset(n, 0, sizeof(*n));
		n->kind = VID_NODE_SWITCH;
		n->fd = -1;
		dp_strlcpy(n->event, b->event, sizeof(n->event));
		dp_strlcpy(n->sysfs, b->sysfs, sizeof(n->sysfs));
	}
	else if (pen && direct)
	{
		vid_node_t *n = &s->nodes[s->numnodes++];
		memset(n, 0, sizeof(*n));
		n->kind = VID_NODE_PEN;
		n->fd = -1;
		dp_strlcpy(n->event, b->event, sizeof(n->event));
		dp_strlcpy(n->sysfs, b->sysfs, sizeof(n->sysfs));
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
		else if (l > 6 && !strncmp(p, "B: SW=", 6))
		{
			vid_bitmap_parse(&b.sw, p + 6, eol);
			b.has_sw = true;
		}
		p = eol + 1;
	}
	vid_scan_block(s, &b);
}

// The whole of /proc/bus/input/devices, about 0.1 ms to read.
static char *vid_proc_text;
static size_t vid_proc_len, vid_proc_size;
static qbool vid_proc_readable = true;

// Returns true when the listing differs from the last read.
static qbool vid_proc_reread(void)
{
	static char *buf;
	static size_t bufsize;
	size_t len = 0;
	int fd;

	fd = open("/proc/bus/input/devices", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		if (vid_proc_readable)
			Con_DPrintf("Input detection: /proc/bus/input/devices is not readable\n");
		vid_proc_readable = false;
		return false;
	}
	vid_proc_readable = true;
	for (;;)
	{
		ssize_t n;
		if (bufsize - len < 4096)
		{
			size_t ns = bufsize ? bufsize * 2 : 16384;
			char *nb = (char *)realloc(buf, ns);
			if (!nb)
				break;
			buf = nb;
			bufsize = ns;
		}
		n = read(fd, buf + len, bufsize - len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	close(fd);

	if (vid_proc_text && len == vid_proc_len && !memcmp(buf, vid_proc_text, len))
		return false;
	if (vid_proc_size < len + 1)
	{
		char *nt = (char *)realloc(vid_proc_text, len + 1);
		if (!nt)
			return false;
		vid_proc_text = nt;
		vid_proc_size = len + 1;
	}
	memcpy(vid_proc_text, buf, len);
	vid_proc_text[len] = 0;
	vid_proc_len = len;
	return true;
}

// SW_TABLET_MODE and screen-pen descriptors, kept open. /proc names the
// eventN node of each, so only those few nodes are ever opened: walking all
// of /dev/input costs about 0.4 s on a Surface (its IPTS nodes are slow to
// open), which was a visible stall on every device change.
static vid_scan_t vid_scan;
static qbool vid_scan_valid;
static qbool vid_nodes_pending; // a wanted node could not be opened yet
static qbool vid_pollset_dirty = true;
static double vid_pen_last = -1000;

static int vid_ulong_bits(void) { return (int)(8 * sizeof(unsigned long)); }

static qbool vid_switch_query(int fd)
{
	unsigned long state[(SW_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
	memset(state, 0, sizeof(state));
	if (ioctl(fd, EVIOCGSW(sizeof(state)), state) < 0)
		return false;
	return (state[VID_SW_TABLET_MODE / vid_ulong_bits()] >> (VID_SW_TABLET_MODE % vid_ulong_bits())) & 1;
}

static void vid_node_open(vid_node_t *n)
{
	char path[64];
	dpsnprintf(path, sizeof(path), "/dev/input/%s", n->event);
	n->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (n->fd < 0)
	{
		// Usually udev has not granted access yet; IN_ATTRIB follows.
		vid_nodes_pending = true;
		return;
	}
	if (n->kind == VID_NODE_SWITCH)
		n->engaged = vid_switch_query(n->fd);
	vid_pollset_dirty = true;
}

static void vid_node_close(vid_node_t *n)
{
	if (n->fd >= 0)
	{
		close(n->fd);
		vid_pollset_dirty = true;
	}
	n->fd = -1;
}

// Adopt a new listing: keep descriptors whose device is still there, close
// the rest, open the new ones.
static void vid_scan_adopt(vid_scan_t *next)
{
	int i, j;

	for (i = 0; i < vid_scan.numnodes; i++)
	{
		vid_node_t *old = &vid_scan.nodes[i];
		if (old->fd < 0)
			continue;
		for (j = 0; j < next->numnodes; j++)
		{
			vid_node_t *n = &next->nodes[j];
			if (n->fd < 0 && n->kind == old->kind && !strcmp(n->event, old->event) && !strcmp(n->sysfs, old->sysfs))
			{
				n->fd = old->fd;
				n->engaged = old->engaged;
				old->fd = -1;
				break;
			}
		}
		if (old->fd >= 0)
			vid_node_close(old);
	}
	vid_nodes_pending = false;
	for (j = 0; j < next->numnodes; j++)
		if (next->nodes[j].fd < 0)
			vid_node_open(&next->nodes[j]);
	vid_scan = *next;
	vid_scan_valid = true;
}

static void vid_nodes_retry(void)
{
	int j;
	vid_nodes_pending = false;
	for (j = 0; j < vid_scan.numnodes; j++)
		if (vid_scan.nodes[j].fd < 0)
			vid_node_open(&vid_scan.nodes[j]);
}

// Re-read procfs. Returns true when anything may have changed.
static qbool vid_linux_refresh(qbool force)
{
	vid_scan_t next;

	if (!vid_proc_reread() && vid_scan_valid && !force)
	{
		if (vid_nodes_pending)
			vid_nodes_retry();
		return false;
	}
	if (vid_proc_text)
		vid_scan_text(&next, vid_proc_text, vid_proc_len);
	else
		memset(&next, 0, sizeof(next));
	vid_scan_adopt(&next);
	return true;
}

static int vid_read_chassis_type(void)
{
	static int chassis = -2;
	FILE *f;

	if (chassis != -2)
		return chassis;
	chassis = -1;
	f = fopen("/sys/class/dmi/id/chassis_type", "r");
	if (!f)
		return chassis;
	if (fscanf(f, "%d", &chassis) != 1)
		chassis = -1;
	fclose(f);
	return chassis;
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
	static int cached = -1;
	const char *desktop;

	if (cached >= 0)
		return cached != 0;
	cached = 0;
	if (vid_os_release_has("Ubuntu Touch") || vid_os_release_has("UBUNTU_TOUCH")
		|| vid_os_release_has("VARIANT_ID=touch") || vid_os_release_has("lomiri"))
		cached = 1;
	else if (access("/usr/share/ubports", F_OK) == 0)
		cached = 1;
	else if ((desktop = getenv("XDG_CURRENT_DESKTOP")) && (vid_strcasestr_has(desktop, "Lomiri") || vid_strcasestr_has(desktop, "Unity8")))
		cached = 1;
	else if (getenv("CLICK_FRAMEWORK") && getenv("CLICK_FRAMEWORK")[0])
		cached = 1;
	return cached != 0;
}

static qbool vid_chassis_is_handheld_or_tablet(int chassis)
{
	// SMBIOS chassis: 11 Hand Held, 30 Tablet.
	// 31 Convertible / 32 Detachable still have a keyboard when docked.
	return chassis == 11 || chassis == 30;
}

#endif // VID_LINUX_PROBES

#ifdef __ANDROID__
// Written by the activity's UI thread (nativeInputDevices), read per frame by
// the engine thread. Bit 3 set means the activity has reported at all.
#define VID_ANDROID_TOUCH    1
#define VID_ANDROID_KEYBOARD 2
#define VID_ANDROID_MOUSE    4
#define VID_ANDROID_VALID    8
static int vid_android_state;
static int vid_android_seen;

JNIEXPORT void JNICALL Java_io_github_dixonsolutions_xonotictouch_XonoticActivity_nativeInputDevices(JNIEnv *env, jclass cls, jboolean touch, jboolean keyboard, jboolean mouse);
JNIEXPORT void JNICALL Java_io_github_dixonsolutions_xonotictouch_XonoticActivity_nativeInputDevices(JNIEnv *env, jclass cls, jboolean touch, jboolean keyboard, jboolean mouse)
{
	int v = VID_ANDROID_VALID | (touch ? VID_ANDROID_TOUCH : 0) | (keyboard ? VID_ANDROID_KEYBOARD : 0) | (mouse ? VID_ANDROID_MOUSE : 0);
	(void)env;
	(void)cls;
	__atomic_store_n(&vid_android_state, v, __ATOMIC_RELEASE);
}
#endif

static void vid_presence_compute(vid_presence_t *p)
{
	memset(p, 0, sizeof(*p));

#if defined(__ANDROID__)
	{
		int v = __atomic_load_n(&vid_android_state, __ATOMIC_ACQUIRE);
		vid_android_seen = v;
		// Until the activity has spoken, a phone is a touch device.
		p->touch = !(v & VID_ANDROID_VALID) || (v & VID_ANDROID_TOUCH) || vid_touch_finger_seen;
		p->keyboard = (v & VID_ANDROID_KEYBOARD) != 0;
		p->mouse = (v & VID_ANDROID_MOUSE) != 0;
	}
#elif defined(DP_MOBILETOUCH)
	p->touch = true;
	p->keyboard = false;
#else
	p->touch = VID_SDL_HasTouchDevices() || vid_touch_finger_seen;
#ifdef VID_LINUX_PROBES
	{
		qbool external, switch_found = false;
		int i;

		if (!vid_scan_valid)
			vid_linux_refresh(true);
		p->touch = p->touch || vid_scan.touch;
		p->keyboard = vid_scan.keyboard;
		external = vid_scan.external_keyboard;
		for (i = 0; i < vid_scan.numnodes; i++)
		{
			if (vid_scan.nodes[i].kind != VID_NODE_SWITCH || vid_scan.nodes[i].fd < 0)
				continue;
			switch_found = true;
			if (vid_scan.nodes[i].engaged)
				p->tablet_mode = true;
		}
		// A tablet-mode switch saying "tablet" overrides the built-in keyboard
		// (a folded-back Type Cover stays listed). A USB or Bluetooth keyboard
		// is not part of the chassis, so it counts whatever the switch says.
		if (switch_found && p->tablet_mode)
			p->keyboard = external;
		if (vid_chassis_is_handheld_or_tablet(vid_read_chassis_type()) && p->touch)
			p->keyboard = external;
		if (vid_is_ubuntu_touch())
		{
			p->touch = true;
			p->keyboard = external;
		}
	}
#endif
#endif
}

static const vid_presence_t *vid_presence_get(void)
{
	if (!vid_presence_valid)
	{
		vid_presence_compute(&vid_presence);
		vid_presence_valid = true;
	}
	return &vid_presence;
}

void VID_DetectTouchHardware(qbool *has_touchscreen, qbool *is_touch_only)
{
	const vid_presence_t *p = vid_presence_get();
	*has_touchscreen = p->touch;
	*is_touch_only = p->touch && !p->keyboard;
}

// ---------------------------------------------------------------------------
// Last active input

typedef enum vid_active_e
{
	VID_ACTIVE_NONE,     // nothing used yet: presence decides
	VID_ACTIVE_TOUCH,    // a finger or pen touched the screen
	VID_ACTIVE_KEYBOARD  // sustained keyboard / mouse play
}
vid_active_t;

static vid_active_t vid_active = VID_ACTIVE_NONE;

static void vid_presence_refresh(void);
#ifdef VID_LINUX_PROBES
static qbool vid_linux_poll(void);
static qbool vid_pens_open(void);
#endif

// Hysteresis. Touch wins at once; keyboard and mouse have to be used for a
// moment, with no finger on the glass, before they take the controls away.
#define VID_KBM_WINDOW        2.0   // seconds the evidence below must fit in
#define VID_KBM_PRESSES       3     // distinct key / button presses
#define VID_KBM_MOUSE_TRAVEL  0.15  // pointer travel, in window widths
#define VID_KBM_MOUSE_STEP    0.03  // most one motion event may contribute
#define VID_TOUCH_QUIET       1.5   // seconds after the last finger event
#define VID_PEN_QUIET         1.0   // pen hover drives the pointer too

static double vid_touch_last = -1000;
static double vid_kbm_start = -1000;
static int vid_kbm_presses;      // key presses plus mouse clicks
static float vid_kbm_travel;
static int vid_kbm_keys;         // key presses alone

static const char *vid_mode_name(int mode)
{
	return mode <= 0 ? "off" : (mode >= 2 ? "always" : "auto");
}

// Re-derive vid_touchscreen and say what happened if it changed.
static void vid_reapply(const char *why, const char *msg_on, const char *msg_off)
{
	qbool was_on = vid_touchscreen.integer != 0;
	qbool is_on;
	const char *msg;

	VID_ApplyTouchscreenMode();
	is_on = vid_touchscreen.integer != 0;
	if (was_on == is_on)
		return;
	msg = is_on ? msg_on : msg_off;
	Con_Printf("Input: %s -> touch controls %s%s%s\n", why, is_on ? "on" : "off", msg ? ": " : "", msg ? msg : "");
	if (msg)
		SCR_Toast(msg);
}

static void vid_kbm_reset(void)
{
	vid_kbm_start = -1000;
	vid_kbm_presses = 0;
	vid_kbm_travel = 0;
	vid_kbm_keys = 0;
}

static void vid_touch_used(void)
{
	vid_touch_last = host.realtime;
	vid_kbm_reset();
	if (!vid_touch_finger_seen)
	{
		// SDL often reports no touch device until the first finger lands.
		vid_touch_finger_seen = true;
		vid_presence_valid = false;
	}
	if (vid_active == VID_ACTIVE_TOUCH && vid_presence_valid)
		return;
	vid_active = VID_ACTIVE_TOUCH;
	vid_reapply("touch used", "Touch detected: touch controls shown", NULL);
}

void VID_NoteTouchUse(void)
{
	vid_touch_used();
}

void VID_NoteTouchActivity(void)
{
	vid_touch_last = host.realtime;
	vid_kbm_reset();
}

// Evidence for keyboard / mouse play. Only matters while the controls are on.
static qbool vid_kbm_accept(void)
{
	const vid_presence_t *p;

	if (vid_active == VID_ACTIVE_KEYBOARD || !vid_touchscreen.integer || vid_touchscreen_mode.integer != 1)
		return false;
	// A finger is down or has just lifted: keys now are an elbow on a
	// folded cover, and pointer motion is the compositor's touch emulation.
	if (host.realtime - vid_touch_last < VID_TOUCH_QUIET)
	{
		vid_kbm_reset();
		return false;
	}
	// Keys from a keyboard folded behind the screen.
	p = vid_presence_get();
	if (p->tablet_mode && !p->keyboard)
		return false;
	if (host.realtime - vid_kbm_start > VID_KBM_WINDOW)
	{
		vid_kbm_reset();
		vid_kbm_start = host.realtime;
	}
	return true;
}

static void vid_kbm_check(void)
{
	if (vid_kbm_presses < VID_KBM_PRESSES && vid_kbm_travel < VID_KBM_MOUSE_TRAVEL)
		return;
	vid_active = VID_ACTIVE_KEYBOARD;
	if (!vid_kbm_keys)
		vid_reapply("mouse used", NULL, "Mouse in use: touch controls hidden");
	else
		vid_reapply("keyboard used", NULL, "Keyboard in use: touch controls hidden");
	vid_kbm_reset();
}

void VID_NoteKeyboardUse(void)
{
	if (!vid_kbm_accept())
		return;
	vid_kbm_presses++;
	vid_kbm_keys++;
	vid_kbm_check();
}

void VID_NoteMouseUse(float travel, qbool press)
{
	// Without a keyboard there is nothing to move with: a lone mouse (or a
	// pen the compositor turns into one) is not a reason to take the stick
	// away. Keys already pressed in this window say otherwise.
	if (!vid_presence_get()->keyboard && !vid_kbm_keys)
		return;
	if (vid_active == VID_ACTIVE_KEYBOARD || !vid_touchscreen.integer || vid_touchscreen_mode.integer != 1)
		return;
#ifdef VID_LINUX_PROBES
	// Wayland hands a pen to SDL as a mouse. Catch up on the pen's own
	// events before counting this as a mouse.
	if (vid_pens_open() && vid_linux_poll())
		vid_presence_refresh();
	if (host.realtime - vid_pen_last < VID_PEN_QUIET)
		return;
#endif
	if (!vid_kbm_accept())
		return;
	if (travel > VID_KBM_MOUSE_STEP)
		travel = VID_KBM_MOUSE_STEP;
	if (travel > 0)
		vid_kbm_travel += travel;
	if (press)
		vid_kbm_presses++;
	vid_kbm_check();
}

// ---------------------------------------------------------------------------
// Live hot-plug.
//
// Linux: inotify on /dev/input (a node appears, disappears, or udev grants
// access) and a kernel uevent socket (SUBSYSTEM=input) say when to re-read
// procfs; the kept switch and pen descriptors deliver EV_SW and pen events
// directly. All of it is one poll() with a zero timeout per frame. Where
// neither watch is allowed (strict confinement), procfs is re-read every
// few seconds instead.
//
// Android: the activity pushes InputManager's answer through JNI; the frame
// reads one int.

static qbool vid_hotplug_init;

static void vid_presence_changed(const vid_presence_t *old, const vid_presence_t *now)
{
	const char *msg = NULL;
	qbool was_on, is_on;

	// Plugging a keyboard in or pulling it out is a fresh start: the
	// hardware answer stands until something is used again.
	if (old->keyboard != now->keyboard)
	{
		vid_active = VID_ACTIVE_NONE;
		vid_kbm_reset();
	}

	was_on = vid_touchscreen.integer != 0;
	VID_ApplyTouchscreenMode();
	is_on = vid_touchscreen.integer != 0;

	if (old->tablet_mode != now->tablet_mode && was_on != is_on)
		msg = is_on ? "Tablet mode: touch controls shown" : "Laptop mode: touch controls hidden";
	else if (old->keyboard != now->keyboard)
	{
		if (now->keyboard)
			msg = (was_on && !is_on) ? "Keyboard connected: touch controls hidden" : "Keyboard connected";
		else
			msg = (!was_on && is_on) ? "Keyboard disconnected: touch controls shown" : "Keyboard disconnected";
	}
	else if (old->touch != now->touch)
	{
		if (now->touch)
			msg = (!was_on && is_on) ? "Touchscreen detected: touch controls shown" : "Touchscreen detected";
		else
			msg = "Touchscreen removed";
	}

	Con_Printf("Input hot-plug: keyboard=%d touch=%d mouse=%d tablet_mode=%d -> touch controls %s%s%s\n",
		now->keyboard, now->touch, now->mouse, now->tablet_mode, is_on ? "on" : "off", msg ? ": " : "", msg ? msg : "");
	if (msg)
		SCR_Toast(msg);
}

// Recompute presence and react if the answer moved.
static void vid_presence_refresh(void)
{
	vid_presence_t old = vid_presence;
	qbool had = vid_presence_valid;

	vid_presence_valid = false;
	vid_presence_get();
	if (!had)
		return;
	if (old.keyboard != vid_presence.keyboard || old.touch != vid_presence.touch
		|| old.tablet_mode != vid_presence.tablet_mode || old.mouse != vid_presence.mouse)
		vid_presence_changed(&old, &vid_presence);
}

#ifdef VID_LINUX_PROBES

static int vid_inotify_fd = -1;
static int vid_uevent_fd = -1;
static double vid_rescan_at;      // 0: nothing pending
static double vid_fallback_next;  // procfs poll when neither watch works
#define VID_RESCAN_DELAY     0.15 // let a burst of node events settle
#define VID_POLL_PERIOD      0.1
#define VID_FALLBACK_PERIOD  3.0

#define VID_POLL_MAX (2 + VID_MAX_NODES)
static struct pollfd vid_pollfds[VID_POLL_MAX];
static int vid_pollnode[VID_POLL_MAX]; // -1 inotify, -2 uevent, else node index
static int vid_npollfds;

static void vid_watch_open(void)
{
	vid_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (vid_inotify_fd >= 0 && inotify_add_watch(vid_inotify_fd, "/dev/input", IN_CREATE | IN_DELETE | IN_ATTRIB) < 0)
	{
		close(vid_inotify_fd);
		vid_inotify_fd = -1;
	}

	vid_uevent_fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (vid_uevent_fd >= 0)
	{
		struct sockaddr_nl addr;
		memset(&addr, 0, sizeof(addr));
		addr.nl_family = AF_NETLINK;
		addr.nl_groups = 1; // kernel events
		if (bind(vid_uevent_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		{
			close(vid_uevent_fd);
			vid_uevent_fd = -1;
		}
	}
	vid_pollset_dirty = true;
}

static void vid_pollset_build(void)
{
	int i;
	vid_npollfds = 0;
	if (vid_inotify_fd >= 0)
	{
		vid_pollfds[vid_npollfds].fd = vid_inotify_fd;
		vid_pollfds[vid_npollfds].events = POLLIN;
		vid_pollnode[vid_npollfds++] = -1;
	}
	if (vid_uevent_fd >= 0)
	{
		vid_pollfds[vid_npollfds].fd = vid_uevent_fd;
		vid_pollfds[vid_npollfds].events = POLLIN;
		vid_pollnode[vid_npollfds++] = -2;
	}
	for (i = 0; i < vid_scan.numnodes && vid_npollfds < VID_POLL_MAX; i++)
	{
		if (vid_scan.nodes[i].fd < 0)
			continue;
		vid_pollfds[vid_npollfds].fd = vid_scan.nodes[i].fd;
		vid_pollfds[vid_npollfds].events = POLLIN;
		vid_pollnode[vid_npollfds++] = i;
	}
	vid_pollset_dirty = false;
}

static void vid_schedule_rescan(void)
{
	double at = host.realtime + VID_RESCAN_DELAY;
	if (!vid_rescan_at || at < vid_rescan_at)
		vid_rescan_at = at;
}

static void vid_inotify_drain(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	while ((n = read(vid_inotify_fd, buf, sizeof(buf))) > 0)
	{
		char *p = buf;
		while (p < buf + n)
		{
			struct inotify_event *ev = (struct inotify_event *)p;
			// eventN is the only node this reads; js / mouse / by-id churn is
			// part of the same device and says nothing new.
			if ((ev->mask & IN_Q_OVERFLOW) || (ev->len && !strncmp(ev->name, "event", 5)))
				vid_schedule_rescan();
			p += sizeof(struct inotify_event) + ev->len;
		}
	}
}

static void vid_uevent_drain(void)
{
	char buf[4096];
	ssize_t n;
	while ((n = recv(vid_uevent_fd, buf, sizeof(buf) - 1, 0)) > 0)
	{
		ssize_t i = 0;
		buf[n] = 0;
		// "add@/devices/...\0ACTION=add\0DEVPATH=...\0SUBSYSTEM=input\0..."
		while (i < n)
		{
			if (!strcmp(buf + i, "SUBSYSTEM=input"))
			{
				vid_schedule_rescan();
				break;
			}
			i += (ssize_t)strlen(buf + i) + 1;
		}
	}
}

// Returns true when a switch changed state.
static qbool vid_node_drain(vid_node_t *n)
{
	struct input_event ev[32];
	qbool changed = false, dropped = false;
	ssize_t r;
	int i;

	while ((r = read(n->fd, ev, sizeof(ev))) > 0)
	{
		for (i = 0; i < (int)(r / (ssize_t)sizeof(ev[0])); i++)
		{
			if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED)
				dropped = true;
			else if (n->kind == VID_NODE_SWITCH && ev[i].type == EV_SW && ev[i].code == VID_SW_TABLET_MODE)
			{
				if (n->engaged != (ev[i].value != 0))
					changed = true;
				n->engaged = ev[i].value != 0;
			}
			else if (n->kind == VID_NODE_PEN)
			{
				vid_pen_last = host.realtime;
				// Pen on glass: the same as a finger.
				if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH && ev[i].value == 1)
					vid_touch_used();
			}
		}
	}
	if (r < 0 && errno != EAGAIN && errno != EINTR)
	{
		// ENODEV: the device went away. The listing change follows.
		vid_node_close(n);
		vid_schedule_rescan();
		return true;
	}
	if (dropped && n->kind == VID_NODE_SWITCH)
	{
		qbool e = vid_switch_query(n->fd);
		if (e != n->engaged)
			changed = true;
		n->engaged = e;
	}
	return changed;
}

// One zero-timeout poll over the watches and kept descriptors. Returns true
// when presence may have changed.
static qbool vid_linux_poll(void)
{
	qbool presence_dirty = false;
	int i, r;

	if (vid_pollset_dirty)
		vid_pollset_build();

	if (vid_npollfds > 0)
	{
		r = poll(vid_pollfds, vid_npollfds, 0);
		if (r > 0)
		{
			for (i = 0; i < vid_npollfds; i++)
			{
				short re = vid_pollfds[i].revents;
				if (!re)
					continue;
				if (vid_pollnode[i] == -1)
					vid_inotify_drain();
				else if (vid_pollnode[i] == -2)
					vid_uevent_drain();
				else if (vid_pollnode[i] < vid_scan.numnodes && vid_scan.nodes[vid_pollnode[i]].fd == vid_pollfds[i].fd)
				{
					vid_node_t *n = &vid_scan.nodes[vid_pollnode[i]];
					if (re & (POLLERR | POLLHUP | POLLNVAL))
					{
						vid_node_close(n);
						vid_schedule_rescan();
						presence_dirty = true;
					}
					else if (vid_node_drain(n))
						presence_dirty = true;
				}
			}
		}
	}
	return presence_dirty;
}

static qbool vid_pens_open(void)
{
	int i;
	for (i = 0; i < vid_scan.numnodes; i++)
		if (vid_scan.nodes[i].kind == VID_NODE_PEN && vid_scan.nodes[i].fd >= 0)
			return true;
	return false;
}

static void vid_linux_frame(void)
{
	static double poll_next;
	qbool presence_dirty = false;

	// Nothing here needs frame accuracy: a device change is noticed within
	// VID_POLL_PERIOD (+ VID_RESCAN_DELAY to let the burst settle), and most
	// frames cost one comparison.
	if (host.realtime >= poll_next)
	{
		poll_next = host.realtime + VID_POLL_PERIOD;
		presence_dirty = vid_linux_poll();
	}

	if (vid_inotify_fd < 0 && vid_uevent_fd < 0 && host.realtime >= vid_fallback_next)
	{
		vid_fallback_next = host.realtime + VID_FALLBACK_PERIOD;
		vid_schedule_rescan();
	}
	// Without inotify nothing announces udev granting access to a new node.
	if (vid_nodes_pending && vid_inotify_fd < 0 && !vid_rescan_at)
		vid_rescan_at = host.realtime + 1.0;

	if (vid_rescan_at && host.realtime >= vid_rescan_at)
	{
		vid_rescan_at = 0;
		if (vid_linux_refresh(false))
			presence_dirty = true;
	}

	if (presence_dirty)
		vid_presence_refresh();
}
#endif // VID_LINUX_PROBES

void VID_TouchHotplugFrame(void)
{
#if defined(DP_MOBILETOUCH) && !defined(__ANDROID__)
	return;
#else
	if (!vid_hotplug_init)
	{
		const vid_presence_t *p;
		vid_hotplug_init = true;
#ifdef __ANDROID__
		// The old builds forced Always and archived it. Once, move that back
		// to Auto; a later explicit Always sticks.
		if (vid_touchscreen_mode_rev.integer < 1)
		{
			if (vid_touchscreen_mode.integer == 2)
				Cvar_SetValueQuick(&vid_touchscreen_mode, 1);
			Cvar_SetValueQuick(&vid_touchscreen_mode_rev, 1);
		}
#endif
#ifdef VID_LINUX_PROBES
		vid_watch_open();
		vid_linux_refresh(false);
#endif
		vid_presence_valid = false;
		VID_ApplyTouchscreenMode();
		p = vid_presence_get();
		Con_Printf("Input hot-plug: startup keyboard=%d touch=%d mouse=%d tablet_mode=%d, touch controls %s, watching %s\n",
			p->keyboard, p->touch, p->mouse, p->tablet_mode, vid_touchscreen.integer ? "on" : "off",
#if defined(__ANDROID__)
			"InputManager"
#elif defined(VID_LINUX_PROBES)
			(vid_inotify_fd >= 0 && vid_uevent_fd >= 0) ? "inotify+uevent"
				: vid_inotify_fd >= 0 ? "inotify" : vid_uevent_fd >= 0 ? "uevent" : "procfs poll"
#else
			"nothing"
#endif
			);
		return;
	}

#ifdef __ANDROID__
	if (__atomic_load_n(&vid_android_state, __ATOMIC_ACQUIRE) != vid_android_seen)
		vid_presence_refresh();
#elif defined(VID_LINUX_PROBES)
	vid_linux_frame();
#endif
#endif
}

void VID_TouchDetect_Init(void)
{
	Cvar_RegisterVariable(&vid_touchscreen_mode_rev);
#ifdef __ANDROID__
	// Controls on until the activity reports a keyboard; Auto decides from
	// there.
	vid_touch_applying = true;
	Cvar_SetValueQuick(&vid_touchscreen, 1);
	vid_touch_applying = false;
#endif
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
	else if (vid_active == VID_ACTIVE_TOUCH)
		want = 1;
	else if (vid_active == VID_ACTIVE_KEYBOARD)
		want = 0;
	else if (vid_touchscreen_touchonly.integer)
		want = touch_only ? 1 : 0;
	else
		want = has_touch ? 1 : 0;

	if (vid_touchscreen.integer != want)
	{
		Cvar_SetValueQuick(&vid_touchscreen, want);
		Con_Printf("Touch controls: %s (mode=%s, hardware touch=%s, touch-only=%s, last input=%s)\n",
			want ? "on" : "off",
			vid_mode_name(mode),
			has_touch ? "yes" : "no",
			touch_only ? "yes" : "no",
			vid_active == VID_ACTIVE_TOUCH ? "touch" : vid_active == VID_ACTIVE_KEYBOARD ? "keyboard/mouse" : "none");
	}
	vid_touch_applying = false;
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
	const vid_presence_t *p;
	(void)cmd;
#ifdef VID_LINUX_PROBES
	vid_linux_refresh(true);
#endif
	vid_presence_refresh();
	VID_ApplyTouchscreenMode();
	p = vid_presence_get();
	Con_Printf("vid_touchscreen_mode %d, vid_touchscreen %d, detected %d, touch-only %d, keyboard %d, tablet mode %d, last input %s\n",
		vid_touchscreen_mode.integer,
		vid_touchscreen.integer,
		vid_touchscreen_detected.integer,
		vid_touchscreen_touchonly_detected.integer,
		p->keyboard, p->tablet_mode,
		vid_active == VID_ACTIVE_TOUCH ? "touch" : vid_active == VID_ACTIVE_KEYBOARD ? "keyboard/mouse" : "none");
}
