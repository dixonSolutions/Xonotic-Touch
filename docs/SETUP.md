# First-run setup and touch UX

Xonotic Touch uses a **guided startup chain** on the main menu, **in-game asset download progress**, touch-native menus (no on-screen pointer), and **system keyboard** integration on GNOME/Wayland.

## Branding

| Surface | Value |
|---------|--------|
| Window title | `Xonotic Touch` (`-customgamename` from launcher / `run-local.sh`) |
| Network / version cvar | `g_xonoticversion` → `"Xonotic Touch"` in `touch/xonotic.cfg` |
| Setup dialogs | Titles use **Xonotic Touch** (not generic “Welcome” or engine build strings) |

Engine build watermarks (`WATERMARK`, `buildstring`) remain for developer logs only; they are not shown in the touch setup UI.

## Startup wizard chain

On the first main-menu frame, dialogs open **in order**; each step is skipped when already complete.

| Step | Dialog | `shouldShow()` when |
|------|--------|-------------------|
| 1 | Terms of Service | `_termsofservice_accepted` &lt; server ToS version |
| 2 | Asset download | Touch + assets not ready (see below) |
| 3 | Profile setup (`FirstRun`) | `_cl_name` still default (empty) |
| 4 | Touch setup (`TouchSetup`) | `vid_touchscreen` and `touch_setup_done` is `0` |

Implementation: `MainWindow_tryOpenStartupDialogs()` in `qcsrc/menu/xonotic/mainwindow.qc`.

After ToS accept or asset download completes, `main.firstDraw = true` re-runs the chain so the next step opens automatically.

### Skip conditions (returning users)

| User state | Skipped steps |
|------------|----------------|
| Assets already on disk | Asset download dialog |
| Player name saved (`_cl_name` non-empty) | Profile setup |
| `touch_setup_done 1` in config / `touch.layout.cfg` | Touch setup |
| ToS already accepted | ToS dialog |

## Asset download (first launch)

### Launcher behavior

`packaging/start.sh` (and dev `xonotic_touch_begin_asset_fetch()` in `scripts/lib/xonotic-shlib.sh`):

1. Sync slim bundle into the user data directory.
2. If assets are **missing**, start `xonotic_fetch_game_assets` in a **background** shell job (game launches immediately).
3. Pass engine flags: `_touch_asset_fetch_active`, `_touch_assets_ready`.

The game is **not** blocked on a terminal download anymore.

### Progress file

While downloading, the shell writes `data/.asset-fetch-progress` (three lines):

```
running|done|error
0–100
Human-readable status message
```

Menu QC polls this file each frame in `XonoticTouchAssetFetchDialog` and draws a progress bar.

### Ready marker

When all required packs are present, `scripts/lib/asset-fetch.sh` creates `data/.assets-ready`. The download dialog closes and the chain continues to profile / touch setup.

Detection logic (same as fetch): core `xonotic-data.pk3dir` asset dirs or matching `.pk3` files, plus maps, music, and nexcompat packs. See `xonotic_assets_need_fetch()` in `scripts/lib/asset-fetch.sh`.

### Environment

| Variable | Purpose |
|----------|---------|
| `XONOTIC_ASSET_FETCH_PROGRESS` | Path to progress file (set by launcher) |
| `XONOTIC_SKIP_ASSET_FETCH=1` | Skip all fetch (testing) |
| `XONOTIC_TOUCH_DATA_DIR` | Data directory for `fetch-assets-runtime.sh` |

## Touch-only menus (no cursor)

On touch devices (`vid_touchscreen 1`):

- **Engine:** Menus use direct finger position (`VID_SyncTouchFinger` + full-screen tap), not the SteelStorm grab-and-drag puck (`touch_puck_cur_*`).
- **Menu QC:** `draw_drawMousePointer` is hidden; `menu_mouse_absolute 1` maps taps to controls.
- **In-game HUD:** `HUD_Cursor_Show` does not draw pointer sprites (quick menu etc. still use touch position).

Config: `menu_mouse_absolute 1` in `touch/xonotic.cfg`.

## On-screen keyboard (GNOME / Wayland)

When a menu **input box** is focused:

- `vid_touchscreen_showkeyboard` is set to `1`.
- Engine calls `SDL_StartTextInput()` and `SDL_SetTextInputRect()` from the field’s screen rect (even when `SDL_HasScreenKeyboardSupport()` is false — required on many Linux compositors).
- On focus leave or menu hide, keyboard is dismissed.

**GNOME:** Enable **Settings → Accessibility → Typing → Screen Keyboard** (or equivalent) so the compositor shows the OSK when text input is active.

Cvars (menu sets position when focusing inputs):

| Cvar | Purpose |
|------|---------|
| `vid_touchscreen_textinput_x/y/w/h` | Console-pixel rect for OSK placement |
| `vid_touchscreen_showkeyboard` | Request OSK from menu QC |
| `vid_touchscreen_supportshowkeyboard` | Read-only; `1` when touch mode expects OSK |

## Key source files

| Area | Path |
|------|------|
| Startup chain | `qcsrc/menu/xonotic/mainwindow.qc` |
| Asset download UI | `qcsrc/menu/xonotic/dialog_touch_asset_fetch.qc` |
| Skip / progress helpers | `qcsrc/menu/xonotic/touch_startup_util.qc` |
| Profile wizard | `qcsrc/menu/xonotic/dialog_firstrun.qc` |
| Touch wizard | `qcsrc/menu/xonotic/dialog_touch_wizard.qc` |
| Asset fetch + progress | `scripts/lib/asset-fetch.sh` |
| Packaged launcher | `packaging/start.sh` |
| Dev launcher | `scripts/lib/xonotic-shlib.sh` (`xonotic_run_native`) |
| Touch input / keyboard | `engine/darkplaces/vid_sdl.c` |
| Menu input + OSK hooks | `qcsrc/menu/item/inputbox.qc`, `qcsrc/menu/menu.qc` |

## Testing checklist

1. **Clean install (no assets):** Flatpak launch → ToS (if needed) → download dialog with moving progress → profile → touch preset → main menu.
2. **Assets present:** No download dialog; only missing profile/touch steps.
3. **Fully configured user:** Only ToS if version bumped; otherwise straight to touch home.
4. **Input field:** Tap name field → GNOME OSK appears; typed text enters the box.
5. **Menu navigation:** No puck cursor; taps hit buttons directly.

See also [TESTING.md](TESTING.md) and [CONTROLS.md](CONTROLS.md).
