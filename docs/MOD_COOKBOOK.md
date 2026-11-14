# Mod cookbook

Recipes for common mods. Each one is a working example in
[`touch/lua/examples`](../touch/lua/examples) with a README (what it shows, its tier,
how to install it on Android, Linux and the web, what you should see), and each is run
by `scripts/test-mods.sh` in a headless match, so the code here works. The functions are
in [LUA_API.md](LUA_API.md), the game's hooks in [LUA_HOOKS.md](LUA_HOOKS.md); start with
[MODDING.md](MODDING.md).

Build every example into an installable zip with `scripts/build-example-mods.sh`
(`build/mods/<name>.zip`, `build/plugins/<name>.zip`).

Every example's `mod.txt` declares exactly the tiers its code uses, with a reason for
each (`permissions: standard` and `reason.standard: ...` for most of them), and
`scripts/test-mods.sh` fails an example that calls a tier it does not declare. Copy that
habit: the player reads your reasons before your mod first runs, and nothing you leave
out can be granted later. `panel_clock` also shows a package icon (`icon: icon.png`).

## A HUD panel with a settings page

[`examples/mods/panel_clock`](../touch/lua/examples/mods/panel_clock)

Ask the engine for a panel instead of picking coordinates: it finds a place clear of the
touch controls and the stock HUD, draws it in the HUD pass, and lets the player move it.
Settings declared in `mod.txt` become a settings page by themselves; a page of your own
can bind rows to them.

```
setting.seconds: bool 1 | Show seconds
setting.seconds.group: Look
setting.size: int 16 10 32 2 | Text size
```

```lua
xt.hud.panel{
  id = "clock", title = "Clock", w = 120, h = 34, side = "left",
  draw = function(x, y, w, h)
    local fmt = xt.setting("seconds") == "1" and "%H:%M:%S" or "%H:%M"
    xt.draw.text(x + 8, y + 8, xt.time.date(fmt), tonumber(xt.setting("size")) or 16)
  end,
}
xt.settings.page{
  id = "main", title = "Clock panel",
  rows = {
    {type = "switch", id = "seconds", label = "Show seconds", setting = "seconds"},
    {type = "button", id = "reset", label = "Reset", on_press = function() xt.setcvar("lua_panel_clock_size", "16") end},
  },
}
```

## A touch button and a key

[`examples/mods/touch_weapon`](../touch/lua/examples/mods/touch_weapon)

```lua
xt.touch.button{
  id = "next", label = "NEXT", w = 84, h = 84, side = "right",
  press = function() xt.exec("weapnext") end,
  hold = function() end,          -- every frame while the finger stays
  release = function() end,
}
xt.bind("F7", function(key) xt.exec("weapnext") end)
```

Test it without a touchscreen: `lua_mod_simulate finger 0 <x> <y> 1` (where the button
is: `_lua_hud_panels`), then `... 0`; `lua_mod_simulate key F7 1`.

## Hits, damage and the kill feed

[`examples/mods/hit_feedback`](../touch/lua/examples/mods/hit_feedback)

```lua
xt.on("damage", function(amount, health, armor) print("took", amount) end)
xt.on("hit", function(amount) xt.sound("sound/misc/hit.wav") end)   -- this game's client code
xt.on("kill", function(victim, attacker, name) print(attacker, ">", victim) end)
xt.on("notification", function(kind, name, text, args)
  if kind == "info" and name:find("_MURDER", 1, true) then return false end  -- hide: we draw our own
end)
```

## Hiding what the server shows

[`examples/mods/quiet_center`](../touch/lua/examples/mods/quiet_center)

```lua
xt.on("centerprint", function(text)
  if text:find("Prepare", 1, true) then return false end
end)
```

`print`, `chat` and `centerprint` work on every server; `notification` needs this game's
client code (`xt.csqc() == "godnotic"`).

## A game rule

[`examples/plugins/vampire`](../touch/lua/examples/plugins/vampire) (Game rules tier)

```lua
xt.hook("Damage_Calculate", function(h)
  if h.target == h.attacker then h.damage = h.damage * 0.5 return end
  local a = xt.ent(h.attacker)
  if a and a:is_player() and a.alive then a.health = math.min(a.health + h.damage / 2, 200) end
end)
```

Without the Game rules tier the hook still runs and sees everything; its changes are
ignored and entity writes are refused.

## A server game mode with a client HUD

[`examples/plugins/round_timer`](../touch/lua/examples/plugins/round_timer) +
[`examples/mods/round_hud`](../touch/lua/examples/mods/round_hud)

```lua
-- the server mod
local ch = xt.channel("round_timer")
xt.every(1, function() ch:send(nil, {round = 1, left = 30}) end)
ch:on(function(player, msg) if msg.hello then ch:send(player, {welcome = true}) end end)
```

```lua
-- the client mod
local ch = xt.channel("round_timer")
local state
ch:on(function(msg) state = msg end)
xt.on("frame", function() if not said and ch:ready() then said = ch:send({hello = 1}) end end)
```

