# The Linux app (loader)

`loader/` is Godnotic's app on Linux: one window that is the game's interface outside a
match. It is Rust, with one cross-platform core and a native UI per platform:

| Crate | What it is |
| --- | --- |
| `loader/crates/core` (`godnotic-loader-core`) | No UI toolkit: the UI-bridge client, the renderer trait, sessions (starting the engine, the secret fd, how a session ended), the pack downloader, paths, launcher.conf, platform detection, `godnotic://` links. Builds for Linux and for `aarch64`/`armv7`/`x86_64-linux-android`. |
| `loader/crates/gtk` (`godnotic-loader`) | The Linux app: GTK 4 / libadwaita through gtk4-rs, pinned to what the Flatpak runtime has (GNOME 48: GTK 4.18, libadwaita 1.7). |
| `loader/crates/android` | The JNI surface for the Android app's Kotlin UI (stub; filled on `work/androidui`). |

The old Kotlin/Native launcher (`launcher/`) still builds and stays until the engine's
UI-bridge backend is live; the Flatpak installs the Rust app (`/app/bin/godnotic-loader`,
with `xt-launcher` as a link for old shortcuts).

## How screens are made

The engine's menu VM describes every screen once (docs/UI_BRIDGE.md); the app renders the
description with libadwaita and adds styling and motion. The app itself authors only what
must work before or without an engine: the first-frame skeleton, the storage popover with
the downloader, the offline page, the join prompt and password dialog of a `godnotic://`
link, and the hand-over to the game.

