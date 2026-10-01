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

1. Presence: what hardware the system lists. A touchscreen, a keyboard the
   player can type on, a mouse or touchpad, whether a convertible is folded
   into a tablet. On Linux this is read from /proc/bus/input/devices and the
   SW_TABLET_MODE switch (vid_inputscan.h); on Android the activity reports
   it over JNI from InputManager. It is re-read only when the kernel says a
   device came or went (inotify on /dev/input, a kernel uevent socket), so
   an idle frame costs one poll() with a zero timeout.

2. Last active input: what the player is actually using. Input only ever
   confirms what presence already lists; it never stands in for it. A key
   arriving with no keyboard listed is an on-screen keyboard, a remote or a
   mouse's macro button, and changes nothing.
   - A finger (or pen) on the screen shows the controls at once, even with a
     keyboard attached -- a Surface with its Type Cover on, tapped.
   - Sustained typing on a listed keyboard, with a listed mouse or touchpad
     to aim with and no finger on the glass, hides them.
   - A mouse never hides them. Mouse and touch work together: its clicks are
     clicks, and once a listed mouse has been used it aims in the game while
     the touch controls stay (VID_TouchMouselook).

Presence is the answer until something has been used, and a keyboard being
plugged in or removed starts over from presence.

On top of that, the player decides how much of this runs:
- vid_touchscreen_mode: Dynamic (1, all of the above), Touch (2) or
  Keyboard & mouse (0), the last two with no detection at all.
- vid_touchscreen_live 0: Dynamic decides once, when presence has settled at
  launch, and keeps that answer for the session.
- vid_touchscreen_notify 0: switches happen without a toast.
- Ctrl+Alt+1 switches between touch and keyboard-and-mouse and locks the
  result; Ctrl+Alt+2 locks whatever is showing, or unlocks. A lock beats
  every mode and lasts until it is unlocked or the game restarts.
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
extern cvar_t vid_keyboard_detected;
extern cvar_t vid_touchscreen_live;
extern cvar_t vid_touchscreen_notify;

// Android used to force Always on every start, and the archived config kept
// that 2. This records that the one-off move back to Auto has happened, so a
// player who picks Always afterwards keeps it.
static cvar_t vid_touchscreen_mode_rev = {CF_CLIENT | CF_ARCHIVE, "vid_touchscreen_mode_rev", "0", "internal: default-mode migrations already applied to this config"};

qbool VID_SDL_HasTouchDevices(void);

static qbool vid_touch_applying;
static qbool vid_touch_finger_seen;
static qbool vid_hotplug_init;

// Ctrl+Alt+1 / Ctrl+Alt+2: -1 when nothing is locked, else the touch controls
// are held at 0 or 1 whatever the mode and detection say. Not archived, so a
// relaunch unlocks.
static int vid_locked = -1;

// vid_touchscreen_live 0: what Dynamic answered once presence had settled at
// launch (or at the last rescan). -1 until then.
static int vid_fixed = -1;

// ---------------------------------------------------------------------------
// Presence

typedef struct vid_presence_s
{
	qbool touch;        // a direct touch surface (touchscreen or screen pen)
	qbool keyboard;     // a keyboard the player can type on right now
	qbool mouse;        // a mouse, trackpoint or touchpad
	qbool tablet_mode;  // SW_TABLET_MODE engaged
}
vid_presence_t;

static vid_presence_t vid_presence;
static qbool vid_presence_valid;

#ifdef VID_LINUX_PROBES

