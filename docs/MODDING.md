# Lua mods and plugins

Godnotic runs **Lua 5.4** scripts in a sandbox:

- **Client mods** (`<userdir>/mods/<name>/`) draw over the HUD or the whole screen, see
  keys, fingers and the mouse, add console commands, change a few settings, play sounds,
  and keep small files. They are how new Touch UI (HUD panels, overlays, settings pages)
  should be written from now on.
- **Server plugins** (`<userdir>/plugins/<name>/`) run on a server this engine hosts (a
  local match or a dedicated server) and react to the game: connects, spawns, deaths,
  damage, chat, item pickups, `cmd` commands, map changes, match end.

The userdir is `~/.xonotic` on Linux (inside the Flatpak: `~/.var/app/<id>/.xonotic`),
the app's files folder on Android. A mod is a folder with an `init.lua`.

## Why mods instead of engine and QC edits

Most of `engine/` and the game QC is upstream Xonotic, merged daily
([UPSTREAM_SYNC.md](UPSTREAM_SYNC.md)); every upstream line Touch changes can conflict
at the next sync. The whole Lua runtime touches upstream in **three hook lines** plus
their includes and one QC include:

| Upstream file | Line | What it does |
|---|---|---|
| `engine/darkplaces/cl_screen.c` | `TouchLua_ClientFrame();` | each client frame, in the 2D pass under the menu: loads mods, runs `frame`, `hud`, `draw` |
| `engine/darkplaces/keys.c` | `TouchLua_KeyEvent(...)` | each game key (not Escape): mods may take it |
| `engine/darkplaces/sv_main.c` | `TouchLua_SV_InstallBuiltins(prog);` | gives the server VM the `touch_lua_event` builtin (#1740) |
| `qcsrc/server/progs.inc` | `#include <server/touch_lua.qc>` | the Touch mutator that reports game events |

Cvars and console commands are registered from Touch code on first use, and the build
lists only gain object names (`makefile.inc`, which Android's CMake and the web build
read). A feature written as a mod adds **no** upstream lines at all.

## Managing mods

A new folder starts **off**. Turn client mods on and off in **Settings → Mobile → Mods**:
one row per mod with a large ON/OFF switch, its state (running, off, stopped by an
error with the message), Reload and Info, plus Reload all and the master switch. The
tab lists the folder again every two seconds while the menu is open.

On **Android**, *Import mod* in that tab opens the system file picker for a `.zip` and
unpacks it into the mods folder (`ModImport.java`): `init.lua` at the top of the zip or
in its single top folder, which names the mod; every entry name is checked (no absolute
paths, `..`, backslashes or drive letters), at most 256 files, 1 MB each, 8 MB in all.
An update keeps the mod's `data/` folder. The imported mod starts off, like any other.

The same state from the console:

| Client | Server | |
|---|---|---|
| `lua_mods` | `sv_lua_plugins` | list, with state (`off`, `enabled`, `running`, `error`) and error text |
| `lua_mod_enable <name>` | `sv_lua_plugin_enable <name>` | turn on (kept in `mods/enabled.txt` / `plugins/enabled.txt`) |
| `lua_mod_disable <name>` | `sv_lua_plugin_disable <name>` | turn off |
| `lua_mod_reload [name]` | `sv_lua_plugin_reload [name]` | restart one or all, clearing errors |
| `lua_mod_info <name>` | `sv_lua_plugin_info <name>` | folder, memory, error, events, commands |

`cl_lua_enable` / `sv_lua_enable` are the master switches. `_cl_lua_mods` (read-only)
holds one line per client mod, `name<TAB>state<TAB>title<TAB>error<TAB>description`,
which the Mods tab (`qcsrc/menu/xonotic/dialog_touch_mods.qc`) reads.

A mod that raises an error or hits a limit is **stopped** and shows `error` with the
message; the game goes on. Fix it and `lua_mod_reload <name>`.

An optional `mod.txt` gives the manager a title:

```
title: HUD clock
version: 1.0
author: You
description: One line about it.
```

## Permission scopes (API 1)

A new mod can opt into scoped permissions in `mod.txt`:

```
api: 1
permissions: events, display, commands
```

The comma- or space-separated list declares the APIs the mod needs. None are
initially granted. Enable the mod after granting its needs; a denied call raises
`permission denied` (and an uncaught error stops that mod). Unsupported API
versions and unknown scope names reject the manifest. `xt.apiversion` is `"1"`.

| Scope | Access |
|---|---|
| `events` | `xt.on` and delivery of event payloads, including key, chat and player events |
| `commands` | register and receive the mod's own console commands |
| `cvar_read` | `xt.cvar` (private and secret cvars remain hidden) |
| `cvar_write` | `xt.setcvar` (own namespace and realm allowlist only) |
| `exec` | `xt.exec` (realm command allowlist and rate limits still apply) |
| `storage` | `xt.save`, `xt.load`, confined to the mod's data folder |
| `display` | client screen dimensions, drawing and sound |
| `input` | client finger, mouse and key state |
| `players` | player and scoreboard information permitted to that realm |
| `messages` | server tell, centerprint and broadcast |
| `state` | match state, map, time and `xt.realtime` |

`events` includes all event payloads, so a player or key event also reveals the
information in that event without a separate `players` or `input` grant. Lua's
basic language, `print` and `require` of files in the mod remain available.
Scopes never expand the client/server API or its existing allowlists. For example,
`exec` cannot run arbitrary console commands, and client `players` does not reveal
other players' positions. Server plugins already have more information than client
mods because they run on the host.

Inspect and change grants from the local console:

```
lua_mod_permission hello
lua_mod_permission hello grant events
lua_mod_permission hello revoke events
sv_lua_plugin_permission welcome grant messages
```

Only declared scopes may be granted. Revocation applies immediately, including
to already registered event handlers and commands; granting a scope does not
re-run a failed initialization, so reload that mod after granting it. Grants are
stored separately per mod and realm in `mods/.permissions/<name>.txt` or
`plugins/.permissions/<name>.txt`, outside the mod folder, and changes fail closed
if they cannot be saved. Client grants cannot be changed while connected to a
remote server. Grant files are user data and cannot be supplied by a mod package.
The read-only `_cl_lua_permissions` cvar exposes newline-separated client rows:
`name<TAB>mode<TAB>requested<TAB>granted`. Mode is `v1`, `legacy`, or `invalid`;
the last two fields are comma-separated scope names, and granted is the effective
intersection with requested.

For compatibility, a mod with no `api` declaration retains the previous full set
of sandboxed APIs by default. Its permissions can still be inspected and revoked;
legacy grants and API 1 grants are stored separately so switching a manifest to
API 1 does not silently inherit legacy access. Existing examples are legacy mods.
The sandbox is not a substitute for trusting the engine and installed game files.

## A first mod

`<userdir>/mods/hello/init.lua`:

```lua
xt.on("hud", function(w, h)
  xt.draw.text(20, h - 40, "hello from Lua", 16, 1, 1, 0.5, 1)
end)
xt.command("greet", function(who) print("hi " .. (who or "there")) end, "say hi")
```

Then `lua_mod_enable hello`; the command is `lua_hello_greet`. Working examples are in
[`touch/lua/examples`](../touch/lua/examples): `hud_clock` (HUD panel, own cvar,
`require`), `settings_panel` (a settings page with `xt.ui`, `xt.save`), `chat_keys`
(key event), and the plugins `welcome` (connect, death, chat filter, `cmd stats`) and
`underdog` (damage scaling). Copy a folder into `mods/` or `plugins/`.

## The sandbox

Each mod has its own Lua state with only `base`, `coroutine`, `table`, `string`, `math`
and `utf8`.

- **Not there**: `io`, `os`, `debug`, `package`, `dofile`, `loadfile`, `string.dump`,
  `warn`. `load` takes a text string only (never bytecode, never a reader function).
- `require("a.b")` loads `a/b.lua` from the mod's own folder and nothing else;
  `require("xt.ui")` is the built-in widget library.
- `setmetatable` refuses `__gc` (finalizers run where no limit can stop them).
  `collectgarbage` allows `"count"`, `"collect"`, `"step"`.
- **Limits per call** (an event, a command; loading gets 10x instructions and 1 s):
  `lua_memory_mb` (16), `lua_instructions` (2,000,000), `lua_time_ms` (40). String
  pattern functions refuse subjects over 1 MB and patterns over 512 bytes, and the
  matcher itself checks the time limit, so a catastrophic pattern stops too.
- A mod that hits a limit **cannot catch its own death**: `pcall`, `xpcall`,
  `coroutine.resume`/`close` and `load` raise again instead of returning, and every
  further instruction raises.
- Mods only load from your own `mods/` folder, never from the game's virtual filesystem,
  so a server cannot send code in a pk3. While connected to someone else's server,
  `lua_mod_enable` and `lua_mod_reload` refuse (servers can put commands in your console).
- No API returns another player's position, an entity list or a trace, and none sets
  view angles or fires: a mod cannot see through walls or aim. The offline practice
  tools (`cl_touch_esp_enabled`, aim assist) are neither readable as data nor writable.

`tests/lua-sandbox` holds escape attempts; `scripts/test-lua-sandbox.sh` runs them in
the real engine (90 checks plus 9 mods that must get stopped), and
`scripts/test-lua-match.sh` runs the examples in a bot match.

## API: both realms (`xt`)

| Call | |
|---|---|
| `xt.on(event, fn)` | add a handler (events below) |
| `xt.command(name, fn, help)` | console command `lua_<mod>_<name>` (server: `sv_lua_<mod>_<name>`); `fn` gets the arguments as strings |
| `xt.cvar(name)` | a cvar's value, or nil for unknown, private and secret ones (passwords, rcon, keys, tokens, log destinations) |
| `xt.setcvar(name, value)` | set an allowlisted cvar, or the mod's own `lua_<mod>_*` (created and saved on first set); values may not contain `; " $ \` or control characters |
| `xt.exec(line)` | run an allowlisted console command (up to 8 per call); no `; " $ \` or line breaks |
| `xt.save(key, text)` / `xt.load(key)` | text up to 64 KB in `<mod>/data/<key>.txt` (up to 64 keys) |
| `xt.realtime()` | seconds since the game started |
| `xt.modname`, `xt.realm`, `xt.apiversion` | `"hello"`, `"client"`/`"server"`, `"1"` |
| `print(...)` | to the console as `[mod] ...` (64 lines per call) |

Both realms also have an `unload` event, run when the mod is turned off.

## API: client mods

Events:

| Event | Arguments | Return |
|---|---|---|
| `frame` | `dt` | |
| `hud` | `w, h` (2D size) | in a match, over the game view and HUD |
| `draw` | `w, h` | every frame, under the menu and console: overlays and mod-made pages |
| `key` | `key, down, char` (`"MOUSE1"`, `"F5"`, ...) | `true` keeps the key from its binding; keys bound to the console or menu always go through |
| `menu` | `open` | |
| `map` | `name` (`""` when leaving) | |

Calls:

| Call | |
|---|---|
| `xt.draw.fill(x, y, w, h, r, g, b, a)` | rectangle (colours 0..1) |
| `xt.draw.text(x, y, text, size, r, g, b, a)` | text with `^` colour codes; returns its width |
| `xt.draw.textwidth(text, size)` | |
| `xt.draw.pic(path, x, y, w, h, r, g, b, a)` | an image under `gfx/` |
| `xt.draw.clip(x, y, w, h)` / `xt.draw.noclip()` | clip later drawing (reset after each handler) |
| `xt.screen()` | `w, h` of the 2D coordinate space |
| `xt.fingers()` | `{ {id, x, y}, ... }` fingers on the screen |
| `xt.mouse()` | `x, y` of the pointer |
| `xt.keydown(key)` | is a game key down |
| `xt.sound(path)` | play a sound under `sound/` (16 a second at most) |
| `xt.ingame()`, `xt.islocal()`, `xt.map()`, `xt.time()` | match state, local server, map, game time |
| `xt.me()` | `{health, armor, weapon, alive, name, frags, color}` (yourself only) |
| `xt.players()` | scoreboard: `{ {name, frags, color, slot, me}, ... }` |

`xt.draw.*` only works inside `hud` and `draw`.

Settable cvars: `crosshair*`, `hud_*`, `con_notify*`, `con_chat*`, `viewsize`, `fov`,
`cl_bob*`, `showfps`, `cl_showfps`, `showtime`, `showdate`, `sensitivity`, `volume`,
`bgmvolume`, `r_drawviewmodel`, `cl_gunalign`, `scr_conalpha`, `scr_centertime`,
`cl_hidewaypoints`, `cl_zoomfactor`, `cl_zoomspeed`.
Commands: `echo`, `say`, `say_team` (one line per 2 s for all mods together),
`screenshot`, `togglemenu`, `weapnext`, `weapprev`, `weaplast`, `weapbest`,
`messagemode`, `messagemode2`, `ready`, `+showscores`, `-showscores`, `kill`, `impulse`,
`play`.

### `xt.ui`

```lua
local ui = require("xt.ui")
xt.on("draw", function(w, h)
  ui.begin()                                        -- reads finger / mouse
  local x, y, iw = ui.panel(40, 40, 300, 200, "Title")
  local col = ui.column(x, y, iw)
  local rx, ry, rw, rh = col:row(32)
  if ui.button(rx, ry, rw, rh, "Press") then print("pressed") end
end)
```

`ui.panel`, `ui.button` (true when tapped), `ui.toggle(..., label, value)`,
`ui.slider(..., label, value, min, max)`, `ui.text`, `ui.textcentered`,
`ui.column(x, y, w, gap)` with `:row(h)`, `ui.split(x, w, n, gap)`, `ui.inside`,
`ui.pointer`, and `ui.theme` (colours and sizes, change freely). Lua passes all of a
call's results only to the last argument, so take `col:row()` into locals first.

## API: server plugins

Players are numbers `1..maxplayers`; `0` means the world or not a player.

| Event | Arguments | Return |
|---|---|---|
| `frame` | `dt` | |
| `map` | `name` | |
| `connect`, `disconnect`, `spawn` | `id` | |
| `death` | `victim, attacker, deathtype` | |
| `damage` | `target, attacker, damage, deathtype` | a number replaces the damage (0..100000); handlers chain |
| `chat` | `id, message, teamsay` | `false` holds the message back |
| `pickup` | `id, item` | `false` stops the pickup |
| `clientcmd` | `id, command, args` (from `cmd <command> ...`) | `true` when handled |
| `matchend` | | |

| Call | |
|---|---|
| `xt.players()` | ids of the connected players |
| `xt.player(id)` | `{id, name, bot, frags, spectator, team, health, armor, alive, origin = {x, y, z}}` |
| `xt.tell(id, text)` / `xt.centerprint(id, text)` / `xt.broadcast(text)` | messages |
| `xt.map()`, `xt.time()`, `xt.maxplayers()` | |

Settable cvars: `g_balance_*`, `g_start_weapon_*`, `g_pickup_*`, the mutator switches
(`g_instagib`, `g_nix`, `g_grappling_hook`, `g_vampire`, `g_midair`, `g_jetpack`,
`g_dodging`, `g_bloodloss`, `g_rocket_flying`, `g_nades`, `g_buffs`, `g_powerups`,
`g_new_toys`, `g_overkill`, `g_melee_only`, `g_random_gravity`, `g_walljump`,
`g_weaponarena`), `g_spawnshieldtime`, `g_respawn_delay`, `g_friendlyfire`,
`g_mirrordamage`, `timelimit`, `fraglimit`, `leadlimit`, `sv_gravity`; never a name with
`uri`, `url`, `log`, `ban`, `http`, `dest`, `file`, `path` or `password` in it.
Commands: `say`, `echo`, `restart`, `kick`, and `sv_cmd` with `gotomap`, `endmatch`,
`allready`, `lockteams`, `unlockteams`, `shuffleteams`, `nextmap`.

Server plugins only get game events on a server running the Touch game code (local
matches, Touch dedicated servers); the `frame` and `map` events come from it too.
Servers with `sv_threaded 1` run no plugins.

## Engine side

`engine/darkplaces/touch_lua.c` (sandbox, limits, manager, common API),
`touch_lua_cl.c` (client realm), `touch_lua_sv.c` (server realm, builtin #1740),
`touch_lua_lib.h` (`xt.ui`), `touch_lua_vm.c` (Lua compiled as one unit) and
`touch_lua54/` (Lua 5.4.8 from lua.org, MIT, unmodified). The game side is
`qcsrc/server/touch_lua.qc`.
