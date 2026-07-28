# Ultramarine touch test — 2026-07-25

Target: `borysthebear@100.125.7.103` (Tailscale `ultramarine`, Surface, GNOME Wayland)  
Build under test: Flatpak `io.github.dixonSolutions.XonoticTouch` **1.1.1** (`3125de8`, 2026-07-03)  
Remote clone: `~/Projects/Xonotic-Touch` (same commit) for fast local edits

## Verdict

| Area | Status | Notes |
|------|--------|--------|
| App update / launch | **Pass** | Flatpak 1.1.1; fixed launcher re-execs user config |
| Assets | **Pass** | `.assets-ready`; **1.2 GB** packs |
| Profile persistence | **Fixed** | Was wiped by `xonotic-client.cfg` `_cl_name ""` after `config.cfg`; launcher now `+exec config.cfg` / `autoexec.cfg` after defaults. `touch_profile_done` gate added. |
| Home / Play menu | **Fixed** | Touch home no longer overwritten by Singleplayer at the same nexposee slot; Play exposes Campaign / Multiplayer / Profile / touch settings |
| Touch console | **Pass** | Confirmed in menu and in map |
| Join → freelook | **Partial** | Remote mouse injection still flaky for Join |

### Root causes found (2026-07-25 evening)

1. **Setup every launch:** `+exec xonotic.cfg` → `xonotic-client.cfg` sets `_cl_name ""` *after* archived config loads, clearing the name so FirstRun always returns.
2. **Incomplete home:** `mainwindow.qc` registered Touch Home and Singleplayer on the **same** nexposee position; Singleplayer won and Play looked broken/empty.
3. **Flatpak data dir:** `XDG_DATA_HOME` under Flatpak pointed assets at `~/.var/app/...` instead of `~/.local/share/xonotic-touch`; `start.sh` now pins the host path.

### WhatsApp feedback fixes (2026-07-25 afternoon)

User photos showed: portrait-held tablet with landscape UI, huge DODGE/HUD, yellow QC spam, missing Campaign, extreme lag.

