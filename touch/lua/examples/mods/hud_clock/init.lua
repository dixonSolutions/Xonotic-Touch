-- HUD clock: a small panel with the time, frames per second and your health.
-- Place: lua_hud_clock_corner 4 a free spot on the left edge (default, clear of
-- the touch controls, chat, scores and the console button: xt.freespot),
-- 0 top left, 1 top right, 2 bottom left, 3 bottom right.
local fmt = require("format")   -- format.lua in this folder

local shown = true
local fps, frames, acc = 0, 0, 0

if not xt.cvar("lua_hud_clock_corner") then
  xt.setcvar("lua_hud_clock_corner", "4")   -- the mod's own cvar, saved in config.cfg (with storage)
end

xt.on("frame", function(dt)
  frames, acc = frames + 1, acc + dt
  if acc >= 0.5 then
    fps, frames, acc = math.floor(frames / acc + 0.5), 0, 0
  end
end)

xt.on("hud", function(w, h)
  if not shown then return end
  local me = xt.me()
  local lines = { fmt.clock(xt.realtime()), fps .. " fps" }
  if me and me.alive then lines[#lines + 1] = "health " .. me.health end
  local size, pad = 12, 6
  local width = 0
  for _, s in ipairs(lines) do width = math.max(width, xt.draw.textwidth(s, size)) end
  local pw, ph = width + pad * 2, #lines * (size + 2) + pad * 2
  local corner = tonumber(xt.cvar("lua_hud_clock_corner")) or 4
  local x, y
  if corner >= 4 then
    x, y = xt.freespot(pw, ph)
  else
    x = (corner % 2 == 1) and (w - pw - 8) or 8
    y = (corner >= 2) and (h - ph - 8) or 8
  end
  xt.draw.fill(x, y, pw, ph, 0, 0, 0, 0.45)
  for i, s in ipairs(lines) do
    xt.draw.text(x + pad, y + pad + (i - 1) * (size + 2), s, size, 1, 1, 1, 0.9)
  end
end)

xt.command("toggle", function()
  shown = not shown
  print(shown and "shown" or "hidden")
end, "show or hide the HUD clock")

print("HUD clock ready (lua_hud_clock_toggle)")
