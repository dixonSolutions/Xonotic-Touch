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

`dixonSolutions/dixonSolutions.github.io` is the fourth repository: no game source, only
the Pages site that keeps the update addresses of installs made before the rename
pointing at the current ones.

Package identifiers, data directory names, environment variables and release asset
names all say Godnotic: Flatpak `io.github.dixonSolutions.Godnotic`, Android
`io.github.dixonsolutions.godnotic`, click `godnotic.dixonsolutions`. What that meant for
installs made under the earlier identifiers:

- **Flatpak** moves by itself: the old ref carries an end-of-life rebase
  (`scripts/publish-flatpak-remote.sh`), so `flatpak update` switches the app and
  Flatpak carries its data folder across; the first start renames the pack folder
  inside it (`packaging/start.sh`, `GamePaths::adopt_old_name`). Nothing is downloaded
  again. The rebase was tried on a throwaway repository, not yet seen on a real install.
- **Native Linux** installs have `~/.local/share` and `~/.config` folders renamed the
  same way.
- **Android** cannot move: a new application id is a new application to Android, with
  its own storage. An old install stops being updated and the new APK installs beside
  it, downloading its data again.
- **Ubuntu Touch** is the same, and the new id needs its own OpenStore listing.

`scripts/test-old-name.sh` fails the tests when the earlier name comes back. Short
internal prefixes (`xt-` in the web shell's element ids and storage keys, `xt.` in the
Kotlin launcher) are not the name and stay: renaming the storage keys would make every
browser download its data again.

## Front ends

Each menu screen is described once, in QuakeC, as a tree of cards, lists, tabs and
controls (docs/UI_BRIDGE.md). A native renderer draws that description where there is
one; the QuakeC touch menu draws it everywhere else and is the fallback.

- **Android**: a Kotlin boot activity (banner, data download as a foreground service
  with progress and "ready" notifications, automatic resume), then the engine's SDL
  surface with a Kotlin view tree over it for the menus (docs/FRONTEND_BRIDGE.md). The
  banner, the boot screen and Home share one background, so the launch is one
  continuous picture. If the native interface cannot attach or stops answering for
  4 s, the QuakeC menu takes over.
- **Linux**: a Rust loader (`loader/`, GTK 4 and libadwaita) that is the app's window
  outside a match and starts the engine for one (docs/LAUNCHER.md). Its downloader is
  ranged, resumable and CRC-checked. `packaging/start.sh` and the fetch scripts remain
  for Ubuntu Touch and as the fallback.
- **Web**: an HTML/CSS loader over the engine compiled to WebAssembly (docs/WEB.md),
  then the QuakeC touch menu. Game data lives in the browser's private file system
  and is read in place.
- **In the engine, on every platform**: the console button (top right in menus, beside
  MENU in a match) opens the engine console with its own on-screen keys; the HUD is
  CSQC on Godnotic's own game code and engine-drawn on servers that send their own.

Verified on a Galaxy Tab Active5: first launch with no popups, the download screen and
its background behaviour, the console, native Home, Servers, Settings (vertical tabs
with horizontal sub-tabs), Credits and pause, a full bot match and leaving it, joining
a password-locked LAN game hosted on a desktop, a setting surviving a kill and restart.
Cold start to Home is 0.8-1.2 s. Not verified: a phone-sized device, rotation and
display cutouts, the Linux loader's pages by hand on a real desktop session, the web
build on any phone or tablet, hosting a LAN game from the tablet.

## Game data