| Cause | Fix |
|-------|-----|
| `prvm_traceqc 1` left on (statement trace every QC op) | Force `prvm_traceqc 0` in `autoexec`, `touch/startup.cfg`, launcher `+set` |
| `vid_touchscreen_density` multiplied into widget size (×2 on HiDPI) | Removed density from `Touch_WidgetScale()` |
| Touch profiles never applied (`startup.cfg` written but not `+exec`'d) | Launcher/`run-fixed` now `+exec touch/startup.cfg` after config |
| Screen-calc fell back to 1920×1080; greedy sed parsed `2880x1920` as `0×1920` | GNOME `gdctl` path + non-greedy WxH parse → **1440×960** @ scale 2 |
| Home looked incomplete | Compact Play home with big **Campaign** / **Multiplayer** |
| Thermal/Iris Xe lag at native 2880p | Default perf profile **battery**; render budget uses logical size |

Verified on device: home shows Campaign; Campaign opens full Singleplayer (Level 1 Boil…); resolution 1440×960; trace spam stopped after `prvm_traceqc 0`.

**Use on Ultramarine until Flatpak rebuild:** `~/xonotic-touch-test/run-fixed.sh` (override `menu.dat`/`csprogs.dat` in `zzz-touch-fix.pk3dir`). Hold tablet **landscape** (auto-rotate disabled for the session).

### Thermal mode (Surface Pro 4 — 2026-07-25)

Fanless tablet was overheating under 1440p + glass HUD. Switched defaults:

| Knob | Value |
|------|--------|
| Resolution | **960×640** (was 1440×960) |
| FPS cap | **30** + vsync |
| UI | `touch_simple_draw 1` (rects only, no glass scanlines / portrait HUD) |
| Effects | particles/decals/shadows/viewmodel off, picmip 3 |
| Power | `powerprofilesctl set power-saver` at launch |
| Perf profile | `thermal` (default) |

If it still runs hot: quit the game (`systemctl --user stop xonotic-touch-test`) and let the chassis cool before another session.

## Environment

| Item | Value |
|------|--------|
| Host | Ultramarine / Fedora, user `borysthebear` |
| Display | eDP-1 **2880×1920@120**, GNOME scale **2.0** → game **1440×960** fullscreen |
| GPU | Mesa Intel Iris Xe (ADL GT2), GL 4.6 |
| Session | Wayland `wayland-0`, `XDG_RUNTIME_DIR=/run/user/1000` |
| Tools installed | `grim`, `ydotool`, `gnome-screenshot` (grim unusable: no wlr-screencopy on GNOME) |
| Capture method | In-engine `screenshot` → `~/.xonotic/data/screenshots/*.tga` → SCP → PNG |

### Access notes

- Initial SSH was down; came up mid-session. Prefer password auth (`1122`).
- GNOME Shell Screenshot D-Bus: **AccessDenied** from SSH.
- RDP (`gnome-remote-desktop`) started on **:3389** but credentials could not be stored (login keyring locked / `grdctl` segfault).
- Interaction: `ydotool` absolute mouse at **physical** coords (`logical × 2`).

## Assets inventory

Path: `~/.local/share/xonotic-touch/data/`  
Marker: `.assets-ready` **YES**

| Pack | Size (approx) |
|------|----------------|
| `xonotic-20260618-maps.pk3` | 597 M |
| `xonotic-20260618-data.pk3` | 306 M |
| `xonotic-20260618-nexcompat.pk3` | 120 M |
| `xonotic-20260618-music.pk3` | 106 M |
| `xonotic-20260618-xoncompat.pk3` | 2.3 M |
| fonts (`unifont`, `xolonium`, …) | present |
| `xonotic-data.pk3dir` + font `*.pk3dir` | present |

Also: `autoexec.cfg` / `config.cfg` with `_cl_name "TouchAgent"`, `touch_setup_done 1`.  
Stale progress file still mentions discover/download; ignore when `.assets-ready` exists.

See `assets/assets-inventory.txt`.

## Console verification

Confirmed on-device via screenshots:

1. Menu: `echo TOUCH_CONSOLE_OK` → printed (`tagged-10-console-echo.png`)
2. In map session: `echo INGAME_TOUCH_CONSOLE_OK` → printed (`tagged-30-ingame-console-ok.png`)
3. Cvars: `vid_touchscreen` → `"1"`; `touch_setup_done` → `"1"`
4. Top-left 64×64 touch area + `` ` `` key both used to toggle console
5. `screenshot` command writes TGA under `~/.xonotic/data/screenshots/`

## Touch / CSQC

- Console traces call `Touch_Draw`, `Touch_DrawOverlay`, `Touch_DrawActionButton`, `Touch_Glass_*` (`touch_draw.qc`)
- On-screen **DODGE** circular control visible while map loaded (`tagged-41-playing-console.png`, `tagged-42-playing-hud.png`)
- Game menu exposes **Touch controls** entry

## Warnings observed (non-blocking for local play)

| Warning | Impact |
|---------|--------|
| `Engine lacks DP_CRYPTO` | No pubkey / XonStat player IDs |
| `dpmaster master1.xonotic.org … timed out` | Server browser / master list |
| `VM_drawstring: z value from pos discarded` | Minor draw warning during touch UI |
| `status` showed high `% lost` under remote test load | Perf under automation; re-check on device interactively |

## Screenshots (local copies)

Directory: `docs/test-runs/2026-07-25-ultramarine/screenshots/`

| Tag | What it shows |
|-----|----------------|
| `tagged-10-console-echo` | Console `TOUCH_CONSOLE_OK` on welcome UI |
| `tagged-14-direct-launch` | FirstRun skipped after `+set _cl_name` |
| `tagged-23` / `26` | Map load + Join/Spectate; touch draw traces |
| `tagged-30` | `INGAME_TOUCH_CONSOLE_OK`, cvars OK |
| `tagged-37` | Courtfun CTF welcome, assets OK |
| `tagged-41` / `42` | **DODGE** touch widget + PAUSE + touch draw |
| `tagged-45` / `46` | Console + Game menu + `vid_touchscreen 1` |

Full TGA/PNG set includes tags `02`–`46`.

## How the session was driven

```bash
# On ultramarine (graphical user session env):
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0
export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
export YDOTOOL_SOCKET=$XDG_RUNTIME_DIR/.ydotool_socket
ydotoold --socket-path=$YDOTOOL_SOCKET --socket-own=$(id -u):$(id -g) &

# Launch (direct binary so +set sticks):
flatpak run --command=/app/bin/xonotic io.github.dixonSolutions.XonoticTouch \
  -xonotic -customgamename "Xonotic Touch" \
  +exec xonotic.cfg +exec screen.layout.cfg \
  +set _touch_assets_ready 1 +vid_fullscreen 1 +vid_touchscreen 1 \
  +vid_width 1440 +vid_height 960 +set _cl_name TouchAgent \
  +set touch_setup_done 1 +set cl_allow_uid2name 0

# Capture: open console (`), `screenshot`, SCP TGA, convert with Pillow
```

Repo clone for code iteration: `~/Projects/Xonotic-Touch`.

## Follow-ups (optional)

1. Unlock GNOME keyring / set RDP credentials for visual remote desktop.
2. Grant screenshot portal once on-device so `grim`/Shell Screenshot work from SSH.
3. Prefer real multitouch injection (or on-device finger) for Join / Play now; ydotool mouse is flaky with `vid_touchscreen 1` + scale 2.
4. Clear leftover QC statement tracing if still noisy (`developer 0`, disable `prvm_*trace*`).
5. Investigate `DP_CRYPTO` / master timeout if online features matter on this image.
