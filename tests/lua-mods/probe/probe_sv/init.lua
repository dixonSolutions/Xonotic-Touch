-- scripts/test-mods.sh drives this through its console commands and checks
-- the "probe_sv:" lines
local function try(label, f, ...)
  local ok, err = pcall(f, ...)
  print("probe_sv: " .. label .. " " .. (ok and "ok" or ("err " .. tostring(err))))
  return ok, err
end

local function above_player(dz)
  local p = xt.ent(1)
  local o = xt.vec(p.origin)
  return o + xt.vec(0, 0, dz or 32)
end

xt.command("tiers", function()
  try("spawn", function() local e = xt.ents.spawn("info_notnull", {origin = above_player(64)}) if e then e:remove() end end)
  try("write", function() xt.ent(1).health = xt.ent(1).health end)
  try("enginecvar", xt.engine.cvar, "hostname")
end, "tier checks")

xt.command("stale", function()
  local e = xt.ents.spawn("info_notnull", {origin = above_player(64)})
  print("probe_sv: stale before " .. tostring(e and e:valid()))
  e:remove()
  print("probe_sv: stale after " .. tostring(e:valid()))
  try("stale read", function() return e.origin end)
  try("stale write", function() e.health = 5 end)
  print("probe_sv: stale xt.ent " .. tostring(xt.ent(e)))
end, "stale handles")

xt.command("private", function()
  print("probe_sv: rcon_password=" .. tostring(xt.engine.cvar("rcon_password")))
  print("probe_sv: hostname=" .. tostring(xt.engine.cvar("hostname")))
  print("probe_sv: plain rcon_password=" .. tostring(xt.cvar("rcon_password")))
  try("manager cmd", xt.engine.cmd, "sv_lua_plugin_enable bad_loop")
  try("expansion", xt.engine.cmd, "echo $rcon_password")
  try("engine echo", xt.engine.cmd, "echo probe_sv engine_full echo ran")
end, "private cvars")

local inside, nested, total = false, 0, 0
local function count() total = total + 1 if inside then nested = nested + 1 end end
xt.hook("FilterItem", count)
xt.hook("Item_Spawn", count)
xt.command("reentry", function()
  local before = total
  inside = true
  local e = xt.ents.spawn("item_armor_small", {origin = above_player(48)})
  inside = false
  print("probe_sv: reentry nested=" .. nested .. " spawned=" .. tostring(e ~= nil) .. " hookcalls_during=" .. (total - before))
  if e and e:valid() then e:remove() end
end, "re-entrancy")

local got, players = 0, {}
xt.channel("probe"):on(function(player, msg)
  got = got + 1
  players[player] = (players[player] or 0) + 1
end)
xt.command("chancount", function() print("probe_sv: channel received " .. got) end, "channel count")

xt.command("world", function()
  local p = xt.ent(1)
  local o = xt.vec(p.origin)
  local tr = xt.world.trace(o, o + xt.vec(0, 0, -4096), p)
  print(string.format("probe_sv: trace fraction %.3f solid=%s gametype=%s", tr.fraction, tostring(xt.world.contents(tr.endpos + xt.vec(0, 0, -4)).solid), xt.world.gametype()))
  local items = xt.ents.find{class = "item_health_mega"}
  local near = xt.ents.find{near = o, radius = 4096, limit = 5}
  print("probe_sv: find " .. #items .. " mega, " .. #near .. " near")
  print("probe_sv: player " .. tostring(p) .. " name=" .. p.name .. " health=" .. p.health .. " bot=" .. tostring(p.bot))
end, "world")

-- the hottest server hooks, with trivial handlers, for sv_lua_hooks timing
local calls = 0
xt.hook("PlayerPreThink", function(h) calls = calls + 1 end)
xt.hook("Damage_Calculate", function(h) calls = calls + 1 end)
xt.hook("PlayerPhysics", function(h) calls = calls + 1 end)
print("probe_sv: loaded")
