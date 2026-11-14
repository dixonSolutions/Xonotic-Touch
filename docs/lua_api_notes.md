## Calls, errors and limits

Every function above checks its tier on every call against what the mod declares and
what the player granted. A call without its tier raises
`permission denied: the <tier> tier is not turned on for this mod` (declared, not granted),
or `needs the <tier> tier` when `mod.txt` does not declare it (whatever is granted), and an error the mod does not catch with
`pcall` stops that mod, never the game.

| Limit | Cvar | Default | What happens past it |
|---|---|---|---|
| instructions per call | `lua_instructions` | 20,000,000 (loading: 10x) | the mod stops ("instruction limit") |
| time per call | `lua_time_ms` | 100 ms (loading: 2 s) | the mod stops ("time limit") |
| memory per mod | `lua_memory_mb` | 64 MB | the mod stops ("memory limit") |
| time per mod per frame | `lua_frame_ms` | 50 ms | the rest of that frame's calls into the mod are skipped (counted by `lua_mod_top`) |
| time per hook call | `lua_hook_ms` | 10 ms | the mod stops |
| hook time per mod per frame | `lua_hook_frame_ms` | 50 ms | the mod stops ("hook budget") |

A server cannot change these cvars (a change while connected to a server this game does
not host is put back). `lua_mod_top` (and the `_lua_top` cvar, JSON) shows each mod's CPU
time, calls, skipped calls and memory over the last second.

**Re-entrancy.** While a mod's code runs, no event or hook is delivered to any mod. Game
code that a mod's own API call runs (an item spawning, a player changing team, a vote)
may fire hooks; they are not delivered. So a handler never runs inside another mod's
call, and a mod never sees its own actions echoed back as hooks.

## Entity handles (server mods)

`xt.ent(n)` (and `xt.ents.find`, `xt.ents.spawn`, `tr.ent`) give **handles**: userdata
that name an entity without holding it. Each use checks that the entity is still the
one the handle was made for: removed, its slot reused by another entity, a player slot
reused by another player, or a new map, and the handle is **stale**. `h:valid()` says so;
any other use of a stale handle is an error (`stale entity handle`). A handle never
holds a pointer, so a mod can keep handles in tables as long as it likes.

```lua
local p = xt.ent(1)                 -- player 1
print(p.name, p.health, p.armor, p.rockets, p.alive, p.bot)
local o = xt.vec(p.origin)          -- {x, y, z} as a vector
p.health = 150                      -- needs the gamerules tier
p:give("rockets", 20)               -- GiveItems words: rockets, cells, health, armor, devastator, ...
p:take("devastator")
p:teleport(o + xt.vec(0, 0, 64), xt.vec(0, 90, 0))
local box = xt.ents.spawn("item_health_mega", {origin = o + xt.vec(64, 0, 16)})
if box and box:valid() then box:remove() end
```

**Reading** `h.<field>` reads any number, vector, string or entity field of the game by
its name (`h.classname`, `h.origin`, `h.velocity`, `h.team`, `h.frags`, ...). Aliases:
`armor` (`armorvalue`), `shells`, `bullets`/`nails`, `rockets`, `cells`, `fuel`
(`ammo_*`), `name` (`netname`); computed: `id`, `alive`, `bot`. Function fields read as nil.

