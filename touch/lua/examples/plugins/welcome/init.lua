-- Welcome: greetings, killing sprees, a "cmd stats" reply and a chat filter.
local streak, kills = {}, {}

local function name(id)
  local p = xt.player(id)
  return p and p.name or "someone"
end

xt.on("map", function(map)
  streak, kills = {}, {}
  print("welcome: new map " .. map)
end)

xt.on("connect", function(id)
  xt.tell(id, "^2Welcome to " .. xt.map() .. ", " .. name(id) .. "^7! Type ^3cmd stats^7 for your kills.")
end)

xt.on("spawn", function(id)
  streak[id] = 0
end)

xt.on("death", function(victim, attacker, deathtype)
  streak[victim] = 0
  if attacker ~= 0 and attacker ~= victim then
    streak[attacker] = (streak[attacker] or 0) + 1
    kills[attacker] = (kills[attacker] or 0) + 1
    if streak[attacker] == 3 then
      xt.broadcast("^3" .. name(attacker) .. "^7 is on a killing spree!")
    end
  end
  print(string.format("welcome: %s killed %s (%s)", name(attacker), name(victim), deathtype))
end)

xt.on("chat", function(id, msg, teamsay)
  if msg:lower():find("badword", 1, true) then
    xt.tell(id, "^1That word is not allowed here.")
    return false   -- the message is not sent
  end
end)

xt.on("clientcmd", function(id, cmd, args)
  if cmd == "stats" then
    xt.tell(id, string.format("You have %d kills this map.", kills[id] or 0))
    return true    -- handled: the game does not answer "unknown command"
  end
end)