#include "vid_inputscan.h"

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
		qbool as_tablet, switch_found = false;
		int i;

		if (!vid_scan_valid)
			vid_linux_refresh(true);
		p->touch = p->touch || vid_scan.touch;
		p->keyboard = vid_scan.keyboard;
		p->mouse = vid_scan.mouse;
		for (i = 0; i < vid_scan.numnodes; i++)
		{
			if (vid_scan.nodes[i].kind != VID_NODE_SWITCH || vid_scan.nodes[i].fd < 0)
				continue;
			switch_found = true;
			if (vid_scan.nodes[i].engaged)
				p->tablet_mode = true;
		}
		as_tablet = (switch_found && p->tablet_mode)
			|| (vid_chassis_is_handheld_or_tablet(vid_read_chassis_type()) && p->touch);
		if (vid_is_ubuntu_touch())
		{
			p->touch = true;
			as_tablet = true;
		}
		// Held as a tablet: the chassis' own keyboard and touchpad are folded
		// behind the screen or face-down on the table, whatever /proc still
		// lists (a Type Cover stays listed once folded away, touchpad and
		// all). Only what the player attached over USB or Bluetooth is still
		// something to type on or aim with.
		if (as_tablet)
		{
			p->keyboard = vid_scan.external_keyboard;
			p->mouse = vid_scan.external_mouse;
		}
	}
#else
	// Nothing to ask on this platform: a desktop build has a keyboard and a
	// mouse.
	p->keyboard = true;
	p->mouse = true;
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
	VID_ACTIVE_KEYBOARD  // sustained typing on a listed keyboard
}
vid_active_t;

static vid_active_t vid_active = VID_ACTIVE_NONE;

// A listed mouse or touchpad has been used since it was last plugged in.
// Confirmation only: it never shows or hides the controls, and without a
// mouse in the listing nothing sets it.
static qbool vid_mouse_confirmed;

static void vid_presence_refresh(void);
static qbool vid_presence_settled(void);
#ifdef VID_LINUX_PROBES
static qbool vid_linux_poll(void);
static qbool vid_pens_open(void);
#endif

// Hysteresis. Touch wins at once; the keyboard has to be used for a moment,
// with no finger on the glass, before it takes the controls away.
#define VID_KBM_WINDOW        2.0   // seconds the presses below must fit in
#define VID_KBM_PRESSES       3     // distinct play-key presses
#define VID_TOUCH_QUIET       1.5   // seconds after the last finger event
#define VID_PEN_QUIET         1.0   // pen hover drives the pointer too

static double vid_touch_last = -1000;
static double vid_kbm_start = -1000;
static int vid_kbm_presses;

static const char *vid_mode_name(int mode)
{
	return mode <= 0 ? "keyboard & mouse" : (mode >= 2 ? "touch" : "dynamic");
}

// Automatic switches only; the shortcuts always say what they did.
static void vid_notify(const char *msg)
{
	if (msg && vid_touchscreen_notify.integer)
		SCR_Toast(msg);
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
	vid_notify(msg);
}