**Writing** (gamerules) is limited to `health`, `armor`, the ammo fields, `origin` (the
entity is moved properly, a player too), `velocity`, `angles` (a player's view turns),
`avelocity`, `gravity`, `scale`, `alpha`, `colormod`, `glowmod`, `effects`, `max_health`;
on entities the mod spawned also `target`, `targetname`, `message`, `netname`, `wait`,
`speed`, `delay`, `count`, `cnt`, `dmg`, `nextthink`. Numbers must be finite and within
±10,000,000.

**Methods**: `valid`, `num`, `is_player`, `is_bot`, `get(field)`, `set(field, value)`,
`remove` (only entities this mod spawned), and for players `tell`, `centerprint`, `give`,
`take`, `teleport`, `respawn`, `setteam(1..4)`, `kick([reason])`, `ban(minutes [, reason])`.
Kicks, bans and map changes are queued as the console commands an admin would type and
run between frames.

**Spawning** (`xt.ents.spawn`, gamerules) creates entities the way the map would, from
these classes only: `item_*`, `weapon_*`, `target_position`, `info_teleport_destination`,
`info_notnull`, `func_pointparticles`, `trigger_push`, `trigger_teleport`; `model` and
`noise*` fields are refused. 64 live entities per mod, 16 a second; they are removed
when the mod stops. The game may refuse an item (a mutator that removes it): then
`spawn` returns nil.

## Client mods on public servers

A client mod runs in the player's game. What it gets depends on the server:

| | Local games, Godnotic servers, servers using this game's client code | A server that sends other client code |
|---|---|---|
| drawing, HUD panels, touch buttons, binds, sounds, effects, pages | yes | yes |
| events from the engine: `frame`, `hud`, `draw`, `key`, `touch`, `print`, `chat`, `centerprint`, `weapon`, `damage`, `spawn`, `death`, `spectate`, `score`, `connect`, `disconnect` | yes | yes |
| events from the game code: `notification`, `kill`, `hit`, `pickup`, `match`, `hud_overlay`, `scoreboard` | yes | no |
| client hooks (`xt.hook`) | yes | no (the subscription is accepted, the handler never runs) |
| `xt.entities` | yes (see the offline-only rule) | empty |
| mod channels and server pages | with a server whose mods use them | no (nothing is sent) |

`xt.csqc()` (or `xt.game.info().csqc`) says which is in effect: `"godnotic"`, `"foreign"`
or `"none"` (not connected).

**The offline-only rule.** In an offline match against bots (this game runs the server,
no other human is connected, no demo plays: `xt.offline()`), `xt.entities` lists every
player (from the local server, so bots outside the view too), projectile and item, so
radars and training overlays work. Anywhere else it lists players and projectiles only
while they are on the screen and not behind a wall, which is what the player sees
anyway; items are always listed (they are part of the map). This is the same gate as
the game's see-through practice helpers, checked in the engine on every call: a mod
cannot get around it.

## Messages between server and client mods

```lua
-- server mod
local ch = xt.channel("ctf_hud@1")
ch:send(nil, {flag = "taken", by = 3})        -- every player (nil) or one player id
ch:on(function(player, msg) print(player, msg.ask) end)
```

```lua
-- client mod
local hud = xt.channel("ctf_hud@1")
hud:on(function(msg) print(msg.flag) end)
if hud:ready() then hud:send({ask = "scores"}) end
```

A channel is a name (`a-z 0-9 _ . @`, up to 32; a version suffix like `@1` is a
convention). A message is one value (usually a table) that `xt.json.encode` can encode,
at most **1 KB** as JSON. `channel:ready()` is true on a client once the server said it
speaks mod channels; before that `send` returns false and sends nothing.

| Limit | Value |
|---|---|
| message | 1 KB of JSON, depth 32 |
| server to one player | 20 a second, bursts of 40; never more than the free space of the player's reliable buffer |
| player to the server | 5 a second, bursts of 10 (the engine's limit; the game's command flood control does not apply to these) |
| a client takes from the server | 60 a second |
| page from a server | 16 KB of JSON in at most 64 parts, 2 a second per player; answers 20 a second |
| handlers | 16 `channel:on` per mod |

**The wire.** No new network message: stock clients and servers are never confused.

* Server to client: `svc_stufftext` lines that start with `//`, which any client's
  console treats as a comment. This game's client takes them before the console and
  never runs them: `//xt1h <version>` (hello), `//xt1m <channel> <value>` (a message),
  `//xt1p <page> <part> <parts> <chunk>` (a page in parts), `//xt1c <page prefix>`
  (close pages).
* Client to server: `clc_stringcmd`, only after the server's hello: `xthello <version>`,
  `xtmsg <channel> <value>`, `xtpage <page> <row> <event> <value>`.
* `<value>` is the JSON with every byte outside `A-Z a-z 0-9 _ . ~ -` written as `%XX`, so
  a frame is one line of tokens without spaces, quotes, `;` or `$`. Everything read from
  the other side is checked before use: names by character set and length, values by
  size and escape, JSON by depth and size, pages by part order and total size.

**The player decides.** `cl_lua_channels 0` turns it off; `lua_channel_block
<address|current|*> [channel|pages|*]` refuses one server's (or every server's)
messages, kept in `luastate/mods/channels_blocked.txt` (`lua_channel_unblock` undoes);
`cl_lua_server_pages 0` refuses pages from servers. None of these can be changed by a
server.

## UI descriptions