Phones download the slim set: ASTC textures, zstd pack entries, Opus music, JPEGs
re-encoded within a measured tolerance, editor-only files left out; 390 MB against
1046 MB for the stock packs (docs/PHONE_ASSETS.md). The app asks the GPU for ASTC
first and takes the full set otherwise. Each published set was compared in the engine
against the full set (eight maps, four cameras each) and every entry checked against
its CRC; the first attempt at the JPEG step failed that comparison and was not
published. Browsers whose GPU has ASTC download the same slim set (405 MB with the
build's own files); other browsers and desktop installs use the full set. Music is part
of every set; there is no music choice anywhere.

## Start-up and loading

Fonts, menu pictures, sounds and models load on first use; what a map loaded late is
recorded and loaded under its next loading screen; loose-directory lookups are cached;
the renderer no longer restarts on GPUs without S3TC. On the tablet, warm and
throttled: menu ready 1.2 s after process start (5.0 s with the switches off), a
stormkeep bot match 4.6-5.0 s after the tap (13.4 s), worst frame in the first
seconds of play 61-100 ms. Every change has a cvar to turn it off
(`touch_loadtime 1` prints the phase tables). The map warm-up does not run while the
device is in battery saver; on the tablet that is 4.0 s from Play to the game against
2.5 s with it.

## Network

Between Godnotic peers the reliable channel sends up to 32 fragments per round trip
instead of one, and both sides answer a signon at once instead of at the next frame:
a join at 100 ms round trip went from 5.8 s to 3.0 s, and at 10 % loss from 27.8 s to
4.2 s. Stock servers and clients are unaffected (packet captures). Large packs
download over up to four ranged connections; a dropped or frozen connection rejoins by
itself; a player who leaves the app becomes a spectator and comes back into the game
(docs/NETWORK.md). Tested in network namespaces with latency, loss, reordering and
duplication, against a stock engine in both directions, and with hostile peers under
sanitizers. Not tested: the Android side on a real mobile network, Insanotic interop.

## LAN games

Host from the menu (port 26000 by default, optional password answered by challenge,
never sent), find games with the LAN filter without a master server, join by tapping.
Tested between the tablet and a desktop on one network in both directions, including
the host in the background. Games on a VPN (Tailscale, WireGuard, ZeroTier) are found
by a unicast sweep of small subnets and by remembered hosts, tested in namespaces only,
not with real VPN software. docs/LAN.md has the security notes: the password is a
shared secret for a trusted network.

## Mods

Lua 5.4 in a light sandbox with separate client and server realms; a mod error or a
budget overrun stops that mod and never the game. A mod runs at the `standard` tier as
soon as it is turned on; `gamerules`, `internet` and `engine_full` are switches, the
last with a warning (docs/LUA_API.md, generated: 109 functions, 172 server hooks, 54
client hooks). Mods can add settings pages and HUD panels, talk between server and
client over rate-limited channels, and a server can show pages to its players. A
server can neither install mods nor drive the manager, and see-through data is only
available offline. docs/MODDING.md and docs/MOD_COOKBOOK.md are the guides, with 15
examples in `touch/lua/examples`, each run in a headless bot match by
`scripts/test-mods.sh` (105 checks). `scripts/test-lua-sandbox.sh`,
`scripts/test-lua-packages.sh` and `test-mods.sh` need game data and are not CI jobs;
the package suite is sensitive to machine load. Not done: the examples on a device,
a fuzz harness for the channel parser, native executable mods (out of scope).

## Privacy

One first-run dialog: the player's name (docs/SETUP.md). A fresh install shares nothing with XonStat (the three
identity and statistics settings start off; an install that had opted in keeps its
choice); Settings > Mobile > Privacy has the switches and the terms. The game still
fetches Xonotic's `checkupdate.txt` at each start, which also supplies the banned
server list and carries no player identifier.

## Not done

- A phone-sized device has not been used; menu sizes there come from offscreen runs.
- The menu redesign in progress: Home as three picture cards, tap to select and one
  Join in the server list, a two-row map grid, the LAN address popup, local games
  unreachable from the network unless LAN is on, the slide-to-join control.
- The Rust core driving the Android interface (Kotlin reads the bridge itself today).
- Web: online servers; anything on a phone or tablet.
- Ubuntu Touch hardware validation; a Windows build of the mod runtime.
