-- one hook call far over lua_hook_ms: stopped by the time limit
local n = 0
xt.hook("PlayerPhysics", function(h)
  n = n + 1
  if n > 300 then while true do end end
end)
