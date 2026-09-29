# Touch hardware detection

On-screen controls stay in CSQC (`client/touch_*.qc`). Whether they run is an
engine decision: detect hardware, then set `vid_touchscreen` from a user mode.

This is the same 0 / 1 / 2 model SuperTuxKart already ships as
`multitouch_active` (disabled / if available / enabled). Official STK assumes
Android/iOS, where touch is present; Linux tablets need a real scan.

## Settings

| User-facing | Cvar | Meaning |
|-------------|------|---------|
| Off / Auto / Always | `vid_touchscreen_mode` | Preference. Archived. |
| Auto-detect touch-only devices | `vid_touchscreen_touchonly` | When Auto, require a touchscreen and no physical keyboard |

Settings → Touch. **Rescan hardware** runs `vid_touchscreen_rescan`.

## Detection order

1. SDL touch devices of type `SDL_TOUCH_DEVICE_DIRECT` (after SDL video
   init). SDL on X11 also lists touchpads (XI2 dependent touch); those are
   mice and do not count.
2. `/proc/bus/input/devices` (parsed in `vid_inputscan.h`) —
   `INPUT_PROP_DIRECT` or a name containing
   `touchscreen`; a touchpad (`INPUT_PROP_POINTER`) is not a touchscreen. A
   real keyboard has `KEY_A`, `KEY_Z` and `KEY_SPACE`, is not a controller
   (`BTN_JOYSTICK` / `BTN_GAMEPAD` — a Bluetooth pad that also reports
   letters is still a pad), is not a touch surface, and is not a known button
   set (power / sleep / lid / video bus / gpio-keys / headset / HDMI / Intel
   HID / WMI hotkeys). Bitmaps are read with the kernel's word size (an armhf
   Click build on an arm64 kernel still sees 64-bit words). A mouse is
   anything with `BTN_LEFT` that moves a pointer: `REL_X` + `REL_Y` (mice,
   trackballs, trackpoints) or a touchpad (`INPUT_PROP_POINTER` with
   `BTN_TOOL_FINGER` and `ABS_X`/`ABS_Y`). Direct surfaces, controllers and
   the same virtual and lizard-mode devices as for keyboards do not count.
3. SMBIOS chassis type 11 (handheld) or 30 (tablet) counts as touch-only when a
   screen is present. Convertibles (31) and detachables (32) still need no
   keyboard.
4. Ubuntu Touch / Lomiri (`/etc/os-release`, `CLICK_FRAMEWORK`, desktop name).
5. The first direct `SDL_FINGERDOWN` counts as a touchscreen (SDL often
   reports zero devices until the first finger).
6. `SW_TABLET_MODE` from `/dev/input` (Flatpak: `--device=input`). Engaged
   means the built-in keyboard and touchpad are folded away or detached,
   whatever `/proc` still lists -- a chassis pad face-down on the table is
   nothing to aim with. Keyboards and mice on USB (bus 0x03) or Bluetooth
   (0x05) count regardless -- they are the player's, not the chassis'. The
   Surface Type Cover is the exception: its keyboard and its touchpad sit
   on USB but are chassis, so the switch can fold them away. A handheld or
   tablet chassis (3) and Ubuntu Touch (4) read the same way.
7. Permanently-present virtual keyboards and pointers (`keyd`, `ydotool`,
   `xdotool`, `uinput`, remote desktop) and Steam Deck / Steam Controller
   lizard-mode keyboards and mice are neither keyboards nor mice.

All of 2-4 and 6 is desktop Linux only. Android builds compile none of it
(no `/proc`, `/sys`, `/dev/input`, os-release, DMI or Click checks); see
Android below.

## Live hot-plug

Event driven. `VID_TouchHotplugFrame()` runs from `CL_UpdateScreen`. On an
idle frame it does one time comparison. Ten times a second it makes one
zero-timeout `poll()` over:

- inotify on `/dev/input` (`IN_CREATE | IN_DELETE | IN_ATTRIB`; `IN_ATTRIB`
  is udev granting access to a node it just created);
- a `NETLINK_KOBJECT_UEVENT` socket filtered to `SUBSYSTEM=input`;
- the kept `SW_TABLET_MODE` descriptors, whose `EV_SW` events carry fold and
  unfold directly (`SYN_DROPPED` re-reads with `EVIOCGSW`);
- the kept screen-pen descriptors (`BTN_TOOL_PEN` + `INPUT_PROP_DIRECT`).

