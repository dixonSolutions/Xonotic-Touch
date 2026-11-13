-- mod_menu: a menu page and a settings entry.
--
-- xt.menu.page describes a page of rows; the engine checks it, keeps it, and
-- hands it as JSON to whichever menu draws it (the QuakeC menu, the Android
-- or the Linux app). Changes and button presses come back to the callbacks;
-- page:set changes a row and the page is drawn again. The manifest's
-- setting.* lines are this mod's entry under Settings > Mods.

local notes = xt.data.load("notes", {})
local draft = ""

local function items()
  local t = {}
  for i, n in ipairs(notes) do t[i] = {id = "n" .. i, label = n} end
  return t
end

local page
page = xt.menu.page{
  id = "notes", title = xt.setting("title"),
  rows = {
    {type = "field", id = "draft", label = "New note", placeholder = "type here",
      on_change = function(v) draft = v end},
    {type = "button", id = "add", label = "Add",
      on_press = function()
        if draft == "" then return end
        table.insert(notes, 1, draft)
        local max = tonumber(xt.setting("max")) or 5
        while #notes > max do table.remove(notes) end
        xt.data.save("notes", notes)
        print("mod_menu: added '" .. draft .. "', " .. #notes .. " notes")
        page:set("list", "items", items())
        page:set("fill", "value", #notes / max)
      end},
    {type = "list", id = "list", label = "Notes", items = items(),
      on_select = function(id) print("mod_menu: selected " .. id) end},
    {type = "progress", id = "fill", label = "Space used", value = #notes / 5},
    {type = "button", id = "clear", label = "Clear", style = "destructive",
      on_press = function()
        notes = {}
        xt.data.save("notes", notes)
        page:set("list", "items", {})
        page:set("fill", "value", 0)
        print("mod_menu: cleared")
      end},
  },
}
print("mod_menu: page registered with " .. #notes .. " notes")
