-- no mod.txt: a legacy mod. It gets the HUD set until the user grants more.
local function try(label, f)
  local ok, err = pcall(f)
  local what = ok and "allowed" or (tostring(err):find("permission denied", 1, true) and "denied" or ("error " .. tostring(err)))
  print("LEGACY " .. label .. " " .. what)
end
try("exec", function() xt.exec("echo LEGACY exec ran") end)
try("storage", function() xt.save("k", "v") end)
try("cvar_write", function() xt.setcvar("crosshair_size", "0.5") end)
try("cvar_read", function() return xt.cvar("fov") end)
try("state", function() return xt.realtime() end)
try("display", function() return xt.screen() end)
try("events", function() xt.on("frame", function() end) end)