| Bridge node | Widget |
| --- | --- |
| `app.stack` | `AdwNavigationView` pages (push/pop animated when the stack grows or shrinks by one) |
| `app.modal` | `AdwDialog` (a bottom sheet on narrow windows) |
| `screen` | `AdwToolbarView`: header bar (title, Back, the screen's actions) and the `toolbar` child as sticky top bars, the `actionbar` child or `primary` as the sticky bottom bar, one scrolling region |
| `hero`, `card` | custom-styled boxes with `GtkPicture` previews, chips and badges; a card is one button |
| `grid` | `GtkFlowBox` for short grids (home tiles, modes) |
| `list` (`display: table`) | `GtkColumnView` over a `GListModel` backed by the core's list windows: dense rows, sortable headers (a click re-windows the engine with the new sort), lower-priority columns dropped and two-line rows on narrow windows |
| `list` (`display: cards`) | `GtkGridView` of preview cards; a picture is asked for when its card is bound and dropped when unbound |
| `split` | `AdwNavigationSplitView` (Settings' vertical tabs), one column on narrow windows |
| `tabs` | `AdwInlineViewSwitcher` fixed at the top of the pane, each tab scrolling under it |
| `section`, `form` | `AdwPreferencesGroup` |
| `switch` | `AdwSwitchRow`, or `GtkCheckButton` for `style: checkbox` (the filter row) |
| `slider` | `AdwActionRow` + `GtkScale` (values throttled to 10 Hz while dragging) |
| `stepper` | `AdwSpinRow` |
| `choice` | `AdwToggleGroup` (segmented, when it fits) or `AdwComboRow` |
| `field` | `AdwEntryRow`, `AdwPasswordEntryRow` (closed/open padlock for set/unset), `GtkSearchEntry`, `GtkEntry` in bars |
| `row` | `AdwWrapBox` (wraps on narrow windows) |
| `preview` | big picture, title, attribute rows, description, actions |
| `status`, `progress`, `text`, `image`, `divider` | `AdwStatusPage`, `GtkProgressBar`, labels, `GtkPicture`, `GtkSeparator` |
| `toast`, `busy` | `AdwToast`, `AdwBanner` |
| anything else | a generic `AdwActionRow`, or its children, or nothing |

Actions: `confirm` asks with `AdwAlertDialog`; `link` shows the host and opens the URL with
`GtkUriLauncher` (the portal in Flatpak); `copy` uses the clipboard; `file` opens
`GtkFileDialog`; `back` sends `back`.

Threading: one main loop. The core's store is borrowed mutably only while a message is
applied; widget callbacks only queue messages, which a pump sends after the borrow ends.

## Starting the game and coming back

Until the engine's UI-bridge backend mode exists (`-uibridge`, branch `work/uibridge`), the
app starts the game through `packaging/start.sh` (or `GODNOTIC_START`):

- **Play** starts it as always (its own menu).
- A **`godnotic://join/<host>:<port>`** link (the desktop file registers
  `x-scheme-handler/godnotic`) asks "Join this game?" and starts the engine in **native
  front end mode** (`engine/darkplaces/touch_nativefe.c`): `GODNOTIC_FRONTEND=native`,
  `GODNOTIC_FE_RESULT=<file>`, `+connect`. The engine opens straight into the join and, when
  the match is left, the player is kicked, the join fails or a password is refused, writes
  `kind`/`reason`/`server` and quits instead of showing its menu.
- A **password** is never in argv or the environment: the app writes
  `join\t<address>\t<password>` into a pipe the engine gets as fd 3
  (`GODNOTIC_FE_SECRET_FD=3`), reads once and closes. "Needs a password" and "Wrong password"
  come back as results; the app asks with an `AdwPasswordEntryRow` and starts again.
- The **window hides** while the engine runs and comes back with the result as a toast; a
  crash shows the log path (`<user base>/last-session.log`).
- **No orphans**: `GODNOTIC_FE_WATCH_PID` makes the engine quit within a second when the app
  is gone; the game window carries the app id (`SDL_VIDEO_X11_WMCLASS`,
  `SDL_VIDEO_WAYLAND_WMCLASS`), so the dock shows one Godnotic entry.

## Game data: the storage popover

A header-bar button opens a popover: packs installed, size, data set and why, free space,
**Download / Check again** with progress, **Stop**. Music is always part of the set. The
downloader is the core's port of `PackFetch` (Kotlin launcher and Android app), writing the
same files so either can resume the other's partial download:

- The plan reads the size with `Range: bytes=0-0`, then the zip's tail by explicit offsets
  (GitHub's release storage answers suffix ranges with 501), and keeps only
  `Xonotic/data/<name>.pk3` one level deep.
- Stored entries are fetched in 8 MiB chunks over 8 connections into `<name>.part`; chunk
  progress is saved to `<name>.part.state` (big-endian `XTPC`, csize, count, one i32 per
  chunk: Java's DataOutputStream format) about once a second and on stop. Deflated entries
  are inflated as one stream (`miniz_oxide`).
- `.packs-setup` pins the archive (ETag, size); every range carries `If-Range`; a new build
  mid-run ends with "published a new build" and the parts of the old one are discarded.
- CRC32 is checked before a part is renamed into place; 10 attempts with back-off for
  network trouble; no-space is refused before anything is fetched.
- After a complete run `<data>/.assets-ready` and `touch/music-ready.txt` are written for
  `start.sh`.
- HTTP is `ureq` with rustls and the platform's certificate store (feature `http-ureq`);
  Android passes its own `fetch::Http`.
- **The split set** ([DATA_FETCH.md](DATA_FETCH.md)): when the server has
  `godnotic-data-astc.json` (or `-dxt.json` where the GPU has no ASTC and the stock packs
  were not chosen by hand), `fetch::manifest` fetches the `first_play` packs, writes
  `.assets-ready` (Play works from here), then everything else by priority. Each pack is one
  explicit byte range of the set's archive, in 8 MiB chunks with the same `.part.state`, checked
  against size, CRC32 and SHA-256 before the rename; a map's own pack goes in after the packs
  it needs. While a game runs, `<engine userdir>/data/data/touch/data-status.txt` tells it what
  landed (the engine mounts it) and `data-request.txt` says which map the player waits for,
  which then goes first. An install with the old layout whole is left alone until a new set is
  published. A download that was not finished carries on when the app starts.
  `datasets::fetch_all` is the one call; `cargo run -p godnotic-loader-core --features
  http-ureq --example datafetch` is the same from a shell (`scripts/test-datafetch.sh`).
  Without a manifest it is the single archive as above.

## Command line

```
godnotic-loader [godnotic://join/HOST:PORT]
  --fixture=NAME|PATH   render a UI-bridge fixture instead of the engine
  --fixtures-dir=DIR    where fixture names are looked up (default: tests/uibridge/fixtures)
  --size=WxH  --dark | --light  --reduced-motion
  --snapshot=FILE.png [--snapshot-delay=MS]   render the window to a PNG and quit
  --script=FILE         drive the UI through its widgets (see crates/gtk/src/script.rs)
  --time                print the first-frame time
```

## Build and test

```sh
loader/build.sh            # inside org.gnome.Sdk//48 (or the newest installed), -> loader/dist/
loader/build.sh --check    # + clippy -D warnings (GTK and core) and the core tests
cd loader && cargo test -p godnotic-loader-core --features http-ureq   # core alone, no GTK
```

CI: `.github/workflows/loader.yml` (core tests, clippy, `cargo check` for the three Android
targets; the GTK app built and linted inside the GNOME 48 SDK on x86_64 and aarch64
runners); `build-and-publish.yml` takes its `loader-bin-*` artifacts for the Flatpak.

Core tests: every fixture in `tests/uibridge/fixtures` deserializes into typed nodes and
round-trips; each `seq-*` fixture played through NDJSON framing and the store ends equal to
the player's latest snapshot; base mismatches and dangling children ask for `sync`; list
windows (block requests, bounded cache, exact `items-changed` splices for in-place updates);
the fixture player's windows (sort, filter) and value confirmations; join links and
addresses; the session plan keeps the password out of argv/env and a real child reads it on
fd 3; session results; the split-set downloader (`tests/fetch_manifest.rs`: manifest checks,
first play then the rest with exact byte counts, a map pack last, stop and resume, a damaged
pack, a server that ignores the range, a wanted map, old installs, a new set, no space); the
single-archive downloader against a local range server (plan, full run, resume
after stop and after a kill, archive change, old-build pack, CRC failure, 503 retry, 404 and
slim-to-full fallback, no space, roles, the chunk-state format).

GUI tests run only on a private Xvfb, never on the user's display:

```sh
loader/scripts/run-headless.sh -- <command>        # Xvfb :90+, no WAYLAND_DISPLAY, checked
loader/scripts/shot.sh home 1280x800 dark out.png  # one fixture to a PNG, in the GNOME runtime
loader/scripts/shots-all.sh <outdir>               # every fixture, 3 sizes, light and dark
loader/scripts/drive.sh loader/tests/navigate.script <outdir>   # Home > Servers > ... > Mods
```

`GODNOTIC_DATA_DIRS` points fixture mode at folders of pk3s (or unpacked `maps/*.jpg`) so the
fixture player answers `pk3:` image refs with real map previews.

`loader/tests/session.script` (with a stand-in `GODNOTIC_START` that runs a built engine
offscreen, and a dedicated server with `sv_touch_lan_password`) joins from the GUI with no
password, a wrong one and the right one, and checks that the window hides and returns.

## What remains

- The engine's UI-bridge backend (`-uibridge`) is not merged: the app renders the fixtures,
  and Home/Multiplayer/Settings are not live. When it lands, `Backend::Engine`
  (`session::Backend`, NDJSON over the child's stdio) replaces the fixture player.
- Hosting from Create, the live server list, map previews from the engine, Settings values
  and the Mods manager all arrive through the bridge; nothing of them is in the app.
- Sticky section headers (`GtkListHeaderFactory`) and per-screen scroll anchors by row key
  are not done; lists keep their position across in-place row updates.
- One window: the app hides while the engine's own window shows the match (hand-over); the
  engine is not rendered inside the app window.
