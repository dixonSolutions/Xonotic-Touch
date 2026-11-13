# Godnotic: architecture and what has been verified

Godnotic is a touch-first client built on Xonotic: the DarkPlaces engine, Xonotic's
QuakeC game code, a sandboxed Lua mod runtime, and a native front end per platform.
This page says how the pieces fit and, for each area, what has been measured or tested
and what has not. It makes no claim that a first download or a cold map load takes
milliseconds: network throughput, pack size, storage latency and decode time set lower
bounds. A warm relaunch is about a second on a tablet; a cold map load is seconds.

## Repositories

| Repository | Owns | Consumers |
| --- | --- | --- |
| `dixonSolutions/Godnotic` | Packaging, native front ends, web loader, releases and update channels | Android, Flatpak, Ubuntu Touch, Pages, Vercel |
| `dixonSolutions/Godnotic-darkplaces` | The engine | Carried here in `engine/darkplaces` at the commit pinned in `components.json` |
| `dixonSolutions/Godnotic-data` | Game code and configs; the data packs as releases (`data-<date>` full, `data-slim` repacked) | Carried here in `engine/data/xonotic-data.pk3dir`; clients download the packs |

CI fails a `main` whose engine or game code differs from its pins.
`scripts/component-push.sh` publishes a change made here to its component repository
and pins it; a commit made in a component repository is built inside this one, then
imported, tested, merged and released by `component-sync.yml` (see AGENTS.md). That
workflow's schedule has not been seen to fire on its own; a run started by hand works.
Upstream Xonotic is merged only by hand (`upstream-watch.yml`, docs/UPSTREAM_SYNC.md).

Android, Flatpak and OpenStore package identifiers, data directory names, live-update
asset names and browser storage keys still carry the project's earlier name. They are
compatibility contracts: changing one makes a different application and strands
installed users. Everything a player sees says Godnotic.

## Front ends

- **Android**: a Kotlin/Java boot activity (banner, data download as a foreground
  service with progress and "ready" notifications, metered-network choice, automatic
  resume), then the engine's SDL surface. The banner stays up until the engine's first
  lit frame. JNI passes commands through a bounded queue pumped by the engine thread.
- **Linux**: a Kotlin/Native GTK 4 / libadwaita launcher (Play, Mods, Host, Data,
  Settings). Its Data page uses the same ranged, resumable, CRC-checked downloader as
  Android (`launcher/core`, 30 tests); `packaging/start.sh` and the fetch scripts remain
  the fallback where the launcher is not used (Ubuntu Touch starts them directly).
- **Web**: an HTML/CSS loader over the engine compiled to WebAssembly (docs/WEB.md).
  GTK cannot run on static hosting (Broadway needs a server process), so the browser
  front end is plain HTML with the same layout as the Android boot screen.
- **In the engine, on every platform**: the console button (top right in menus, beside
  MENU in a match) opens the engine console with its own on-screen keys; the touch
  menu (tile launcher, 48 dp rows, sheets that scroll) is QuakeC; the HUD is CSQC.

Verified on a Galaxy Tab Active5: first launch with no popups, the download screen and
its background behaviour, the console, the touch menu's taps, drags and size setting,
a full bot match. Not verified: a phone-sized device, rotation and display cutouts,
themed icons, the launcher's GTK pages by hand, the console button on Linux and web.

## Game data

Phones download the slim set: ASTC textures, zstd pack entries, Opus music, JPEGs
re-encoded within a measured tolerance, editor-only files left out; 390 MB against
1046 MB for the stock packs (docs/PHONE_ASSETS.md). The app asks the GPU for ASTC
first and takes the full set otherwise. Each published set was compared in the engine
against the full set (eight maps, four cameras each) and every entry checked against
its CRC; the first attempt at the JPEG step failed that comparison and was not
published. Desktop and web still use the full set.

## Start-up and loading

Fonts, menu pictures, sounds and models load on first use; what a map loaded late is
recorded and loaded under its next loading screen; loose-directory lookups are cached;
the renderer no longer restarts on GPUs without S3TC. On the tablet, warm and
throttled: menu ready 1.2 s after process start (5.0 s with the switches off), a
stormkeep bot match 4.6-5.0 s after the tap (13.4 s), worst frame in the first
seconds of play 61-100 ms. Every change has a cvar to turn it off
(`touch_loadtime 1` prints the phase tables). Joining over a real network is still
bound by the stock stop-and-wait handshake.

## LAN games

Host from the menu (port 26000 by default, optional password answered by challenge,
never sent), find games with the LAN filter without a master server, join by tapping.
Tested between the tablet and a desktop on one network in both directions, including
the host in the background. docs/LAN.md has the security notes: the password is a
shared secret for a trusted network.

## Mods

Lua 5.4 in a sandbox with separate client and server realms; a mod error or a
budget overrun stops that mod and never the game. docs/PLUGIN_STANDARD.md is the
package and manifest standard; docs/LUA_HOOKS.md lists the 172 game hooks bridged
from Xonotic's mutator system (generated by `scripts/gen-lua-hooks.py`). Packages
install from a file or an HTTPS link, are checked whole before anything is written,
and start with nothing granted; turning one on shows what it asks for and why. A
server can neither install nor drive the manager. Native executable mods are out
of scope: native code cannot be sandboxed inside the engine process (the standard
sketches an out-of-process protocol as a proposal only). Covered by
`scripts/test-lua-sandbox.sh` and `scripts/test-lua-packages.sh`, which need game data
and are not CI jobs; the manager was exercised on the tablet, where it could not yet
be shown that a write hook changes damage only with its permission.

## Privacy

No first-run dialogs. A fresh install shares nothing with XonStat (the three
identity and statistics settings start off; an install that had opted in keeps its
choice); Settings > Mobile > Privacy has the switches and the terms. The game still
fetches Xonotic's `checkupdate.txt` at each start, which also supplies the banned
server list and carries no player identifier.

## Not done

- A phone-sized device has not been used; menu sizes there come from offscreen runs.
- Dialog title bars and action rows scroll with their content; drawn checkboxes are
  small inside their 48 dp hit areas; menu text fields use the system keyboard.
- Web: all data is copied into browser memory at start, and the first visit downloads
  everything before play.
- The windowed reliable channel for faster joins between Godnotic peers.
- Ubuntu Touch hardware validation; a Windows build of the mod runtime.
