-- Damage scale: a write-capable hook (docs/LUA_HOOKS.md, Damage_Calculate).
--
-- h.damage is an in/out argument of the hook: changing it changes the hit,
-- but only while the hooks_write permission is granted. Without it the
-- plugin still runs and sees every hit; its changes are ignored.

local changed, told = 0, {}

xt.hook("Damage_Calculate", function(h)
  local factor = tonumber(xt.setting("factor")) or 1
  if h.target == h.attacker and xt.setting("selfdamage") ~= "1" then return end
  if h.damage <= 0 or factor == 1 then return end
  h.damage = h.damage * factor
  changed = changed + 1
  -- h.attacker is an entity number: a player's is their player id
  local who = xt.entity(h.attacker)
  if who and who.player and not told[h.attacker] then
    told[h.attacker] = true
    xt.tell(h.attacker, string.format("Damage is scaled by %.2f on this server", factor))
  end
end)

xt.every(30, function()
  if changed > 0 then print("damage_scale: scaled " .. changed .. " hits so far") end
end)
