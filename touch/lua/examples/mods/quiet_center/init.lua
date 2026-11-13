-- quiet_center: filtering what the server shows.
--
-- "centerprint" is every centred message the server sends (on any server);
-- "notification" with kind "center" is one of the game's own centred
-- messages (this game's client code). Returning false hides either.

local hidden = 0
local function matches(text)
  local words = xt.setting("words")
  return words ~= "" and text:lower():find(words:lower(), 1, true) ~= nil
end

xt.on("centerprint", function(text)
  if matches(text) then
    hidden = hidden + 1
    print("quiet_center: hid centerprint (" .. hidden .. ")")
    return false
  end
end)

xt.on("notification", function(kind, name, text)
  if kind == "center" and (matches(text) or matches(name)) then
    hidden = hidden + 1
    print("quiet_center: hid " .. name .. " (" .. hidden .. ")")
    return false
  end
end)

xt.command("count", function() print("quiet_center: " .. hidden .. " hidden") end, "how many messages were hidden")
