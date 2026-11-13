-- trivial handlers on the hottest server hooks: sv_lua_hooks shows the cost per call
local n = 0
for _, name in ipairs({"PlayerPreThink", "PlayerPhysics", "Damage_Calculate", "GetPressedKeys", "SV_StartFrame", "PM_Physics"}) do
  xt.hook(name, function(h) n = n + 1 end)
end
