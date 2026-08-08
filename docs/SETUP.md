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
| 3 | Profile setup (`FirstRun`) | `touch_profile_done` is `0` and name/stats incomplete |
| 4 | Touch setup (`TouchSetup`) | `vid_touchscreen` and `touch_setup_done` is `0` |

Implementation: `MainWindow_tryOpenStartupDialogs()` in `qcsrc/menu/xonotic/mainwindow.qc`.

After ToS accept or asset download completes, `main.firstDraw = true` re-runs the chain so the next step opens automatically.

### Skip conditions (returning users)

| User state | Skipped steps |
|------------|----------------|
| Assets already on disk | Asset download dialog |
| `touch_profile_done 1` (or legacy: custom name + stats Yes/No) | Profile setup |
| `touch_setup_done 1` in config / `touch.layout.cfg` | Touch setup |
| ToS already accepted | ToS dialog |

## Asset download (first launch)

### Asset resolution (our app owns everything)

Xonotic Touch always runs from **its own** user data directory (user data + game packs). Resolution order — stop at the first success:

| Step | Condition | Action |
|------|-----------|--------|
| 1 | Assets already in our app data (`.assets-ready` / complete packs) | Use them. **No download. No import** from any other Xonotic install. |
| 2 | Original Flatpak `org.xonotic.Xonotic` present | **Copy** (duplicate) packs into our data dir. Never hardlink, symlink, or use the Flatpak folder in place. |
| 3 | Otherwise | Download via the in-game wizard (progress UI). |

Implemented in `scripts/lib/asset-discover.sh` (steps 1–2) and `packaging/start.sh` (step 3). Native `~/.xonotic` and Debian `/usr/share` installs are **not** import sources.

A **fast** ready-check runs before launch. Flatpak copy and download run in the **background** and drive the fullscreen setup wizard via `data/touch/asset-progress.txt`. If our data is already ready, the wizard never appears. Gameplay stays blocked until `.assets-ready` exists.

### Boot assets (why the package is not empty)

The wizard is drawn by the engine, so the package has to contain enough art for the menu to render before anything is downloaded. `scripts/fetch-boot-assets.sh` stages that subset — the `luma` menu skin plus console and loading graphics, ~31 MB — into `xonotic-data.pk3dir/gfx`, and `scripts/stage-slim-data.sh` calls it after stripping the large media directories. Both `stage-flatpak.sh` and `stage-click.sh` inherit it from there; `XONOTIC_SKIP_BOOT_ASSETS=1` opts out for offline builds. The script copies from a local `engine/data/xonotic-data.pk3dir` when one exists, otherwise it pulls just those paths from upstream with a blob-filtered sparse checkout.

Slim staged data is ~92 MB, against ~3 GB for a full install.

### Profile persistence

The packaged launcher re-execs `config.cfg` / `autoexec.cfg` **after** `xonotic.cfg`. Without that, `xonotic-client.cfg`’s `_cl_name ""` would clear the saved name on every start and reopen the profile wizard. Saving profile settings also sets `touch_profile_done 1`.

### Launcher behavior

`packaging/start.sh` (and dev `xonotic_touch_begin_asset_fetch()` in `scripts/lib/xonotic-shlib.sh`):

1. Sync slim bundle into the user data directory.
2. If **our** data is already ready → skip import/download.
3. Else if **original Flatpak Xonotic** exists → copy packs into our data.
4. Else seed the progress file, start `xonotic_resolve_missing_assets` in a background job, and show the blocking in-menu progress dialog.
5. Pass engine flags: `_touch_asset_fetch_active`, `_touch_assets_ready`.
6. Re-run the engine when the wizard asks for it (see **Relaunch**).

The engine launches for the setup UI, but **Play** and other game actions wait until assets are ready.

### Progress file

While resolving, the shell writes `data/touch/asset-progress.txt` (three lines):

```
discover|running|done|error
0–100
Human-readable status message
```

The `discover` phase covers “our data ready?” then “copy from Flatpak Xonotic?”; the wizard shows a sweeping bar for it, because neither step can report a percentage. The bar becomes a real percentage in the `running` download phase, and turns red with a **Try again** button on `error`.

The launcher seeds this file *before* exec so the wizard is never blank, and never deletes it — the background worker overwrites it in place.

The name matters: DarkPlaces' `FS_CheckNastyPath` rejects every path with a leading dot, so the menu cannot read a `.asset-fetch-progress`. Anything the engine has to open lives under `touch/` with a plain name.

### Ready marker

When all required packs are present, `scripts/lib/asset-fetch.sh` creates `data/.assets-ready`. Only the shell reads it; the menu learns readiness from the `_touch_assets_ready` cvar the launcher passes in, and from a `done` status in the progress file.

### Relaunch

Packs that appear after the engine has built its search path stay invisible to it, so the wizard does not try to hot-load them. On success — and on **Try again** after a failure — it writes `data/touch/relaunch-request.txt` and quits; `packaging/start.sh` loops, sees the marker, removes it, re-runs asset preparation and starts the engine again. Assets are ready by then, so the wizard does not reappear and the chain continues to profile / touch setup. When the marker cannot be written (unpackaged run), the wizard falls back to `fs_rescan` in place.

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

When a menu **input box** is focused, or the **console / chat** is open:

- Menu focus sets `vid_touchscreen_showkeyboard` to `1`; console/chat pulse `SDL_StartTextInput()` on enter and keep it active.
- Console also draws an **in-engine keyboard** (letters/digits/SPACE/BKSP/ENTER) so typing works even when the compositor OSK is suppressed (common on GNOME when a virtual hardware keyboard such as `keyd` is present).
- Engine calls `SDL_SetTextInputRect()` for platform OSK placement (GNOME / Ubuntu Touch).

**GNOME (Ultramarine):** Enable **Settings → Accessibility → Typing → Screen Keyboard**. Platform OSK may still stay hidden if a hardware/virtual keyboard is attached — use the in-engine console keyboard.

**Ubuntu Touch:** lomiri-keyboard via Wayland text-input; in-engine keyboard is always available as fallback.

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
| Local install discovery | `scripts/lib/asset-discover.sh` |
| Touch-friendly menu buttons | `qcsrc/menu/xonotic/touchbutton.qc` |
| Packaged launcher | `packaging/start.sh` |
| Dev launcher | `scripts/lib/xonotic-shlib.sh` (`xonotic_run_native`) |
| Touch input / keyboard | `engine/darkplaces/vid_sdl.c` |
| Menu input + OSK hooks | `qcsrc/menu/item/inputbox.qc`, `qcsrc/menu/menu.qc` |

## Testing checklist

1. **Clean install (no assets):** Flatpak launch → ToS (if needed) → download dialog with moving progress → profile → touch preset → main menu.
2. **Assets present:** No download dialog; only missing profile/touch steps.
3. **Fully configured user:** Only ToS if version bumped; otherwise straight to the stock Xonotic main menu.
4. **Input field:** Tap name field → GNOME OSK appears; typed text enters the box.
5. **Menu navigation:** No puck cursor; taps hit buttons directly.

See also [TESTING.md](TESTING.md) and [CONTROLS.md](CONTROLS.md).
