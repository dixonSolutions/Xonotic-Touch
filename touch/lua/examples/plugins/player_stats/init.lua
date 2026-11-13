-- player_stats: persistent storage.
--
-- xt.data.save / xt.data.load keep a table as JSON in the mod's own data/
-- folder (64 KB per key). Saving on "map" and "unload" keeps the numbers
-- across map changes and restarts.

local stats = xt.data.load("stats", {})
local function entry(id)
  local p = xt.player(id)
  if not p then return nil end
  local key = p.name
  stats[key] = stats[key] or {kills = 0, deaths = 0, maps = 0}
  return stats[key]
end

xt.on("death", function(victim, attacker, deathtype)
  local v = entry(victim)
  if v then v.deaths = v.deaths + 1 end
  if attacker ~= 0 and attacker ~= victim then
    local a = entry(attacker)
    if a then a.kills = a.kills + 1 end
  end
end)

local function save(why)
  local n = 0
  for _ in pairs(stats) do n = n + 1 end
  xt.data.save("stats", stats)
  print("player_stats: saved " .. n .. " players (" .. why .. ")")
end

xt.on("map", function()
  for _, id in ipairs(xt.players()) do
    local e = entry(id)
    if e then e.maps = e.maps + 1 end
  end
  save("map")
end)
xt.on("unload", function() save("unload") end)

xt.chatcommand("stats", function(player)
  local e = entry(player)
  xt.tell(player, string.format("%d kills, %d deaths, %d maps", e.kills, e.deaths, e.maps))
end, "your kills and deaths across maps")

local n = 0
for _ in pairs(stats) do n = n + 1 end
print("player_stats: loaded " .. n .. " players")
