-- round_timer: the server half of a game mode that ships a client HUD.
--
-- xt.channel("round_timer") is the same name on both sides. channel:send(nil,
-- value) goes to every player whose game has a mod listening (round_hud);
-- channel:on gets their requests. Stock clients never see any of it.

local ch = xt.channel("round_timer")
local round, left = 1, 0
local hellos = 0

local function length() return tonumber(xt.setting("length")) or 60 end

xt.on("map", function()
  round, left = 1, length()
end)

-- a coroutine counting the rounds down, once a second
xt.spawn(function()
  left = length()
  while true do
    xt.sleep(1)
    left = left - 1
    if left <= 0 then
      round, left = round + 1, length()
      xt.broadcast("^3Round " .. round .. "!")
    end
    ch:send(nil, {round = round, left = left})
  end
end)

-- a client mod says hello when it starts: answer that player alone
ch:on(function(player, msg)
  if type(msg) == "table" and msg.hello then
    hellos = hellos + 1
    print("round_timer: hello from player " .. player .. " (" .. tostring(msg.hello) .. ")")
    ch:send(player, {round = round, left = left, welcome = true})
  end
end)

-- a page from the server: !round shows the player a small form
xt.chatcommand("round", function(player)
  xt.page.show(player, {
    id = "menu", title = "Round timer",
    rows = {
      {type = "label", id = "info", text = "Round " .. round .. ", " .. left .. " s left"},
      {type = "button", id = "next", label = "Vote: next round now",
        on_press = function(p) print("round_timer: next round asked by player " .. tostring(p)); left = 1 end},
    },
  })
  print("round_timer: page shown to player " .. player)
end, "show the round timer's page")
