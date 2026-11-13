-- panel_clock: a HUD panel with a settings page.
--
-- xt.hud.panel asks the engine for a panel: it finds a place clear of the
-- touch controls and the stock HUD, draws it in the HUD pass and lets the
-- player move it (cvars lua_panel_clock_clockx / _clocky). The manifest's
-- setting.* lines become a settings page by themselves (Settings > Mods);
-- xt.settings.page adds one of our own with a button.

local function hex(c)
  return tonumber(c:sub(1, 2), 16) / 255, tonumber(c:sub(3, 4), 16) / 255, tonumber(c:sub(5, 6), 16) / 255
end

local drawn = false
xt.hud.panel{
  id = "clock", title = "Clock", w = 120, h = 34, side = "left",
  draw = function(x, y, w, h)
    local size = tonumber(xt.setting("size")) or 16
    local fmt = xt.setting("seconds") == "1" and "%H:%M:%S" or "%H:%M"
    local r, g, b = hex(xt.setting("color"))
    xt.draw.gradient(x, y, w, h, {0, 0, 0, 0.55}, {0.1, 0.1, 0.15, 0.25}, true)
    local text = xt.time.date(fmt)
    local tw = xt.draw.textsize(text, size)
    xt.draw.text(x + (w - tw) / 2, y + (h - size) / 2, text, size, r, g, b, 1)
    if not drawn then
      drawn = true
      print("panel_clock: drawn at " .. math.floor(x) .. "," .. math.floor(y))
    end
  end,
}

local page = xt.settings.page{
  id = "main", title = "Clock panel",
  rows = {
    {type = "section", text = "Clock"},
    {type = "switch", id = "seconds", label = "Show seconds", setting = "seconds"},
    {type = "slider", id = "size", label = "Text size", setting = "size"},
    {type = "button", id = "reset", label = "Reset to defaults",
      on_press = function()
        xt.setcvar("lua_panel_clock_seconds", "1")
        xt.setcvar("lua_panel_clock_size", "16")
        print("panel_clock: reset pressed")
      end},
    {type = "label", id = "where", text = "Move the panel in the HUD editor."},
  },
  on_change = function(row, value, event)
    print("panel_clock: " .. row .. " " .. event .. " " .. tostring(value))
  end,
}
