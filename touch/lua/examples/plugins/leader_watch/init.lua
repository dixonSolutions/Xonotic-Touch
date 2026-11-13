-- Leader watch: how a plugin does continuous work.
--
-- A literal "while true do ... end" in a handler would hit the instruction or
-- time limit and stop the plugin: every call into Lua has a budget. Instead,
-- xt.spawn runs a function as a coroutine and xt.sleep(seconds) pauses it;
-- each step until the next sleep is one call with its own budget, and the
-- game keeps running in between. xt.every / xt.after are the timer form.

local leader, dirty = nil, false

xt.on("death", function() dirty = true end)
xt.on("map", function() leader, dirty = nil, false end)

local function currentLeader()
  local best, bestFrags = nil, -math.huge
  for _, id in ipairs(xt.players()) do
    local p = xt.player(id)
    if p and not p.spectator and p.frags > bestFrags then best, bestFrags = p, p.frags end
  end
  return best
end

-- the long-running task: checks twice a second, forever
xt.spawn(function()
  while true do
    if dirty then
      dirty = false
      xt.sleep(3)  -- let a flurry of kills settle first
      local p = currentLeader()
      if p and p.id ~= leader then
        leader = p.id
        xt.broadcast(string.format("^3%s^7 takes the lead with %d frags", p.name, p.frags))
        print("leader_watch: new leader " .. p.name)
      end
    end
    xt.sleep(0.5)
  end
end)

-- and a timer
xt.every(60, function()
  print(string.format("leader_watch: %.0f s into the match", xt.time()))
end)
