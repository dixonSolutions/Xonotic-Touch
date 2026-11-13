-- radar: entities around the player, and the offline-only rule.
--
-- xt.entities lists players, projectiles and items. In an offline match
-- against bots (xt.offline() is true) it lists everyone, so a radar is a
-- training tool there. On other people's servers it lists only players and
-- projectiles that are on your screen and not behind a wall: what you see
-- anyway. The engine enforces this, whatever the mod does.

local said
local size = 120

xt.hud.panel{
  id = "radar", title = "Radar", w = size, h = size, side = "right",
  draw = function(x, y, w, h)
    local view = xt.view()
    local me = view.origin
    local yaw = math.rad(view.angles.y)
    local cx, cy = x + w / 2, y + h / 2
    xt.draw.circle(cx, cy, w / 2 - 2, 0, 0, 0, 0.45, true)
    xt.draw.circle(cx, cy, w / 2 - 2, 0.5, 0.8, 1, 0.6)
    local list = xt.entities("player")
    for _, e in ipairs(list) do
      local dx, dy = e.x - me.x, e.y - me.y
      -- rotate into view space: up on the radar is where you look
      local fx = dx * math.cos(-yaw) - dy * math.sin(-yaw)
      local fy = dx * math.sin(-yaw) + dy * math.cos(-yaw)
      local px, py = cx - fy / 2000 * (w / 2), cy - fx / 2000 * (h / 2)
      if (px - cx) ^ 2 + (py - cy) ^ 2 < (w / 2) ^ 2 then
        xt.draw.fill(px - 2, py - 2, 4, 4, 1, e.visible and 1 or 0.3, 0.3, 1)
      end
    end
    local offline, why = xt.offline()
    local key = tostring(offline) .. #list
    if said ~= key then
      said = key
      print("radar: offline=" .. tostring(offline) .. " (" .. why .. "), " .. #list .. " players listed")
    end
  end,
}
