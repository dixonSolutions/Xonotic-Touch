-- round_hud: the client half of round_timer. It listens on the channel the
-- server mod sends on, and says hello once the server is ready for it.

local ch = xt.channel("round_timer")
local state, said = nil, false

ch:on(function(msg)
  if type(msg) ~= "table" then return end
  if msg.welcome or not state then
    print("round_hud: round " .. tostring(msg.round) .. ", " .. tostring(msg.left) .. " s left")
  end
  state = msg
end)

xt.on("frame", function()
  -- ready() is true once the server said it speaks mod channels
  if not said and ch:ready() then
    said = ch:send({hello = "round_hud 1.0"})
  end
end)

xt.on("disconnect", function() state, said = nil, false end)

xt.on("hud", function(w, h)
  if not state then return end
  local text = "Round " .. state.round .. "  " .. state.left .. " s"
  local tw = xt.draw.textsize(text, 16)
  xt.draw.fill(w / 2 - tw / 2 - 8, h * 0.11, tw + 16, 22, 0, 0, 0, 0.5)
  xt.draw.text(w / 2 - tw / 2, h * 0.11 + 3, text, 16, 1, 0.85, 0.3, 1)
end)
