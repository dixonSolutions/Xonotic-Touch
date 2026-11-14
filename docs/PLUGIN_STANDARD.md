# Godnotic mod package standard (API 2)

This is the standard for Lua **client mods** and **server mods**: what a package
is, what its manifest says, how it is installed, reviewed, enabled, updated and
removed, what each permission allows, which events and hooks it can use, how it
does continuous work, and what the engine promises when a mod misbehaves.
(This file keeps its old name, `PLUGIN_STANDARD.md`, so existing links work.)
The API reference for the functions themselves is [MODDING.md](MODDING.md); every
game hook is listed in [LUA_HOOKS.md](LUA_HOOKS.md) (generated).

"Mod" below means either kind unless the realm matters: a **client mod** runs in your
game, on this device (`<userdir>/mods/<name>/`); a **server mod** runs in games you
host, on a server this engine runs (`<userdir>/plugins/<name>/`: the folder keeps its
old name for compatibility). Server mods are managed with `sv_lua_mods` and
`sv_lua_mod_*`; the older names `sv_lua_plugins` and `sv_lua_plugin_*` still work.

## 1. Package format

A package is **a zip archive** (`.zip`). For development, a plain folder is accepted
by the same installer. The archive holds the mod's files either at its top level or
inside **one** top folder (which is what zipping a folder produces):

```
hud_widget.zip
└── hud_widget/
    ├── mod.txt          manifest (required for API 1 and 2)
    ├── init.lua         entry point (required)
    ├── lib/draw.lua     modules for require("lib.draw")
    └── README           optional
```

Rules the installer enforces (anything else refuses the whole package; nothing is
written):

