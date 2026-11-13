-- a hot client hook that never returns: stopped by lua_hook_ms, the HUD goes on
local n = 0
xt.hook("HUD_Draw_overlay", function(h)
  n = n + 1
  if n == 60 then
    print("bad_clhook: looping now")
    while true do end
  end
end)
