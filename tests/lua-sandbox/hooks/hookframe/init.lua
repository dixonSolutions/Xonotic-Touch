-- each call under lua_hook_ms, but the calls of one frame add up past
-- lua_hook_frame_ms: the frame budget stops it
xt.hook("PlayerPhysics", function(h)
  local x = 0
  for i = 1, 150000 do x = x + i end
end)
