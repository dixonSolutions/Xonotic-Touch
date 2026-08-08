# Desktop system tray (StatusNotifierItem)

Xonotic Touch registers a **host-side** StatusNotifierItem tray (Python +
AppIndicator), the same protocol Potato Tomato uses. On GNOME / Ultramarine /
Silverblue you need an **AppIndicator and KStatusNotifierItem** Shell
extension or the icon will not appear.

## Why host-side?

Closing the Flatpak UI tears down the sandbox. The tray and background asset
`fetchd` therefore run via `flatpak-spawn --host` against helpers copied into
`~/.var/app/<id>/data/xonotic-touch/` so downloads and the tray survive window
close / `flatpak kill` of the game instance.

## Menu

| Item | Action |
|------|--------|
| Status | Live download percent / Running / In tray |
| Previous games | Up to 3 maps from `g_maplist_mostrecent` in `config.cfg` |
| Show window | Re-runs Flatpak / focuses session |
| Close window | Quits the engine; tray + fetchd stay |
| Quit Xonotic Touch | Stops fetchd, kills Flatpak, exits tray |

## Background download

The setup wizard’s **Download in background** writes
`data/touch/background-fetch-request.txt` and quits the engine. `start.sh`
hands the job to `xonotic-touch-fetchd.sh`, keeps the tray, and when packs are
ready:

1. Sends a desktop notification (“Game data finished downloading…”)
2. Opens Xonotic Touch if it is not already running

Reopening the app while a download is live **joins** the existing `fetch.lock`
job — it does not start a second curl.

## Close-to-tray

When the tray starts successfully, closing the game window keeps the session
(`XONOTIC_TOUCH_CLOSE_TO_TRAY` defaults on). Set
`XONOTIC_TOUCH_CLOSE_TO_TRAY=0` to pause downloads and exit on window close
(old behaviour). `XONOTIC_TOUCH_NO_TRAY=1` disables the tray entirely.

## Flatpak permissions

Manifest `finish-args` include:

- `org.freedesktop.Notifications`
- `org.freedesktop.Flatpak` (for `flatpak-spawn --host`)
- StatusNotifierWatcher / AppMenu talk names
- `xdg-run/xonotic-touch-tray:create`

For a live install before the next release build:

```bash
flatpak override --user io.github.dixonSolutions.XonoticTouch \
  --talk-name=org.freedesktop.Notifications \
  --talk-name=org.freedesktop.Flatpak \
  --talk-name=org.kde.StatusNotifierWatcher \
  --talk-name=com.canonical.AppMenu.Registrar \
  --talk-name=com.canonical.indicator.application
```

## Code map

| Path | Role |
|------|------|
| `scripts/xonotic-touch-tray.py` | AppIndicator menu |
| `scripts/xonotic-touch-fetchd.sh` | Host download + notify + auto-open |
| `packaging/start.sh` | Session lock, helper sync, close-to-tray loop |
| `dialog_touch_asset_fetch.qc` | Background download button |
