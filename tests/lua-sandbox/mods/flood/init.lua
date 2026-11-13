-- own cvars: capped per mod, and not saved in config.cfg without storage
local n = 0
for i = 1, 100 do
  if pcall(xt.setcvar, "lua_flood_v" .. i, "1") then n = n + 1 end
end
print("FLOOD created " .. n)