Mods describe pages as rows; the engine checks and bounds them and hands each page to
the menus as one JSON document (schema 1). The QuakeC menu, the Android app and the
Linux app each draw it with their own widgets.

```lua
local page = xt.settings.page{        -- or xt.menu.page{...}
  id = "main", title = "My mod",
  rows = {
    {type = "section", text = "Look"},
    {type = "switch", id = "big", label = "Big text", setting = "big"},  -- bound to setting.big
    {type = "slider", id = "alpha", label = "Opacity", min = 0, max = 1, step = 0.1, value = 0.8,
      on_change = function(v) print("alpha", v) end},
    {type = "choice", id = "side", label = "Side", options = {"left", "right"}, value = "left"},
    {type = "field", id = "tag", label = "Tag", value = "", placeholder = "your tag"},
    {type = "button", id = "reset", label = "Reset", on_press = function() print("reset") end},
    {type = "list", id = "maps", label = "Maps", items = {"stormkeep", "afterslime"},
      on_select = function(id) print("picked", id) end},
    {type = "progress", id = "load", label = "Loaded", value = 0.4},
  },
  on_change = function(row, value, event) print(row, value, event) end,
}
page:set("load", "value", 0.9)        -- change a row; the page is drawn again
```

**Rows.** The row types are the UI bridge's node types (docs/UI_BRIDGE.md on
`work/uibridge`, section "Mods"), so a mod page maps one to one onto the screens the
menus already draw. Every row has `type`; rows that hold a value or act have an `id`
(`a-z 0-9 _`, up to 32). Fields by type (all strings at most 256 bytes):

| type | fields | events | UI bridge node |
|---|---|---|---|
| `section` | `text` | - | `section` |
| `card` | `title`, `children` (rows) | - | `card` |
| `text` (`label`) | `text` | - | `text` |
| `divider` | - | - | `divider` |
| `switch` | `label`, `value` (bool) or `setting` | change | `switch` |
| `slider` | `label`, `min`, `max`, `step`, `value` (number) or `setting` | change | `slider` |
| `stepper` | `label`, `min`, `max`, `step`, `value` (number) or `setting` | change | `stepper` |
| `choice` | `label`, `options` (up to 16), `value` or `setting` | change | `choice` (options as `{value, label}`) |
| `field` | `label`, `value`, `placeholder`, `maxlen`, or `setting` | change | `field` (`kind: text`) |
| `colour` (`color`) | `label`, `value` (`rrggbb`) or `setting` | change | `colour` (`#rrggbb`) |
| `keybind` | `label`, `value` (a key name) or `setting` | change | `keybind` (value as a one-name array) |
| `button` | `label`, `style` (`"primary"`, `"destructive"`) | press | `button` (`action` normal) |
| `list` | `label`, `items` (strings, or `{id, label, detail}`; up to 64), `value` (the selected id) | select | a `section` of `row` nodes, each with an action |
| `image` | `file` (a PNG/TGA of the mod), `w`, `h` | - | `image` (ref `mod:<id>/<file>`) |
| `progress` | `label`, `value` (0..1) | - | `progress` |

Any row may have `desc` and `disabled = true`. Limits: 64 rows in all, cards nested 4
deep, 8 pages per mod, 16 KB of JSON (inside the bridge's bounds for mods: 256 nodes,
depth 8, 512-byte strings, 32 KiB). A row bound to a setting maps onto the bridge's bind
`mod:<id>.<setting>`. A page object has `page:set(row, field, value)`,
`page:rows(rows)`, `page:value(row)`, `page:remove()`; `xt.page.get(id)` finds it again.

**Values persist** when a row is bound to one of the manifest's settings
(`setting = "<name>"`): the renderer's change sets the cvar `lua_<mod>_<name>` (checked
against the setting's type and range) before the mod's callbacks run, and the config
keeps it. Other rows keep their value in the description for the session.

**The manifest's settings are a page by themselves.** Every mod with `setting.<name>`
lines gets a page `<mod>.settings` (kind `"settings"`) with one row per setting,
grouped by `setting.<name>.group` and described by `setting.<name>.desc`, without code.

**The JSON** a renderer reads:

```
{"schema":1, "id":"<mod>.<page>", "origin":"<mod>", "realm":"client"|"server",
 "kind":"settings"|"menu"|"server", "title":"...", "desc":"...", "from_server":false,
 "rows":[{"type":"switch","id":"big","label":"Big text","setting":"lua_mymod_big","value":true}, ...]}
```

