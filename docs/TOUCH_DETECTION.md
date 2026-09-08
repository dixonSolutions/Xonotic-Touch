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

1. `SDL_GetNumTouchDevices()` (after SDL video init).
2. `/proc/bus/input/devices` — `INPUT_PROP_DIRECT` or a name containing
   `touchscreen`; a real keyboard is `KEY_A` on a device that is not gpio-keys /
   power / lid / HDMI.
3. SMBIOS chassis type 11 (handheld) or 30 (tablet) counts as touch-only when a
   screen is present. Convertibles (31) and detachables (32) still need no
   keyboard.
4. Ubuntu Touch / Lomiri (`/etc/os-release`, `CLICK_FRAMEWORK`, desktop name).
5. First `SDL_FINGERDOWN` while Auto is selected re-runs the scan (SDL often
   reports zero devices until the first finger).
6. `SW_TABLET_MODE` from `/dev/input` (Flatpak: `--device=input`). Engaged
   means the built-in keyboard is folded away or detached, whatever `/proc`
   still lists. Keyboards on USB (bus 0x03) or Bluetooth (0x05) count
   regardless -- they are the player's, not the chassis'.
7. Permanently-present virtual keyboards (`keyd`, `ydotool`, `xdotool`,
   `uinput`, remote desktop) are not keyboards.

## Live hot-plug

`VID_TouchHotplugFrame()` runs from `CL_UpdateScreen` and re-reads the
hardware once a second: one read of `/proc/bus/input/devices` (about
0.1 ms) plus one ioctl per kept tablet-switch descriptor. `/dev/input` is
walked only when that listing changes -- opening every event node costs
about 0.4 s on a Surface, and doing it per second was a rhythmic render
stall. When the keyboard or touchscreen answer changes it
re-applies `vid_touchscreen_mode`: in Auto the CSQC overlay, weapon strip and
console pill go away the moment a keyboard is attached and return when it is
removed -- in the menu as well as mid-match, because every `touch_*.qc` draw
already gates on `vid_touchscreen`. A toast (`SCR_Toast`, drawn over menus
too) says *Keyboard connected: touch controls hidden* or *Keyboard
disconnected: touch controls shown*.

Testing without hardware: `scripts/fake-keyboard.py 10` creates a `uinput`
keyboard on bus 0x03 for ten seconds.

Android/iOS (`DP_MOBILETOUCH`) still force Always.

## Defaults

| Build | Mode | Touch-only filter |
|-------|------|-------------------|
| Official-style engine default | Auto (`1`) | On |
| Xonotic Touch on a tablet/phone | Auto | On |
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
