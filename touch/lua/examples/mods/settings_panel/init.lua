-- Settings panel: a settings page written as a mod, with the xt.ui widgets.
-- lua_settings_panel_open shows it over the game; tap or click DONE to close.
local ui = require("xt.ui")

local open = false
local state = { size = tonumber(xt.cvar("crosshair_size")) or 0.5, clock = true }

-- restore what was saved last time ("key=value" lines in data/state.txt)
local saved = xt.load("state")
if saved then
  for k, v in saved:gmatch("(%w+)=([%w%.]+)") do
    if k == "size" then state.size = tonumber(v) or state.size end
    if k == "clock" then state.clock = v == "1" end
  end
end

local function save()
  xt.save("state", string.format("size=%.2f\nclock=%s\n", state.size, state.clock and "1" or "0"))
end

xt.command("open", function() open = true end, "show the settings panel")

xt.on("draw", function(w, h)
  if not open then return end
  ui.begin()
  local pw, ph = math.min(420, w - 40), 230
  local x, y, iw = ui.panel((w - pw) / 2, (h - ph) / 2, pw, ph, "Settings (Lua mod)")
  local col = ui.column(x, y, iw)
  local rx, ry, rw, rh = col:row(32)
  local size = ui.slider(rx, ry, rw, rh, "Crosshair size", state.size, 0.2, 1.5)
  if math.abs(size - state.size) > 0.001 then
    state.size = size
    xt.setcvar("crosshair_size", string.format("%.2f", size))
  end
  rx, ry, rw, rh = col:row(32)
  local clock = ui.toggle(rx, ry, rw, rh, "HUD clock mod", state.clock)
  if clock ~= state.clock then
    state.clock = clock
    xt.exec("echo use lua_mod_enable / lua_mod_disable hud_clock to switch the clock")
  end
  local bx, by, bw, bh = col:row(36)
  if ui.button(bx, by, bw, bh, "DONE") then
    open = false
    save()
  end
end)
