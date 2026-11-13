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
at the next sync. The whole Lua runtime touches upstream in **four hook lines** plus
their includes and one QC include:

| Upstream file | Line | What it does |
|---|---|---|
| `engine/darkplaces/cl_screen.c` | `TouchLua_ClientFrame();` | each client frame, in the 2D pass under the menu: loads mods, runs `frame`, `hud`, `draw` |
| `engine/darkplaces/keys.c` | `TouchLua_KeyEvent(...)` | each game key (not Escape): mods may take it |
| `engine/darkplaces/sv_main.c` | `TouchLua_SV_InstallBuiltins(prog);` | gives the server VM the `touch_lua_event` builtin (#1740) |
| `engine/darkplaces/csprogs.c` | `TouchLua_ServerText();` | in `CL_VM_Parse_StuffCmd`: a server put text in the command buffer, so the manager refuses changes until it has run |
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

`cl_lua_enable` / `sv_lua_enable` are the master switches. `enabled.txt` stays next to
the mod folders (the launcher reads and writes it too); the engine replaces it through a
temporary file and a rename, never through a symlink. `_cl_lua_mods` (read-only)
holds one line per client mod, `name<TAB>state<TAB>title<TAB>error<TAB>description`,
which the Mods tab (`qcsrc/menu/xonotic/dialog_touch_mods.qc`) reads.

A mod that raises an error or hits a limit is **stopped** and shows `error` with the
message; the game goes on. Fix it and `lua_mod_reload <name>`.

**Crash-loop protection.** While a mod loads (`init.lua` and what it requires), the
engine keeps `<userdir>/luastate/mods/loading.txt` (`luastate/plugins/` for plugins) on
disk, written with `fsync` before the mod's first line runs and removed once loading
ends, either way. If the engine dies or is killed there, the next start finds it: that
mod is **not** started, is turned off (also in `enabled.txt`) and shows *did not finish
loading last time*. Turning it on again (`lua_mod_enable`, the Mods tab) is the retry. On
the web the marker is also kept in `localStorage`, which is written at once (a write to
the userdir reaches IndexedDB only after the frame yields, which a hang never does).

**Servers cannot drive the manager.** A server can put any command in a client's console
(`stufftext`, its CSQC's `localcmd` and `cvar_set`), and the engine runs it like typed
input. So while connected to a server this engine does not host, while playing a demo,
and afterwards until all text such a server queued has run (including what it queued
with `defer`), these refuse: `lua_mod_enable`, `lua_mod_disable`, `lua_mod_reload`,
`lua_mod_permission ... grant|revoke` and their `sv_lua_plugin_*` twins (a client's
plugins run when it hosts a game later); changes to `lua_memory_mb`, `lua_instructions`,
`lua_time_ms`, `cl_lua_enable`, `sv_lua_enable` (put back at once); and every
`lua_<mod>_<name>` command, unless the mod registered it with `{fromserver = true}`.
The engine marks such text in `CL_VM_Parse_StuffCmd` (the `csprogs.c` hook above). A
server can still bind keys or define aliases (the engine allows that); pressing such a
key later runs as if you typed it.

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
initially granted: importing or copying a mod never grants anything. Enable the mod after granting its needs; a denied call raises
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
stored separately per mod and realm in `<userdir>/luastate/mods/grants/<name>.txt` or
`luastate/plugins/grants/<name>.txt` (one line, `v1 <hex mask>` or `legacy <hex mask>`),
outside the folders mod packages unpack into and out of every path a mod can write, and
changes fail closed if they cannot be saved. Grants cannot be changed while a server may
be sending commands (above). Grant files are user data and cannot be supplied by a mod
package; the old `mods/.permissions/` folder is not read.
The read-only `_cl_lua_permissions` cvar exposes newline-separated client rows:
`name<TAB>mode<TAB>requested<TAB>granted`. Mode is `v1`, `legacy`, or `invalid`;
the last two fields are comma-separated scope names, and granted is the effective
intersection with requested.

A mod with no `api` declaration (written before scopes existed) is a **legacy** mod. It
requests every scope, and until the user grants more it has the HUD set: `events`,
`commands`, `cvar_read`, `display`, `input`, `players`, `state`. Everything that changes
something outside the mod's own view — `cvar_write`, `exec`, `storage`, `messages` —
needs a grant (`lua_mod_permission <name> grant exec`). Legacy grants and API 1 grants
are stored separately so switching a manifest to API 1 does not silently inherit legacy
access. The examples in `touch/lua/examples` declare API 1.
The sandbox is not a substitute for trusting the engine and installed game files.

## A first mod

`<userdir>/mods/hello/init.lua`:

```lua
xt.on("hud", function(w, h)
  xt.draw.text(20, h - 40, "hello from Lua", 16, 1, 1, 0.5, 1)
end)
xt.command("greet", function(who) print("hi " .. (who or "there")) end, "say hi")
```

Then `lua_mod_enable hello`; the command is `lua_hello_greet`. (Without a `mod.txt`
this is a legacy mod, which may draw, listen to events and add commands.) Working examples are in
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
  pattern functions refuse subjects and replacements over 1 MB and patterns over
  512 bytes, and the matcher itself checks the time limit, so a catastrophic pattern
  stops too. Loops inside C library functions are bounded as well (below).
- A mod that hits a limit **cannot catch its own death**: `pcall`, `xpcall`,
  `coroutine.resume`/`close` and `load` raise again instead of returning, and every
  further instruction raises.
- Mods only load from your own `mods/` folder, never from the game's virtual filesystem,
  so a server cannot send code in a pk3. A server's commands cannot change mods,
  grants or limits (*Managing mods* above).
- **Files**: `init.lua`, `mod.txt`, required modules and `xt.save`/`xt.load` files are
  opened one path component at a time below the realm folder with `O_NOFOLLOW`: a mod
  folder that is a symlink is not listed, and a symlink at any level below it (`data`,
  the file, a module) is refused, as are hard-linked files and anything that is not a
  regular file (a FIFO named `init.lua` used to block the folder scan forever). Keys
  and module names cannot hold separators or dot-only parts. The manager's state
  (grants, the crash marker) is in `<userdir>/luastate/`, which no mod path reaches.
- **Names**: a mod's commands and own cvars are `<prefix><mod>_<name>` (`lua_` for
  client mods, `sv_lua_` for plugins) where `<name>` is `a-z0-9` without `_`, so a
  name belongs to exactly one mod even though mod names may contain `_`. Folders whose
  name would share a namespace with the engine (`mod`, `mods`, `plugin`, `plugins`,
  `memory`, `time`, `instructions`, `enable`, `status`, `permission(s)`, or any name
  `X` with an engine command or cvar starting with `<prefix>X_`) are listed with an
  error and never started. A mod may register 16 commands and create 32 cvars; its own
  cvars are saved in `config.cfg` only when it has `storage`.
- **Engine resources**: all mods together may name 64 different pictures
  (`xt.draw.pic`) and 64 different sounds (`xt.sound`) per session (the engine keeps
  them cached for good). Plugins may send each player 10 messages a second (burst 20)
  and broadcast 5 a second (burst 10), and a message that would overflow a client's
  buffer is dropped.
- **Re-entry and aborted frames**: an engine path that comes back into the Lua code
  from inside a call into a mod (a loading-screen redraw while a mod loads a picture,
  a key read meanwhile) is ignored until that call returns. If the engine abandons the
  frame in the middle of a call (`Host_AbortCurrentFrame`: CANCEL on the loading
  screen, `Host_Error`), the next frame notices, stops that one mod with *interrupted*
  and every other mod keeps running.
- **Quit**: every running mod and plugin gets its `unload` event when the engine quits.
- No API returns another player's position, an entity list or a trace, and none sets
  view angles or fires: a mod cannot see through walls or aim. The offline practice
  tools (`cl_touch_esp_enabled`, aim assist) are neither readable as data nor writable.

### C library functions

The count hook only fires between Lua instructions, so a loop inside a C function used
to run unbounded (`string.rep("", math.maxinteger)`, `table.move({}, 1, math.maxinteger,
1, {})`, `table.insert`/`remove`/`sort` on a table with a lying `__len` hung the engine
past SIGTERM, and again at every start). Every exposed function is bounded:

| Functions | Why they end |
|---|---|
| `table.insert`, `remove`, `move`, `concat`, `unpack`, `sort` | each element they read costs one instruction of the call's budget and the time limit is checked every 256 (`lua_geti` in the vendored `ltablib.c` goes through `TouchLua_WorkStep`; done by a macro in `touch_lua_vm.c`, `ltablib.c` itself is unmodified) |
| `table.pack` | one step per argument (at most the Lua stack, 1,000,000) |
| `string.rep` | an empty result returns at once (a wrapper); otherwise `n` is at most the result's length, which the allocator holds to the memory limit before the loop |
| `string.find`, `match`, `gmatch`, `gsub` | subject and replacement up to 1 MB, pattern up to 512 bytes; the matcher checks the time limit (every `l_unlikely` in `lstrlib.c`, including once per backtrack and per capture in a replacement) |
| `string.byte`, `codepoint` ranges, `string.char`, `utf8.char` | at most the Lua stack (1,000,000 values) |
| `string.format` | one conversion per format item; widths and precisions have at most 2 digits (Lua checks); linear in its arguments |
| `string.len`, `sub`, `lower`, `upper`, `reverse`, `pack`, `packsize`, `unpack`, `utf8.len`, `offset`, `codes`, `tostring`, `tonumber` | linear in the string, which the memory limit bounds; `pack` padding grows the result, which the memory limit bounds |
| `math.*`, `select`, `rawlen`, `rawequal`, `rawget`, `rawset`, `next`, `ipairs`, `type`, `getmetatable`, `error`, `assert` | constant time (`math.random`'s retry loop ends with probability 1/2 per step; `error` with a level walks at most the call depth) |
| `print` | stops at a full 1 KB line, whatever the number of arguments |
| `load`, `require` | text up to 1 MB, compiled in one pass |
| `collectgarbage("collect"/"step")` | linear in the mod's heap, which the memory limit bounds |

On top of that, every `string`, `table` and `utf8` call checks the time limit when it
returns, so a slow but finite call cannot run past it by more than its own duration.

`tests/lua-sandbox` holds escape attempts; `scripts/test-lua-sandbox.sh` runs them in
the real engine (108 checks in the escape mod; 13 mods that must get stopped, among them
the C-loop hangs; storage through symlinks and hard links; a FIFO; legacy permissions;
reserved names and namespaces; a demo whose server text tries to drive the manager; an
engine killed while a mod loads), and `scripts/test-lua-match.sh` runs the examples in
a bot match.

## API: both realms (`xt`)

| Call | |
|---|---|
| `xt.on(event, fn)` | add a handler (events below) |
| `xt.command(name, fn, help, opts)` | console command `lua_<mod>_<name>` (server: `sv_lua_<mod>_<name>`), `name` `a-z0-9` without `_`, up to 16 per mod; `fn` gets the arguments as strings. A server's text cannot run it unless `opts` is `{fromserver = true}` |
| `xt.cvar(name)` | a cvar's value, or nil for unknown, private and secret ones (passwords, rcon, keys, tokens, log destinations) |
| `xt.setcvar(name, value)` | set an allowlisted cvar, or the mod's own `lua_<mod>_<name>` (plugins: `sv_lua_<plugin>_<name>`; `name` `a-z0-9` without `_`; up to 32, created on first set, saved in `config.cfg` only with `storage`); values may not contain `; " $ \` or control characters |
| `xt.exec(line)` | run an allowlisted console command (up to 8 per call); no `; " $ \` or line breaks |
| `xt.save(key, text)` / `xt.load(key)` | text up to 64 KB in `<mod>/data/<key>.txt` (up to 64 keys; never through a symlink or a hard link) |
| `xt.realtime()` | seconds since the game started |
| `xt.modname`, `xt.realm`, `xt.apiversion` | `"hello"`, `"client"`/`"server"`, `"1"` |
| `print(...)` | to the console as `[mod] ...` (64 lines per call) |

Both realms also have an `unload` event, run when the mod is turned off and when the
engine quits.

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
Commands (`xt.exec`, with `exec`):

| Command | Why it is safe |
|---|---|
| `echo` | text in the console |
| `say`, `say_team` | chat as the player, one line every 2 s for all mods together |
| `weapnext`, `weapprev`, `weaplast`, `weapbest` | switch weapon |
| `impulse <number>` | the game's weapon and action impulses; exactly one number; the server decides what it does |
| `ready` | toggle ready in warmup |
| `+showscores`, `-showscores` | the scoreboard |
| `kill` | respawn (suicide) |

Not allowed, on purpose: `screenshot` (wrote files by name into the game folder, 114 MB
in 2 s), `togglemenu` and `messagemode`/`messagemode2` (open the menu or a text input
over the game: on a touchscreen there may be no way back), `play` (loads any sound by
name without a limit; `xt.sound` is the bounded way), and anything that binds keys,
changes other settings, quits or connects.

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
| `xt.tell(id, text)` / `xt.centerprint(id, text)` / `xt.broadcast(text)` | messages (10 a second per player, 5 broadcasts a second, for all plugins together) |
| `xt.map()`, `xt.time()`, `xt.maxplayers()` | |

Settable cvars: `g_balance_*`, `g_start_weapon_*`, `g_pickup_*`, the mutator switches
(`g_instagib`, `g_nix`, `g_grappling_hook`, `g_vampire`, `g_midair`, `g_jetpack`,
`g_dodging`, `g_bloodloss`, `g_rocket_flying`, `g_nades`, `g_buffs`, `g_powerups`,
`g_new_toys`, `g_overkill`, `g_melee_only`, `g_random_gravity`, `g_walljump`,
`g_weaponarena`), `g_spawnshieldtime`, `g_respawn_delay`, `g_friendlyfire`,
`g_mirrordamage`, `timelimit`, `fraglimit`, `leadlimit`, `sv_gravity`; never a name with
`uri`, `url`, `log`, `ban`, `http`, `dest`, `file`, `path` or `password` in it.
Commands (`xt.exec`, with `exec`): `say` and `echo` (text), `restart` (the map), `kick`
(a player), and `sv_cmd` with `gotomap`, `endmatch`, `allready`, `lockteams`,
`unlockteams`, `shuffleteams`, `nextmap` (match control). None writes files, binds keys,
quits or connects.

Server plugins only get game events on a server running the Touch game code (local
matches, Touch dedicated servers); the `frame` and `map` events come from it too.
Servers with `sv_threaded 1` run no plugins.

## Engine side

`engine/darkplaces/touch_lua.c` (sandbox, limits, manager, common API),
`touch_lua_cl.c` (client realm), `touch_lua_sv.c` (server realm, builtin #1740),
`touch_lua_lib.h` (`xt.ui`), `touch_lua_vm.c` (Lua compiled as one unit, with the
time-limit hooks for `lstrlib.c` and `ltablib.c` and the reset after an aborted frame)
and `touch_lua54/` (Lua 5.4.8 from lua.org, MIT, unmodified). The game side is
`qcsrc/server/touch_lua.qc`.
