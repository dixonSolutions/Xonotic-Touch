-- counts deaths and damage through read-only hooks; keeps reporting while
-- the other test plugins get stopped, to show the match goes on
local deaths, hits = 0, 0
xt.hook("PlayerDies", function(h)
  deaths = deaths + 1
  print(string.format("HOOKS death of %d by %d, deathtype %d, damage %.0f", h.target, h.attacker, h.deathtype, h.damage))
end)
xt.hook("Damage_Calculate", function(h)
  hits = hits + 1
  h.damage = 99999 -- no hooks_write: must be ignored
end)
xt.every(2, function() print(string.format("HOOKS deaths %d hits %d", deaths, hits)) end)
