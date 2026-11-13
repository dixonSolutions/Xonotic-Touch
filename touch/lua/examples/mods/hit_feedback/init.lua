-- hit_feedback: client events and drawing.
--
-- "damage" and "kill" come from the engine and the game's notifications;
-- "hit" (damage you dealt) needs this game's own client code (xt.csqc()).
-- Returning false from "notification" hides it: here the stock death
-- messages, which the mod draws itself as a compact feed.

local numbers = {}  -- floating damage numbers {text, t}
local feed = {}     -- recent kills {line, t}

xt.on("damage", function(amount, health, armor)
  numbers[#numbers + 1] = {text = "-" .. math.floor(amount), t = xt.time.now()}
  print("hit_feedback: took " .. math.floor(amount))
end)

xt.on("hit", function(amount)
  xt.sound("sound/misc/hit.wav")
  numbers[#numbers + 1] = {text = "+" .. math.floor(amount), t = xt.time.now(), mine = true}
  print("hit_feedback: hit " .. math.floor(amount))
end)

xt.on("kill", function(victim, attacker, name)
  local line = (attacker ~= "" and attacker or "world") .. " > " .. victim
  table.insert(feed, 1, {line = line, t = xt.time.now()})
  if #feed > 5 then feed[6] = nil end
  print("hit_feedback: kill " .. name .. " " .. line)
end)

xt.on("notification", function(kind, name, text, args)
  if xt.setting("feed") == "1" and kind == "info" and (name:find("DEATH_", 1, true) or name:find("_MURDER", 1, true)) then
    return false -- drawn by us instead
  end
end)

xt.on("hud", function(w, h)
  local now = xt.time.now()
  local keep = {}
  for i, n in ipairs(numbers) do
    local age = now - n.t
    if age < 1 then
      local r, g = n.mine and 0.4 or 1, n.mine and 1 or 0.3
      xt.draw.text(w / 2 + 24, h / 2 - 20 - age * 40 - (i - 1) * 4, n.text, 14, r, g, 0.3, 1 - age)
      keep[#keep + 1] = n
    end
  end
  numbers = keep
  if xt.setting("feed") == "1" then
    local x, y = xt.freespot(240, 100, "right")
    for i, k in ipairs(feed) do
      local a = math.max(0, 1 - (now - k.t) / 6)
      if a > 0 then
        xt.draw.fill(x, y + (i - 1) * 18, 240, 16, 0, 0, 0, 0.4 * a)
        xt.draw.text(x + 4, y + (i - 1) * 18 + 2, k.line, 12, 1, 1, 1, a)
      end
    end
  end
end)
