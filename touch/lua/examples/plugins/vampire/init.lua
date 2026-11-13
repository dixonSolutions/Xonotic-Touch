-- vampire: a game rule with a write hook and entity handles.
--
-- Damage_Calculate is the game's own hook (docs/LUA_HOOKS.md); with the Game
-- rules tier, changing h.damage changes the damage. xt.ent gives a live
-- handle to a player: reading h.health reads the game's field; writing it
-- (Game rules) heals.

local healed = 0

xt.hook("Damage_Calculate", function(h)
  if h.target == h.attacker then
    h.damage = h.damage * 0.5 -- your own rockets hurt less
    return
  end
  local attacker = xt.ent(h.attacker)
  if not attacker or not attacker:is_player() or h.damage <= 0 then return end
  local share = tonumber(xt.setting("share")) or 0.5
  local gain = math.floor(h.damage * share)
  if gain > 0 and attacker.alive then
    attacker.health = math.min(attacker.health + gain, 200)
    healed = healed + gain
  end
end)

xt.every(10, function()
  if healed > 0 then
    print("vampire: healed " .. healed .. " in the last 10 s")
    healed = 0
  end
end)