| Rule | Limit |
|---|---|
| download or file size | 8 MB |
| files | 256, each at most 1 MB, at most 8 MB unpacked in all |
| entry names | `/`-separated components of `A-Z a-z 0-9 . _ -`; none empty, `.`/`..` or starting with `.`; no backslash, colon or drive letter, no absolute path; at most 8 deep and 200 characters; no two names that differ only in case |
| file types | `.lua .txt .md .json .png .tga .jpg .jpeg .ogg .wav`, and `LICENSE COPYING README AUTHORS` |
| never in a package | `data/` (the mod's storage belongs to the user), symlinks, devices, encrypted entries, zip64, compression other than stored/deflate |
| integrity | every entry's CRC-32 is checked; an entry that inflates past its declared size is refused at that byte (zip bombs) |
| pinning | an optional sha256 of the archive: `https://…/mod.zip#sha256=<64 hex>` or a second argument; a mismatch refuses |

`__MACOSX/` and `.DS_Store` entries are ignored.

## 2. The manifest: `mod.txt`

`key: value` lines; `#` starts a comment line; unknown keys are ignored (a newer
standard may add some).

```
id: hud_widget
title: Health and armor bars
version: 1.0
author: Godnotic
homepage: https://example.org/hud_widget
update: https://example.org/hud_widget.zip
description: Two bars for your health and armor, placed clear of the touch controls.
api: 2
realm: client
main: init.lua
permissions: events, display, players
reason.events: draws the bars on every HUD frame
reason.display: draws the two bars
reason.players: reads your own health and armor
setting.side: choice left left right | Screen edge
setting.width: number 160 80 320 20 | Bar width
setting.numbers: bool 1 | Show the numbers
```

| Key | Required | Meaning |
|---|---|---|
| `id` | API 2: should | the install name: `a-z 0-9 _`, up to 32 characters. It names the folder, the commands (`lua_<id>_…`) and the cvars. It must equal the folder name; without it the installer uses the archive's top folder, then the file name |
| `title`, `version`, `author`, `description`, `homepage` | no | shown in the manager. `version` is free text; the manager compares nothing, it shows old and new |
| `api` | yes (from API 1) | the API level the mod is written for (section 3). No `api:` line = a **legacy** mod |
| `realm` | no | `client`, `server` or `both`. A `server` package is refused by `lua_mod_install` and a `client` one by `sv_lua_mod_install`; `both` may be installed with either |
| `main` | no | the entry point; only `init.lua` exists in this standard (reserved for later) |
| `permissions` | API 3: yes | every tier (or API 1/2 scope) it may ever use, comma or space separated (sections 5 and 13); nothing undeclared can be granted or called |
| `reason.<scope>` | should | one line telling the user why it needs that tier or scope; shown at install, on the turn-on screen and in the manager |
| `icon` | no | a `.png` or `.tga` file in the package (at most 256 KB, 256x256) shown in the Mods lists; without it no picture is shown |
| `setting.<name>` | no | a user setting (below), up to 8 |
| `update` | no | an `https://` URL `lua_mod_update` uses when the mod was not installed from a URL |

**Settings.** `setting.<name>: <type> <default> [args] | <label>`, `<name>` `a-z 0-9`.
Types: `bool` (`0`/`1`), `number <default> <min> <max> [step]`, `choice <default>
<option> <option>…` (2 to 8), `text <default…>`. The engine creates the cvar
`lua_<id>_<name>` (`sv_lua_<id>_<name>` for server mods) with the default, saved in the
config; the manager's **Settings** page edits it; the mod reads it with
`xt.setting("<name>")` (no permission needed: it is its own).

## 3. Versions and API levels

* `api: N` says which API the mod was written against. This engine runs levels **1
  and 2** (`xt.apimax` is 2). A mod asking for a higher level is refused at install
  and shown as *needs API N* if copied in by hand: update the game.
* A level only ever **adds**. Everything API 1 offered keeps working under API 2;
  a mod written for API 1 runs unchanged.
* Scopes introduced at a level can only be requested by mods declaring that level
  (`hooks` and `hooks_write` need `api: 2`).
* API 2 adds: the `hooks` / `hooks_write` scopes and `xt.hook`, `xt.entity`,
  `xt.hooknames` (server), `xt.after`, `xt.every`, `xt.cancel`, `xt.spawn`,
  `xt.sleep`, `xt.setting`, `xt.reserved`, `xt.freespot` (client), the manifest
  keys `id`, `realm`, `homepage`, `update`, `main`, `reason.*`, `setting.*`.
* Legacy mods (no `api:` line) request every API 1 scope and get the "HUD set"
  (`events commands cvar_read display input players state`) until the user grants
  more; their grants are stored apart from API 1+ grants.
* `version:` is the author's; an install over an existing `id` is an **update**
  whatever the versions say (the review shows both).

## 4. Lifecycle

| Step | How | Code | Grants | Storage (`data/`) |
|---|---|---|---|---|
| **Install** | `lua_mod_install <file.zip / folder / https URL>`, the Mods tab's *Install…*, the Android file picker, the launcher | read whole into memory, checked; **nothing written yet** | — | — |
| **Review** | printed in the console and shown in the Mods tab: title, version (and the installed one), author, homepage, files, sha256, every requested permission with its reason; new ones marked NEW | — | — | — |
| **Confirm** | `lua_install_confirm` (or *Install*) / `lua_install_cancel` | written to `<realm>/.install.tmp/`, renamed to `<realm>/<id>/`; the source is recorded in `luastate/<realm>/sources/<id>.txt` | fresh install: **none** (a stale grant file is deleted) | fresh: none |
| **Enable** | turning it on in the Mods tab first shows what it asks for and grants the selected scopes in one step (`lua_mod_permission <id> grant a,b,c` then `lua_mod_enable <id>`); in the console `lua_mod_enable` says which scopes are still missing | loaded (`init.lua`) | as chosen | — |
| **Run** | events, hooks, timers, commands | — | revocation applies at once; a grant restarts a mod that stopped for lack of it | `xt.save` / `xt.load` with `storage` |
| **Disable** | `lua_mod_disable` / the switch | `unload` event, state closed | kept | kept |
| **Update** | `lua_mod_update <id> [file/url]` (default: the recorded source, then `update:`), or installing a package with the same id | same review; old folder replaced | **kept for scopes still requested**; dropped scopes removed; **new scopes ungranted**, and the review and the result say which | **kept** (moved into the new folder) |
| **Uninstall** | `lua_mod_uninstall <id> [keepdata]` / *Remove* (with a confirmation) | folder deleted (never following a symlink) | **deleted** | deleted, unless `keepdata` (then `data/` stays and a reinstall finds it) |

Nothing is ever enabled or granted automatically: not by an install, an update, a
copy into the folder or a server. On/off is kept in `<realm>/enabled.txt`, grants in
`luastate/<realm>/grants/<id>.txt` (`v1 <hex mask>` or `legacy <hex mask>`), both
outside anything a package or a mod can write.

**Storage on uninstall: removed by default.** Uninstalling is the user saying *get
rid of it*; a mod's saved text can hold anything it chose to keep. `keepdata` is the
explicit opt-out (useful before a reinstall).

**States** the manager shows: `off`, `running`, `enabled` (on, but mods are switched
off), `error` (stopped; the message is kept), `unfinished` (*did not finish loading
last time*: the engine died while it loaded, so it was switched off; turning it on
again is the retry).

## 5. Permission scopes

| Scope | API | Allows |
|---|---|---|
| `events` | 1 | `xt.on` and the payloads of game events (keys, chat, deaths…) |
| `commands` | 1 | its own console commands `lua_<id>_<name>` |
| `cvar_read` | 1 | `xt.cvar` (private and secret cvars stay hidden) |
| `cvar_write` | 1 | `xt.setcvar` on the realm's allowlist and its own cvars |
| `exec` | 1 | `xt.exec` of allowlisted commands (rate limited) |
| `storage` | 1 | `xt.save` / `xt.load` in its own `data/`; archives its own cvars |
| `display` | 1 | drawing, sounds, screen size, `xt.reserved`, `xt.freespot` (client) |
| `input` | 1 | fingers, mouse, keys held (client) |
| `players` | 1 | player names and scores; on the server, player state |
| `messages` | 1 | `xt.tell`, `xt.centerprint`, `xt.broadcast` (server, rate limited) |
| `state` | 1 | map, match time, in-game state, `xt.realtime` |
| `hooks` | 2 | `xt.hook` on the game's mutator hooks, reading their arguments; `xt.entity`, `xt.hooknames` (server) |
| `hooks_write` | 2 | changing a hook's declared outputs and cancelling a hook that allows it (server) |

`hooks_write` is separate from `hooks` on purpose: watching the game and changing
its rules are different decisions. Without `hooks_write` a server mod's handlers still
run and see everything; what they set is ignored.

A call without its scope raises `permission denied: <scope>; not granted yet:
<every missing scope> (… grant all)` and an uncaught error stops the mod. Grant in
the Mods tab (each scope has a switch with the reason) or with
`lua_mod_permission <id> grant <scope>[,<scope>…]|all` / `revoke …`. Grants cannot
change while a server's text may be in the command buffer.

## 6. Events and hooks

**Events** (`xt.on(name, fn)`, scope `events`), both realms: `frame`, `map`,
`unload`. Client: `hud`, `draw`, `key`, `menu`. Server: `connect`, `disconnect`,
`spawn`, `death`, `damage` (return a number to replace it), `chat` (return false
to hold it back), `pickup` (false stops it), `clientcmd` (true when handled),
`matchend`. Arguments: [MODDING.md](MODDING.md).

**Hooks** (server, scope `hooks`). `xt.hook(name, fn)` subscribes to one of the
game's mutator hooks; there are 172 (LUA_HOOKS.md lists them with their arguments
and what the game says they do). The handler gets one table: each argument under
its position key (`e0`, `f4`, `s1`, `v2`, `i1`, `b5`: type letter and the game's
index) and, where the game names it plainly, under that name (`h.damage`). Entities
are numbers (a player's is their player id; `xt.entity(n)` describes one).

```lua
xt.hook("Damage_Calculate", function(h)
  if h.target ~= h.attacker then h.damage = h.damage * 0.5 end   -- needs hooks_write
end)
xt.hook("ForbidThrowCurrentWeapon", function(h) return true end) -- cancel: needs hooks_write
```

* **outputs**: the arguments the game declares `o(…)` may be changed (numbers,
  booleans, vectors, strings; never entities). NaN and infinities are ignored.
  Server mods run in name order; each sees the previous one's changes.
* **cancel**: where the game's description says to return true, a handler
  returning `true` does that.
* **cost**: a hook's bridge callback is in the game's hook chain only while a
  running server mod with `hooks` subscribes to it, so an unused hook costs nothing
  (no call at all; the only fixed cost is one builtin call per server frame that
  asks whether the subscriptions changed). Measured (local bot match, 9 players,
  64 server frames/s, desktop x86-64): a trivial handler costs 2.2 µs per call on
  `PlayerPhysics`, 1.5 µs on `PM_Physics`, 1.0 µs on `PlayerPreThink`, 0.8 µs on
  `GetPressedKeys`, 4.1 µs on `SV_StartFrame`, argument hand-over included: all
  five together about 51 µs per server frame (0.3 % of a 16.7 ms frame).
  `sv_lua_hooks` shows the live numbers.
* **generated**: `scripts/gen-lua-hooks.py` reads the declarations
  (`qcsrc/server/mutators/events.qh`, `qcsrc/common/mutators/events.qh` and two
  mutator headers) and writes the engine table, the QC callbacks and LUA_HOOKS.md;
  run it after an upstream sync (the test suite checks it is current). A game build
  whose hook list differs from the engine's gets no hooks (and a warning) rather
  than wrong ones.
* Client-side hooks (CSQC) are not bridged: a remote server may replace the client
  game code, and client mods have the events above.

## 7. Long-running work ("while loops")

Every call into a mod — an event, a hook, a command, a timer step — has a budget:
`lua_instructions` (2,000,000), `lua_time_ms` (40 ms), `lua_memory_mb` (16 MB);
hook calls `lua_hook_ms` (5 ms) and, per server mod, `lua_hook_frame_ms` (25 ms of hook
calls per frame). **A literal endless loop is stopped by design** — the game must
keep drawing frames. Continuous work is written as work the engine resumes:

```lua
-- a loop that runs forever, one step at a time
xt.spawn(function()
  while true do
    check_something()
    xt.sleep(0.5)        -- pause; the game runs; resume about 0.5 s later
  end
end)

local id = xt.every(10, function() print("every 10 s") end)   -- return false to stop
xt.after(3, function() print("once, 3 s from now") end)
xt.cancel(id)
```

* `xt.spawn(fn)` runs `fn` as a coroutine; `xt.sleep(s)` (only inside one) pauses it,
  `xt.sleep(0)` until the next frame. Each stretch between sleeps is one call.
* `xt.every(s, fn)` (0: every frame), `xt.after(s, fn)`, `xt.cancel(id)`. At most 64
  timers and coroutines per mod. All of a mod's steps due in one frame share one
  call's budget.
* For per-frame work there is also the `frame` event.
* Everything runs on the game's thread between frames; nothing runs in parallel.

## 8. Stability promise

**A mod can never stop the game.** An error, a limit (instructions, time,
memory, the hook budgets), an error in a coroutine or a timer, or an engine frame
abandoned during its call (loading screen cancel) stops **that mod only**: its state
is closed, it is marked `error` with the message, the message is in its output
(`lua_mod_log`, the manager's Output page), and every other mod and the match go
on. A mod that kills the engine while it loads (it cannot, by the limits above, but
a native crash could) is switched off at the next start (*did not finish loading*).

What this covers: everything a mod's Lua code can do — loops, recursion, memory,
patterns, the table library, coroutines, `pcall` (a mod cannot catch its own death),
hooks, timers. What it does not cover:

* **hook outputs are trusted to the game's own checks.** A server mod with
  `hooks_write` can set a hook's output to a value the game code then rejects in its
  own way (a QC runtime error ends the map, as it would for a broken mutator). Only
  declared outputs are writable, never entities, never NaN; grant `hooks_write` only
  to server mods you trust to change the rules.
* bugs in the engine itself (please report them).
* what a granted scope legitimately does (a server mod with `messages` can spam within
  its rate limit; one with `hooks_write` can make a match unplayable).

## 9. Screen space (client mods)

The touch HUD and the game's HUD already use parts of the screen: the top band
(chat on the left, the timer in the middle, scores, the console button and MENU on
the right), the bottom corners (move stick; aim and fire buttons), the crosshair in
the middle, and the weapon/status strip at the bottom centre. `xt.reserved()` returns
these as rectangles in the 2D coordinates `xt.draw` uses; `xt.freespot(w, h,
["left"|"right"])` gives a position for a `w × h` panel clear of them (the left or
right edge, below the top band). Put panels there by default and let the user move
them with a setting.

## 10. Installing from elsewhere

* **URLs**: `https://` only, redirects included (a redirect to `http://` fails);
  through the engine's libcurl, a transfer at a time, the game keeps running
  (`lua_install_status`, the Mods tab shows progress). The web build has no libcurl
  and downloads with the browser's `fetch()` under the same rules (no cookies, no
  referrer; `touch_webfetch.h`). There the host must also allow requests from web
  pages (CORS): `raw.githubusercontent.com` does, GitHub's release download links
  do not. A refused link says so and points to *Pick file*.
* **Files**: an absolute path, or a path relative to the user folder.
* **Web**: *Pick file* in the Mods tab opens the browser's file picker, and a `.zip`
  dropped on the page goes the same way (`web/mods.js`, the same file handshake as
  Android's): the page copies the zip, at most 8 MB, to `<userdir>/luastate/incoming/`
  and the engine checks it and shows the review. Nothing is enabled or granted.
* **Android**: *Pick file* in the Mods tab opens the system picker; the app copies
  the zip to `<userdir>/luastate/incoming/` and the engine installs it from there
  with the same checks and the same review.
* **Launcher**: its Mods page lists client mods and server mods with their permission state.
  *Install from file…* leaves `<userdir>/luastate/incoming/request.txt` (`mod <path>`
  or `plugin <path>` for a server mod); the game reads it once at start, opens the Mods tab and shows
  the same review. The launcher itself never unpacks or enables anything.
* **Servers**: a server can never install, update, remove, enable or grant
  anything: these commands refuse while a server this engine does not host may have
  put text in the command buffer (stufftext, its CSQC's `localcmd`), until that text
  has run.

## 11. Out of scope: native mods

Native code (a `.so`/`.dll` loaded into the game) is **not** part of this standard.
Code in the engine's process can read and write all of its memory, open files and
sockets, and crash the process; no in-process check can contain it, so it could not
honour sections 5 and 8. Lua is the mod language because its interpreter is the
sandbox.

## Appendix A (proposal, not implemented): out-of-process native mods

If native mods are wanted later, they would run **as separate processes** that
talk to the engine over a pipe, sandboxed by the operating system, so the promise
above still holds (a dead mod process is just a stopped mod).

* **Transport**: the engine starts `mod-host <package>` with stdin/stdout as a
  pipe (or a Unix socket on desktop). Messages are length-prefixed JSON (4-byte
  little-endian length, then UTF-8 JSON), at most 64 KB each.
* **Handshake**: engine → `{"t":"hello","api":2,"realm":"server","grants":["events","messages"]}`;
  mod → `{"t":"hello","id":"…","version":"…","subscribe":["death","hook:Damage_Calculate"]}`.
* **Events**: engine → `{"t":"event","seq":17,"name":"death","args":[3,5,"rocket"]}`;
  hooks the same, and for writable hooks the engine waits at most the hook budget for
  `{"t":"result","seq":17,"set":{"damage":12},"cancel":false}`; a late or missing
  answer counts as "no change" and repeated lateness stops the mod.
* **Calls**: mod → `{"t":"call","seq":4,"fn":"tell","args":[3,"hi"]}`, answered with
  `{"t":"ret","seq":4,"ok":true,"value":null}`. Exactly the `xt` functions its grants
  allow, checked by the engine as for Lua.
* **Sandbox**: Linux: a seccomp filter (read/write on its pipe, memory, exit), no
  network or file namespaces (bwrap-style), rlimits on CPU and memory; Android: an
  isolated service process (`android:isolatedProcess`); Windows/macOS: AppContainer /
  App Sandbox. No native mod on the web.
* **Packages**: the same zip and manifest, with `main: <binary>` per platform and
  architecture and a required sha256 per binary; the same review, and an explicit
  "runs native code" warning.

## Appendix B: checklist for a mod author

1. `mod.txt` with `id`, `title`, `version`, `api: 2`, `realm`, `permissions` and a
   `reason.<scope>` line for each.
2. Ask for the fewest scopes; `hooks_write` only when you change the game.
3. No endless loops: `xt.spawn` + `xt.sleep`, `xt.every`, the `frame` event.
4. Client panels at `xt.freespot(...)` by default, movable with a `setting.`.
5. Zip the folder; publish over HTTPS and give the sha256 next to the link.
6. Try it: `lua_mod_install /path/to/your.zip` (a server mod: `sv_lua_mod_install`),
   `lua_install_confirm`, turn it on in the Mods tab, watch `lua_mod_log <id>` or the
   Output page.

Examples: [`touch/lua/examples`](../touch/lua/examples) — `mods/hud_widget` (HUD widget
with settings), the server mods `plugins/damage_scale` (a write hook) and
`plugins/leader_watch` (a coroutine doing continuous work), and the API 1 ones.

## 12. API 3: tiers

API level 3 (`api: 3`; this engine runs 1 to 3, `xt.apimax` is 3) replaces the scopes of
section 5 with four tiers; the details and every function are in
[LUA_API.md](LUA_API.md).

| Tier | Bit in the grant file | Turned on | Allows |
|---|---|---|---|
| `standard` | 0 (always on) | with the mod, no prompt | everything a normal mod does (every API 1/2 scope except `hooks_write`) |
| `gamerules` | 12 (was `hooks_write`) | a switch | hook outputs and cancelling; changing players, entities and the match (server) |
| `internet` | 13 | a switch | `xt.http` to the hosts of the manifest's `http:` line |
| `engine_full` | 14 | a switch with a warning | `xt.engine.cmd`, `xt.engine.cvar`, `xt.engine.setcvar`; private cvars stay hidden |

* **Manifests.** `permissions:` takes tier names (API 3) or the old scope names, which
  map onto tiers; `reason.<tier>` or `reason.<scope>` (the first given for a tier is
  shown). New keys: `http: host, host` (where `xt.http` may go), `setting.<name>.desc`,
  `setting.<name>.group`, and the setting types `int`, `float`, `color` (six hex digits)
  and `key` (a key name). A folder with only `init.lua` runs at the standard tier and
  can never be granted more (section 13).
* **Grants.** Stored as before (`luastate/<realm>/grants/<id>.txt`, `v1 <hex>`): the bits
  above keep their API 1/2 positions, so existing grant files mean the same thing
  (`hooks_write` granted = `gamerules` on). `lua_mod_permission <id> grant <tier>|all`;
  `all` never includes `engine_full`; for a mod installed from a link, `engine_full`
  needs `... grant engine_full confirm` within a minute of the first grant, which prints
  the warning. Like every manager command, none of this can come from a server.
* **Lifecycle.** Unchanged (section 4), except that turning a mod on is all it needs for
  the standard tier.
* **Hooks (section 6).** Client mods can now subscribe to the client program's hooks
  too (54 of them, [LUA_HOOKS.md](LUA_HOOKS.md)); they exist while this game's own
  client code runs (`xt.csqc()`). On the client only the drawing hooks are writable,
  with the standard tier. A hook's arguments now reach the engine in one builtin call
  (`touch_lua_hook_run`, #1753; `touch_lua_hook_more`, #1754, for arguments past the
  seventh) instead of one per argument; the API 2 builtins #1741-#1746 stay for game
  code built before.
* **Stability (section 8)** covers every API 3 entry point: callbacks the engine keeps
  (buttons, pages, channels, HTTP answers, chat commands) run under the same limits as
  events, plus a per-mod budget per frame (`lua_frame_ms`: calls past it are skipped for
  that frame). Handles to engine objects are checked on every use; no API returns a
  pointer. While a mod's code runs, no hook or event is delivered to any mod.

## 13. Declared permissions

What a mod may do is what its manifest **declares** and the player **granted**; the
engine checks both on every call (`tlua_has`: declared ∩ granted ⊇ needed), so a grant
file edited by hand cannot reach past the manifest and a revoke applies to the next call.

| Declared | Granted | Effect |
|---|---|---|
| `standard` (implied for every mod) | (always) | allowed |
| a tier | yes | allowed |
| a tier | no, or not decided yet | refused: *the <tier> tier is not turned on for this mod* |
| not declared | yes (written by hand) | refused: *needs the <tier> tier (add "permissions: <tier>" to mod.txt)* |
| not declared | asked for | the grant is refused: *does not declare the <tier> tier* |
| no `mod.txt` | anything | standard only, listed as *declares nothing*; a grant is refused |

* **Packages.** `lua_mod_install` refuses a package without `mod.txt`. An API 3
  manifest without a `permissions:` line declares the standard tier only.
* **Shown before it runs.** The install review lists each declared tier in plain words
  with the mod's `reason.<tier>`. Turning a mod on is one screen:
  `lua_mod_enable <id> with <tier>[,<tier>...]|all|none [confirm]` grants the chosen
  declared tiers and turns it on (a standard-only mod: one tap, nothing to choose).
* **Not decided yet.** Each mod has a *seen* mask, `luastate/<realm>/seen/<id>.txt`
  (`<hex>`; without the file, what is granted counts as seen). A declared optional tier
  not in it is *not decided yet*: off, and named by `lua_mod_enable`, the Mods page,
  `lua_mod_info` and the 5th field of `_cl_lua_permissions`. Any grant, revoke, decline
  or `lua_mod_enable ... with` marks every declared tier seen. So an update that declares
  more keeps the old grants and asks about the new tiers only;
  `lua_mod_permission <id> decline <tier>` settles one as off.
* **Controlled.** `lua_mod_permission <id> grant|revoke|decline <tier>|all [confirm]`,
  declared tiers only; `all` never includes `engine_full`; `engine_full` keeps its
  warning and, for a mod installed from a link, the confirm step. Nothing here can come
  from a server.
* **What it used.** Since it last started: calls per optional tier, the hosts `xt.http`
  contacted and the number of `xt.engine.cmd` calls, in `lua_mod_info` and in the
  `_lua_detail` keys `used`, `hosts` and `engine_commands`.
* **Status for the UI.** `_cl_lua_mods` / `_sv_lua_plugins`:
  `name<TAB>state<TAB>title<TAB>error<TAB>description<TAB>icon`, `icon` being
  `mod:<id>/<file>` or empty. `_cl_lua_permissions`:
  `name<TAB>mode<TAB>declared<TAB>granted<TAB>not decided yet` (mode `v1`, `legacy`,
  `invalid` or `none`). `_lua_detail` (the selected mod) adds `declares` (tiers, or
  `nothing`), `unseen`, `icon`, `used`, `hosts`, `engine_commands`.
* **Icons.** A `mod:<id>/<file>` image ref is read by the engine from that mod's folder
  (client mods first, then server mods), `.png` or `.tga`, at most 256 KB and 256x256,
  through the same confined file read as the mod's code; anything else gives no image.
