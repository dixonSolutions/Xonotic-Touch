-- Underdog: whoever has the fewest kills this map takes less damage.
if not xt.cvar("sv_lua_underdog_scale") then
  xt.setcvar("sv_lua_underdog_scale", "0.75")
end

local kills = {}

xt.on("map", function() kills = {} end)
xt.on("disconnect", function(id) kills[id] = nil end)
xt.on("death", function(victim, attacker)
  if attacker ~= 0 and attacker ~= victim then kills[attacker] = (kills[attacker] or 0) + 1 end
end)

local function lowest()
  local low, lowkills = nil, math.huge
  for _, id in ipairs(xt.players()) do
    local p = xt.player(id)
    if p and not p.spectator and (kills[id] or 0) < lowkills then low, lowkills = id, kills[id] or 0 end
  end
  return low
end

local told = false
xt.on("damage", function(target, attacker, damage, deathtype)
  if target == lowest() and attacker ~= target then
    local scaled = damage * (tonumber(xt.cvar("sv_lua_underdog_scale")) or 0.75)
    if not told then
      print(string.format("underdog: %s took %.0f instead of %.0f (%s)", xt.player(target).name, scaled, damage, deathtype))
      told = true
    end
    return scaled
  end
end)