A node event schedules one re-read of `/proc/bus/input/devices` 0.15 s
later, so a device's burst of nodes settles first. `/proc` names the
`eventN` node of every switch and pen, so only those nodes are opened, and
descriptors for devices that are still there are kept. `/dev/input` is
never walked: that took about 0.4 s on a Surface. Measured on this
laptop, the poll costs 0.5 µs and a `/proc` read 55-65 µs. Where neither
watch is allowed (strict confinement), `/proc` is re-read every 3 s instead.

When the answer changes, `vid_touchscreen_mode` is applied again. In Auto,
the overlay, weapon strip and console pill come and go in the menu and
mid-match. That covers the CSQC HUD and the engine-drawn server HUD
(`touch_hud.c`), since both gate on `vid_touchscreen`. A toast
(`SCR_Toast`, drawn over menus too) says *Keyboard connected: touch controls
hidden*, *Keyboard disconnected: touch controls shown*, *Tablet mode: touch
controls shown* or *Laptop mode: touch controls hidden*.

## Last active input

Presence answers "what is attached", and it is the source of truth. Auto
mode also asks "what is the player using", which is how games switch their
button prompts. Input only ever confirms a device presence already lists:
a key with no keyboard listed came from an on-screen keyboard, a remote or
a mouse's macro button, and changes nothing.

| Evidence | Effect |
|----------|--------|
| Direct `SDL_FINGERDOWN`, or a pen tip on the screen (evdev `BTN_TOUCH` on a screen pen) | Controls **on at once**, even with a keyboard attached (*Touch detected: touch controls shown*) |
| 3 play keys within 2 s, in the game, from a listed keyboard, with a listed mouse or touchpad | Controls off (*Keyboard in use: touch controls hidden*) |
| Real pointer motion or a click from a listed mouse or touchpad | **Controls stay.** The mouse is confirmed (*Input: mouse in use*) and aims next to them |

A mouse never hides the controls: it says nothing about a keyboard, and
fingers keep working alongside it. Mouse and touch are used together:

- A real mouse's buttons always reach the game (`K_MOUSE*`), touch controls
  or not. Only the clicks SDL makes up from a finger
  (`SDL_TOUCH_MOUSEID`) are dropped while the finger path handles that
  finger. On Android, `SDL_HINT_MOUSE_TOUCH_EVENTS` is off and SDL's
  `mouse_input` touch device (`SDL_MOUSE_TOUCHID`) is not a touchscreen,
  so a mouse is no longer turned into fingers.
- In the game, once a listed mouse has been used, the engine grabs it for
  mouselook next to the touch controls (`VID_TouchMouselook` in
  `vid_sdl.c`): the stick moves, the mouse aims, both can fire.
  `vid_touchscreen_mouselook` tells the touch CSQC that `MOUSE1` is fire
  rather than a press on its controls. It needs a real touchscreen (without
  one the mouse stands in for a finger: desktop testing), `key_game`, and
  no modal touch sheet: the CSQC sets `_touch_owns_screen` while the chat
  sheet or edit mode is up, and the mouse goes back to being a pointer.
- When a click is what confirms the mouse, the grab is taken before that
  click is handed to the game (`VID_UpdateMouseGrab`), so the first press
  after a mouse is plugged in fires rather than landing on whatever control
  sits under the ungrabbed cursor.

Keyboard evidence is ignored:
- within 1.5 s of any finger event, or while a finger is down (keys then
  are an elbow on a folded cover);
- when it is a key repeat, Escape/Back, a function, media or power key, or
  Super;
- outside the game, or while a touch sheet owns the screen: typing a
  message is not playing on the keyboard, and the controls must not vanish
  halfway through a word;
- in tablet mode with no usable keyboard (keys from the folded cover);
- without a listed mouse or touchpad: a keyboard alone leaves nothing to
  aim with.

Mouse evidence is ignored when it is synthetic touch-mouse input
(`which == SDL_TOUCH_MOUSEID`), within 1.5 s of a finger (X11 does not mark
the pointer events it emulates), and within 1 s of pen activity (Wayland
hands a pen to SDL as a mouse).

Until something is used, presence decides. Plugging a keyboard in or
removing it starts over from presence; a mouse that is unplugged has to be
used again once it is back. *Off* and *Always* ignore all of this.

## On-screen keyboard

Only when the system lists no keyboard (`vid_keyboard_detected 0`). With
one, text input stays on so its characters arrive, the console sheet shows
the COMMANDS palette without a KEYS tab, the engine chat sheet keeps just
its quick phrases, and the touch CSQC chat sheet drops its glass keys and
takes typing from the keyboard instead. With none, console and chat also
ask the platform for its own keyboard (`SDL_StartTextInput`: the Android
IME, GNOME's and Lomiri's OSK through Wayland text-input).

## Android

