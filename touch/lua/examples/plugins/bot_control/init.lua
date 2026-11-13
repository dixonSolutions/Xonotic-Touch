-- bot_control: bots.

local function humans()
  local n = 0
  for _, id in ipairs(xt.players()) do
    if not xt.ent(id).bot then n = n + 1 end
  end
  return n
end

xt.chatcommand("bots", function(player, args)
  local want = tonumber(args)
  if not want then
    xt.tell(player, "usage: !bots <number>")
    return
  end
  local have = #xt.players() - humans()
  local n
  if want > have then n = xt.bots.add(want - have) else n = xt.bots.remove(have - want) end
  print("bot_control: bot_number now " .. n)
end, "set the number of bots")

xt.chatcommand("skill", function(player, args)
  local s = tonumber(args)
  if s then xt.bots.skill(s) end
  xt.tell(player, "bot skill " .. xt.bots.skill())
  print("bot_control: skill " .. xt.bots.skill())
end, "read or set the bots' skill")

xt.on("map", function()
  local fill = tonumber(xt.setting("fill")) or 0
  if fill > 0 and #xt.players() < fill then
    print("bot_control: filling to " .. fill)
  end
end)