Only servers the player joined can talk to the player's mods, and the player can refuse
(`cl_lua_channels 0`, `lua_channel_block current`). The same server mod can show a page
on a player's screen with `xt.page.show(player, {...})`; button presses come back to it.

## Chat commands and a vote

[`examples/plugins/chat_vote`](../touch/lua/examples/plugins/chat_vote)

```lua
xt.chatcommand("time", function(player, args)
  xt.tell(player, "Server time: " .. xt.time.date("%H:%M"))
end, "the server's clock")
xt.chatcommand("shuffle", function(player)
  xt.match.vote("shuffleteams", player)        -- Game rules
end)
```

Players type `!time` in chat (the line is not shown to others) or `cmd time`.

## Items and a teleporter

[`examples/plugins/item_placer`](../touch/lua/examples/plugins/item_placer) (Game rules)

```lua
xt.chatcommand("drop", function(player)
  local p = xt.ent(player)
  local f = xt.forward(p.angles)
  local spot = xt.vec(p.origin) + xt.vec(f.x, f.y, 0):normalize() * 96 + xt.vec(0, 0, 16)
  local item = xt.ents.spawn("item_health_mega", {origin = spot})
  if item then print("spawned", item) end
end)
```

Handles are checked on every use: after `item:remove()`, `item:valid()` is false and
reading `item.origin` is an error. What a mod spawned goes away when it stops.

## Bots

[`examples/plugins/bot_control`](../touch/lua/examples/plugins/bot_control) (Game rules)

```lua
xt.chatcommand("bots", function(player, args)
  local want = tonumber(args) or 0
  local have = 0
  for _, id in ipairs(xt.players()) do if xt.ent(id).bot then have = have + 1 end end
  if want > have then xt.bots.add(want - have) else xt.bots.remove(have - want) end
end)
```

## Keeping data across maps

[`examples/plugins/player_stats`](../touch/lua/examples/plugins/player_stats)

```lua
local stats = xt.data.load("stats", {})
xt.on("death", function(victim, attacker)
  local p = xt.player(attacker)
  if p then stats[p.name] = (stats[p.name] or 0) + 1 end
end)
xt.on("unload", function() xt.data.save("stats", stats) end)
```

## An HTTP leaderboard, and the approval flow

[`examples/plugins/leaderboard_http`](../touch/lua/examples/plugins/leaderboard_http) (Internet tier)

```
permissions: standard, internet
reason.internet: sends the scores to the leaderboard site
http: leaderboard.example.org
```

```lua
xt.on("matchend", function()
  xt.http.post("https://leaderboard.example.org/scores", xt.json.encode({map = xt.map()}),
    function(status, body) print("leaderboard", status) end)
end)
```

The install review shows that the mod declares **Internet**, with its reason, and names the host; nothing
is sent until the player turns that tier on, and only to the hosts listed. Plain `http://`
works only to this machine (for testing). The web build sends it with the browser's
`fetch()`, so there the host must also allow requests from web pages (CORS); a refused
request answers -1.

## A menu page and a Settings entry

[`examples/mods/mod_menu`](../touch/lua/examples/mods/mod_menu)

```lua
local notes = xt.data.load("notes", {})
local page
page = xt.menu.page{
  id = "notes", title = "My notes",
  rows = {
    {type = "field", id = "draft", label = "New note"},
    {type = "button", id = "add", label = "Add", on_press = function()
      table.insert(notes, 1, page:value("draft"))
      xt.data.save("notes", notes)
      page:set("list", "items", notes)
    end},
    {type = "list", id = "list", label = "Notes", items = notes},
  },
}
```

The `setting.*` lines of its `mod.txt` are its entry under Settings > Mods.

## A radar that respects the offline-only rule

[`examples/mods/radar`](../touch/lua/examples/mods/radar)

```lua
xt.hud.panel{id = "radar", w = 120, h = 120, side = "right", draw = function(x, y, w, h)
  local me = xt.view().origin
  for _, e in ipairs(xt.entities("player")) do
    xt.draw.fill(x + w / 2 + (e.x - me.x) / 40, y + h / 2 - (e.y - me.y) / 40, 4, 4, 1, 0.3, 0.3, 1)
  end
end}
```

Offline against bots every player is listed; on other people's servers only the ones
on screen and not behind a wall. The engine decides, so the mod cannot be used to see
through walls online.

## Work that keeps going

[`examples/plugins/ticker`](../touch/lua/examples/plugins/ticker)

```lua
xt.spawn(function()
  while true do
    print("tick")
    xt.sleep(1)        -- the game runs; this resumes a second later
  end
end)
xt.every(2, function() return false end)   -- false: stop repeating
xt.after(4, function() print("once") end)
```

## When a mod goes wrong

[`tests/lua-mods/bad`](../tests/lua-mods/bad) holds mods that misbehave on purpose: an
endless loop in an event (stopped by the time limit), an error in a hook (that mod
stops, the hook goes on for others), a memory hog (stopped at `lua_memory_mb`), and a
runaway client hook (stopped by `lua_hook_ms`). The suite checks that each is stopped
with its reason in `lua_mod_log`, and that every other mod and the match keep running.
