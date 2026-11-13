-- trivial handlers on the hottest client hooks: lua_hooks shows the cost per call
local n = 0
for _, name in ipairs({"HUD_Draw_overlay", "DrawCrosshair", "PlayerPhysics", "UpdateCrosshair"}) do
  xt.hook(name, function(h) n = n + 1 end)
end
