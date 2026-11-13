-- an error inside a hot hook: the plugin stops, the match goes on
local n = 0
xt.hook("PlayerPhysics", function(h)
  n = n + 1
  if n > 200 then error("HOOKERROR on purpose") end
end)