static void vid_kbm_reset(void)
{
	vid_kbm_start = -1000;
	vid_kbm_presses = 0;
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

// A play key. It counts towards hiding the controls only while they are on
// in Auto, the system lists a keyboard (so this is that keyboard, not an
// on-screen one) and a mouse or touchpad to aim with, and no finger is on
// the glass.
void VID_NoteKeyboardUse(void)
{
	const vid_presence_t *p;

	if (vid_active == VID_ACTIVE_KEYBOARD || !vid_touchscreen.integer || vid_touchscreen_mode.integer != 1)
		return;
	// A finger is down or has just lifted: keys now are an elbow on a
	// folded cover.
	if (host.realtime - vid_touch_last < VID_TOUCH_QUIET)
	{
		vid_kbm_reset();
		return;
	}
	// No keyboard the system knows of (none attached, or one folded behind
	// the screen): whatever sent this, the player cannot type on it. And a
	// keyboard alone leaves nothing to aim with, so the controls stay.
	p = vid_presence_get();
	if (!p->keyboard || !p->mouse)
		return;
	if (host.realtime - vid_kbm_start > VID_KBM_WINDOW)
	{
		vid_kbm_reset();
		vid_kbm_start = host.realtime;
	}
	if (++vid_kbm_presses < VID_KBM_PRESSES)
		return;
	vid_active = VID_ACTIVE_KEYBOARD;
	vid_reapply("keyboard used", NULL, "Keyboard in use: touch controls hidden");
	vid_kbm_reset();
}

// Real (not touch-emulated) pointer motion or a click. It confirms the mouse
// or touchpad the system lists, so the game can aim with it next to the
// touch controls. It never hides them: a mouse says nothing about a
// keyboard, and fingers keep working alongside it.
void VID_NoteMouseUse(float travel, qbool press)
{
	if (vid_mouse_confirmed || !vid_presence_get()->mouse)
		return;
	if (!press && travel <= 0)
		return;
	// X11 does not mark the pointer events it emulates from a finger.
	if (host.realtime - vid_touch_last < VID_TOUCH_QUIET)
		return;
#ifdef VID_LINUX_PROBES
	// Wayland hands a pen to SDL as a mouse. Catch up on the pen's own
	// events before counting this as a mouse.
	if (vid_pens_open() && vid_linux_poll())
		vid_presence_refresh();
	if (host.realtime - vid_pen_last < VID_PEN_QUIET)
		return;
#endif
	vid_mouse_confirmed = true;
	Con_Printf("Input: mouse in use (the system lists one)\n");
}

qbool VID_TouchMouseConfirmed(void)
{
	return vid_mouse_confirmed && vid_presence_get()->mouse;
}

qbool VID_HasKeyboard(void)
{
	return vid_presence_get()->keyboard;
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
	// A mouse that went away has to be used again once it is back. Without
	// one, keyboard play leaves nothing to aim with, so presence decides.
	if (old->mouse != now->mouse)
	{
		vid_mouse_confirmed = false;
		if (!now->mouse && vid_active == VID_ACTIVE_KEYBOARD)
			vid_active = VID_ACTIVE_NONE;
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
	vid_notify(msg);
}

// Recompute presence and react if the answer moved.
static void vid_presence_refresh(void)
{
	vid_presence_t old = vid_presence;
	qbool had = vid_presence_valid;

	vid_presence_valid = false;
	vid_presence_get();
	if (had && (old.keyboard != vid_presence.keyboard || old.touch != vid_presence.touch
		|| old.tablet_mode != vid_presence.tablet_mode || old.mouse != vid_presence.mouse))
		vid_presence_changed(&old, &vid_presence);
	// On Android presence settles when the activity first reports, and that
	// report may change nothing: take the launch-time answer then, not at
	// whatever input happens to come next.
	if (!vid_touchscreen_live.integer && vid_fixed < 0 && vid_presence_settled())
		VID_ApplyTouchscreenMode();
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

static void VID_TouchscreenToggle_f(cmd_state_t *cmd);
static void VID_TouchscreenLock_f(cmd_state_t *cmd);

void VID_TouchDetect_Init(void)
{
	Cvar_RegisterVariable(&vid_touchscreen_mode_rev);
	Cmd_AddCommand(CF_CLIENT, "vid_touchscreen_toggle", VID_TouchscreenToggle_f, "switch between the touch controls and keyboard-and-mouse, and lock it there until vid_touchscreen_lock or a restart (Ctrl+Alt+1)");
	Cmd_AddCommand(CF_CLIENT, "vid_touchscreen_lock", VID_TouchscreenLock_f, "lock the touch controls as they are, or unlock them (Ctrl+Alt+2); 1 locks, 0 unlocks, no argument toggles");
#ifdef __ANDROID__
	// Controls on until the activity reports a keyboard; Auto decides from
	// there.
	vid_touch_applying = true;
	Cvar_SetValueQuick(&vid_touchscreen, 1);
	vid_touch_applying = false;
#endif
}

// What Dynamic says right now: the last input used, else presence.
static int vid_dynamic_want(qbool has_touch, qbool touch_only)
{
	if (vid_active == VID_ACTIVE_TOUCH)
		return 1;
	if (vid_active == VID_ACTIVE_KEYBOARD)
		return 0;
	if (vid_touchscreen_touchonly.integer)
		return touch_only ? 1 : 0;
	return has_touch ? 1 : 0;
}

// Presence is worth deciding on: the hot-plug watch is up, so SDL's window
// and the system listing have been read (and on Android the activity has
// spoken; until then a phone is only assumed to be touch).
static qbool vid_presence_settled(void)
{
	if (!vid_hotplug_init)
		return false;
#ifdef __ANDROID__
	if (!(__atomic_load_n(&vid_android_state, __ATOMIC_ACQUIRE) & VID_ANDROID_VALID))
		return false;
#endif
	return true;
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
	Cvar_SetValueQuick(&vid_keyboard_detected, vid_presence_get()->keyboard ? 1 : 0);

	mode = vid_touchscreen_mode.integer;
	// Decided at launch: take Dynamic's answer once presence has settled
	// (on Android, once the activity has reported), then keep it.
	if (!vid_touchscreen_live.integer && vid_fixed < 0 && vid_presence_settled())
	{
		vid_fixed = vid_dynamic_want(has_touch, touch_only);
		Con_Printf("Input: decided at launch, touch controls %s for this session\n", vid_fixed ? "on" : "off");
	}
	if (vid_locked >= 0)
		want = vid_locked;
	else if (mode <= 0)
		want = 0;
	else if (mode >= 2)
		want = 1;
	else if (!vid_touchscreen_live.integer && vid_fixed >= 0)
		want = vid_fixed;
	else
		want = vid_dynamic_want(has_touch, touch_only);

	if (vid_touchscreen.integer != want)
	{
		Cvar_SetValueQuick(&vid_touchscreen, want);
		Con_Printf("Touch controls: %s (mode=%s%s, hardware touch=%s, touch-only=%s, last input=%s)\n",
			want ? "on" : "off",
			vid_mode_name(mode),
			vid_locked >= 0 ? ", locked" : (mode == 1 && !vid_touchscreen_live.integer) ? ", decided at launch" : "",
			has_touch ? "yes" : "no",
			touch_only ? "yes" : "no",
			vid_active == VID_ACTIVE_TOUCH ? "touch" : vid_active == VID_ACTIVE_KEYBOARD ? "keyboard" : "none");
	}
	vid_touch_applying = false;
}

void VID_TouchscreenMode_c(cvar_t *var)
{
	static int last_mode = -1;
	if (vid_touch_applying)
		return;
	// Picking a mode is the player deciding, so it takes effect now rather
	// than waiting behind a Ctrl+Alt lock.
	if (var == &vid_touchscreen_mode && var->integer != last_mode)
	{
		last_mode = var->integer;
		if (vid_locked >= 0)
		{
			vid_locked = -1;
			Con_Printf("Input: mode set to %s, lock released\n", vid_mode_name(var->integer));
		}
	}
	VID_ApplyTouchscreenMode();
}

void VID_Touchscreen_c(cvar_t *var)
{
	if (vid_touch_applying)
		return;
	// Direct console / config writes to vid_touchscreen become an explicit mode.
	vid_locked = -1;
	vid_touch_applying = true;
	if (var->integer)
		Cvar_SetValueQuick(&vid_touchscreen_mode, 2);
	else
		Cvar_SetValueQuick(&vid_touchscreen_mode, 0);
	vid_touch_applying = false;
}

// Turning live switching off keeps what is showing now; turning it on hands
// the decision back to Dynamic.
// (Callbacks run on every set, changed or not: a config re-setting the same
// value must not take a new launch answer.)
void VID_TouchscreenLive_c(cvar_t *var)
{
	static int last = -1;
	if (var->integer == last)
		return;
	last = var->integer;
	vid_fixed = -1;
	if (vid_touch_applying)
		return;
	VID_ApplyTouchscreenMode();
}

// The shortcuts are the player asking, so they always say what they did,
// vid_touchscreen_notify or not, and how to undo it.
static void vid_lock_to(int on, qbool switched)
{
	vid_locked = on ? 1 : 0;
	VID_ApplyTouchscreenMode();
	if (switched)
		SCR_Toast(on ? "Touch controls on, locked (Ctrl+Alt+2 unlocks)" : "Keyboard & mouse, locked (Ctrl+Alt+2 unlocks)");
	else
		SCR_Toast(on ? "Touch controls locked on (Ctrl+Alt+2 unlocks)" : "Keyboard & mouse locked (Ctrl+Alt+2 unlocks)");
	Con_Printf("Input: %s, touch controls locked %s until Ctrl+Alt+2 or a restart\n", switched ? "switched by hand" : "locked by hand", on ? "on" : "off");
}

static void vid_unlock(void)
{
	int mode = vid_touchscreen_mode.integer;
	const char *msg;

	// Dynamic carries on from what is showing: the next finger, typing or
	// device change moves it, not the act of unlocking.
	if (vid_locked >= 0 && mode == 1 && vid_touchscreen_live.integer)
	{
		vid_active = vid_locked ? VID_ACTIVE_TOUCH : VID_ACTIVE_KEYBOARD;
		vid_kbm_reset();
	}
	vid_locked = -1;
	VID_ApplyTouchscreenMode();
	if (mode <= 0)
		msg = "Unlocked: back to Keyboard & mouse mode";
	else if (mode >= 2)
		msg = "Unlocked: back to Touch mode";
	else if (!vid_touchscreen_live.integer)
		msg = "Unlocked: back to the launch choice";
	else
		msg = "Unlocked: switching automatically";
	SCR_Toast(msg);
	Con_Printf("Input: %s (touch controls %s)\n", msg, vid_touchscreen.integer ? "on" : "off");
}

// Ctrl+Alt+1: switch between the touch controls and keyboard-and-mouse, and
// hold it there.
void VID_TouchscreenToggle(void)
{
	vid_lock_to(!vid_touchscreen.integer, true);
}

// Ctrl+Alt+2: hold what is showing, or let go of a hold.
void VID_TouchscreenLockToggle(void)
{
	if (vid_locked >= 0)
		vid_unlock();
	else
		vid_lock_to(vid_touchscreen.integer != 0, false);
}

static void VID_TouchscreenToggle_f(cmd_state_t *cmd)
{
	(void)cmd;
	VID_TouchscreenToggle();
}

static void VID_TouchscreenLock_f(cmd_state_t *cmd)
{
	if (Cmd_Argc(cmd) < 2)
		VID_TouchscreenLockToggle();
	else if (atoi(Cmd_Argv(cmd, 1)))
		vid_lock_to(vid_touchscreen.integer != 0, false);
	else if (vid_locked >= 0)
		vid_unlock();
}

void VID_TouchscreenRescan_f(cmd_state_t *cmd)
{
	const vid_presence_t *p;
	(void)cmd;
#ifdef VID_LINUX_PROBES
	vid_linux_refresh(true);
#endif
	// Asked for by hand, so a launch-time answer is taken again.
	vid_fixed = -1;
	vid_presence_refresh();
	VID_ApplyTouchscreenMode();
	p = vid_presence_get();
	Con_Printf("vid_touchscreen_mode %d, live %d, locked %s, vid_touchscreen %d, detected %d, touch-only %d, keyboard %d, mouse %d (%s), tablet mode %d, last input %s\n",
		vid_touchscreen_mode.integer,
		vid_touchscreen_live.integer,
		vid_locked < 0 ? "no" : vid_locked ? "on" : "off",
		vid_touchscreen.integer,
		vid_touchscreen_detected.integer,
		vid_touchscreen_touchonly_detected.integer,
		p->keyboard, p->mouse, vid_mouse_confirmed ? "used" : "not used yet", p->tablet_mode,
		vid_active == VID_ACTIVE_TOUCH ? "touch" : vid_active == VID_ACTIVE_KEYBOARD ? "keyboard" : "none");
}