`XonoticActivity` registers an `InputManager.InputDeviceListener`. It also
re-checks in `onResume` and `onConfigurationChanged`: `keyboard` and
`keyboardHidden` are in `configChanges`, so a keyboard being folded away
arrives there. It passes touch / keyboard / mouse to the engine through
`nativeInputDevices` (JNI, in `vid_touchdetect.c`). The frame reads one
int. A keyboard counts when it is `SOURCE_KEYBOARD` with
`KEYBOARD_TYPE_ALPHABETIC`, not `isVirtual()`, not a gamepad or joystick,
not a fingerprint reader, GPIO button or hall sensor by name, and not
reported hidden (`hardKeyboardHidden == YES`). A mouse counts when it is
`SOURCE_MOUSE`, `SOURCE_TOUCHPAD` or `SOURCE_MOUSE_RELATIVE`, not a
touchscreen or stylus, and not one of those sensors by name. A hidden
keyboard takes its own touchpad with it: a pad reporting the vendor and
product of a listed keyboard is part of that folded unit, while a mouse of
its own stays. Disabled devices (`isEnabled()`, Android 8.1+) count for
nothing. No permission is involved.

The Android build no longer forces Always. It starts with the controls on,
and Auto keeps them on unless a real keyboard is attached or one is
actually played on with a mouse to aim; a touch brings them straight back. Old builds
archived `vid_touchscreen_mode 2`. `vid_touchscreen_mode_rev` moves that
to Auto once, and an Always the player picks later sticks. Relative mouse
mode (pointer capture, Android 8+) is enabled so keyboard-and-mouse play
has mouselook. iOS still forces Always.

## Testing

`scripts/test-input-presence.sh` feeds recorded `/proc/bus/input/devices`
listings (`tests/input-presence/fixtures/`: a laptop, a Surface Pro 9, a
phone, a desktop with a gamepad and keyd, a tablet with a Bluetooth mouse)
to `vid_inputscan.h` and checks touch, keyboard, mouse, tablet switch and
pen. CI runs it on every push (`.github/workflows/tests.yml`), and a
release does not publish without it.

The whole loop runs in a headless GNOME session (`gnome-shell --headless
--wayland --virtual-monitor`) driven through
`org.gnome.Mutter.RemoteDesktop` with a ScreenCast `RecordMonitor` stream.
Mutter creates each virtual device on its first event and the seat only
gains touch or a pointer then, so send one of each and wait a few seconds
before the input that counts.

`scripts/fake-keyboard.py 10` plugs a `uinput` keyboard on bus 0x03 in for
ten seconds (presence only; it presses nothing).

Do not inject key presses or taps into a live desktop session. They land in
whatever window has focus. keyd grabs new keyboards and re-emits them too.
Use an isolated X server instead: `/usr/lib/xorg/Xorg :77` with the
`dummy` video driver, `AutoAddDevices false`, and three
`xf86-input-inputtest` devices (Keyboard, Pointer, Touch). Its socket
protocol drives SDL's real X11 key, mouse and XI2 touch paths without
touching the desktop. Hot-plug presence can still use `uinput` devices
that emit nothing the desktop acts on: a touchscreen that never touches, a
tablet-mode switch, a keyboard that never types, a pen without
`ABS_X`/`ABS_Y`, which libinput ignores.

## Defaults

| Build | Mode | Touch-only filter |
|-------|------|-------------------|
| Official-style engine default | Auto (`1`) | On |
| Xonotic Touch on a tablet/phone | Auto | On |
| Android APK | Auto (old archived Always migrated once) | On |
| Desktop test window (`XONOTIC_DESKTOP_DEV`) | Always | On (ignored) |

`touch/xonotic.cfg` no longer sets `vid_touchscreen 1`. The launcher does not
pass `+vid_touchscreen 1`. Override with `XONOTIC_TOUCH_MODE=always|auto|off`.

## Why not always-on in the Touch port

The overlay is the product on a phone. On a laptop with a keyboard it steals
the pointer and hides the cursor. Auto + touch-only is the dynamic basis;
Always remains one tap away for convertibles and for testers without a panel.

## Upstream

DarkPlaces already had `vid_touchscreen` (Android-oriented, default off). This
layer is the part that belongs in official `darkplaces` even without the CSQC
arena overlay: Linux tablets can opt in without affecting desktop players.
The overlay itself stays a follow-up for `xonotic-data.pk3dir`.

- Engine PR (GitHub mirror): https://github.com/xonotic/darkplaces/pull/6
- Contact: `admin@xonotic.org` and Matrix `#dev:xonotic.org` (GitLab is the
  canonical forge; a GitLab MR can follow if the team wants it there).
