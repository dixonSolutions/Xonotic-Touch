-- Health and armor bars: a HUD widget with settings.
--
-- The three settings are declared in mod.txt (setting.<name>: ...). The engine
-- creates them as cvars (lua_hud_widget_side, lua_hud_widget_width,
-- lua_hud_widget_numbers) with their defaults and shows them in the Mods tab's
-- Settings page; the mod only reads them, so it needs no cvar_read or
-- cvar_write permission: xt.setting(name) reads its own declared settings.

local function setting(name, default)
  local v = xt.setting(name)
  if v == nil or v == "" then return default end
  return v
end

local function bar(x, y, w, h, frac, r, g, b, label, showNumbers)
  xt.draw.fill(x, y, w, h, 0, 0, 0, 0.45)                       -- background
  xt.draw.fill(x + 2, y + 2, (w - 4) * math.max(0, math.min(1, frac)), h - 4, r, g, b, 0.85)
  if showNumbers then
    xt.draw.text(x + 6, y + (h - 12) / 2, label, 12, 1, 1, 1, 1)
  end
end

xt.on("hud", function(w, h)
  local me = xt.me()
  if not me or not me.alive then return end
  local width = tonumber(setting("width", "160")) or 160
  local numbers = setting("numbers", "1") == "1"
  local bh, gap = 18, 4
  -- xt.freespot gives a place clear of chat, the scores, the console button
  -- and the touch sticks and buttons (xt.reserved lists those areas)
  -- Asked with a margin all round and drawn inside it, so the bars keep
  -- clear of whatever is next to the spot (the stock ammo number).
  local m = 10
  local x, y = xt.freespot(width + 2 * m, bh * 2 + gap + 2 * m, setting("side", "left"))
  x, y = x + m, y + m
  bar(x, y, width, bh, me.health / 100, 0.9, 0.25, 0.25, "health " .. me.health, numbers)
  bar(x, y + bh + gap, width, bh, me.armor / 100, 0.3, 0.6, 1.0, "armor " .. me.armor, numbers)
end)
