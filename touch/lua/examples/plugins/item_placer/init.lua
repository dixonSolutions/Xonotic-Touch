-- item_placer: the entity API.
--
-- xt.ents.spawn creates an item or a marker the way the map would (only safe
-- classes); the mod's entities go away when it stops, and h:remove() removes
-- one early. A handle stays checked: once its entity is gone, h:valid() is
-- false. The teleporter is plain Lua: a check twice a second.

local marks = {}     -- the two ends, as entity handles (info_notnull markers)
local spawned = {}

local function infront(p, dist)
  local f = xt.forward(p.angles)
  local o = xt.vec(p.origin)
  return o + xt.vec(f.x, f.y, 0):normalize() * dist + xt.vec(0, 0, 16)
end

xt.chatcommand("drop", function(player)
  local p = xt.ent(player)
  local e = xt.ents.spawn("item_health_mega", {origin = infront(p, 96)})
  if e then
    spawned[#spawned + 1] = e
    print("item_placer: spawned " .. tostring(e))
  else
    print("item_placer: the game did not allow the item here")
  end
end, "put a mega health in front of you")

xt.chatcommand("mark", function(player)
  local p = xt.ent(player)
  local e = xt.ents.spawn("info_notnull", {origin = p.origin})
  table.insert(marks, e)
  if #marks > 2 then
    local old = table.remove(marks, 1)
    if old:valid() then old:remove() end
  end
  xt.tell(player, "mark " .. #marks .. " set")
  print("item_placer: mark " .. #marks .. " at " .. tostring(xt.vec(p.origin)))
end, "mark one end of the teleporter")

xt.chatcommand("portal", function(player)
  -- teleports the player to the other mark from the one nearest to them
  if #marks < 2 or not marks[1]:valid() or not marks[2]:valid() then
    xt.tell(player, "set two marks first (!mark)")
    return
  end
  local p = xt.ent(player)
  local a, b = xt.vec(marks[1].origin), xt.vec(marks[2].origin)
  local here = xt.vec(p.origin)
  local to = here:dist(a) < here:dist(b) and b or a
  p:teleport(to + xt.vec(0, 0, 8))
  xt.world.effect("TE_TELEPORT", to)
  print("item_placer: teleported player " .. player)
end, "jump to the other mark")

xt.chatcommand("clear", function(player)
  local n = 0
  for _, e in ipairs(spawned) do
    if e:valid() then e:remove() n = n + 1 end
  end
  spawned = {}
  print("item_placer: removed " .. n)
end, "remove the items you dropped")