`kind` says where the page belongs: `settings` under Settings > Mods, `menu` as a page of
the mod in the Mods menu, `server` a page a server showed (`from_server` is true: the
renderer must mark it "Page from the server" and always offer a way to close it; its
id starts with `server.`).

**Where renderers read it.** Until the UI bridge's store exists (docs/UI_BRIDGE.md on
`work/uibridge`), the engine publishes:

| | |
|---|---|
| `_lua_pages` (cvar) | the index, JSON: `[{"id","origin","kind","title","from_server"}]` |
| `_lua_page_select` (cvar) | set it to a page id ... |
| `_lua_page` (cvar) | ... and this holds that page's JSON |
| `lua_pages`, `lua_page_json <id>` | the same in the console |
| `lua_page_event <id> <row> change\|press\|select [value]` | what a renderer sends back |
| `_lua_hud_panels` (cvar) | HUD panels and touch buttons, JSON: `[{"mod","id","kind","title","x","y","w","h","xcvar","ycvar"}]`, for a HUD editor (moving one sets its `xcvar`/`ycvar`) |

In the engine all of it goes through five functions of `engine/darkplaces/touch_lua_ui.c`
(`tlua_ui_register`, `tlua_ui_update`, `tlua_ui_remove`, `tlua_ui_publish` and
`tlua_ui_event` for the way back). When the UI bridge is in the tree
(`engine/darkplaces/touch_uib.h`), every page also goes to its store:
`TouchUIB_ModSetPage(<mod>, <page key>, ...)` with the rows turned into the bridge's
page shape (a root `section`, then the rows as nodes, parents first, binds
`mod:<mod>.<row>`, a `list` as a `choice` with `style: radio`, server pages under the
key `server-<page>`), `TouchUIB_ModRemove` when it goes, and the bridge's `value` /
`action` events (`TouchUIB_ModSetEventHandler`) come back through `tlua_ui_event`.

**The Mods manager's own state** (for the menus that draw it): `_cl_lua_mods` and
`_sv_lua_plugins` (one line per mod: `name<TAB>state<TAB>title<TAB>error<TAB>description<TAB>icon`,
`icon` an image ref `mod:<mod>/<file>` or empty), `_cl_lua_permissions`
(`name<TAB>mode<TAB>declared tiers<TAB>granted tiers<TAB>not decided yet`), `_lua_select`
+ `_lua_detail` (one mod in detail: `key<TAB>value` lines; `perm` lines are
`tier<TAB>granted 0/1<TAB>reason`; `declares`, `unseen`, `icon`, `used`
(`tier<TAB>N calls`), `hosts`, `engine_commands`), `_lua_last` (the manager's last message), `_lua_top`
(cost, JSON), and the commands `lua_mod_enable/disable/permission/install/update/uninstall`.
A tier switch is `lua_mod_permission <mod> grant|revoke|decline <tier>` (declared tiers
only); turning a mod on is `lua_mod_enable <mod> [with <tiers>|all|none|keep]` (standard needs
no grant); `engine_full` for a mod installed from a link needs
`lua_mod_permission <mod> grant engine_full confirm` after the first grant printed its
warning.

## Testing helpers

| | |
|---|---|
| `lua_mod_log <mod>` | the mod's recent output and errors |
| `lua_mod_top` | per-mod CPU, calls, skipped calls, memory |
| `lua_mod_reload <mod>` | restart it after editing its files |
| `lua_mod_simulate key <name> <1/0>` | press or release a key for client mods |
| `lua_mod_simulate finger <0-9> <x> <y> <1/0>` | put a finger down or lift it (2D units) |
| `lua_page_event <id> <row> <event> [value]` | act on a page as a renderer would |
| `lua_hooks` / `sv_lua_hooks` | hooks subscribed, calls and microseconds per call |
| `scripts/test-mods.sh` | the example suite (docs/MODDING.md, "Testing your mod") |

## For Insanotic mod authors

Hook handlers can read the arguments under the game's index as well (`h[4]` is
Insanotic's `ev[4]`), and writing `h[4]` writes the output. Entities in hooks are
numbers here (`xt.ent(n)` makes a handle); vectors are `{x, y, z}` tables (`xt.vec` adds
the arithmetic Insanotic's `xon.vec` has). Returning `true` cancels a hook where the
game says so, as there.
