-- API 3 tiers: the old scopes are the standard tier (no grant needed);
-- engine_full is a switch "grant all" never turns on
local function try(label, fn)
    local ok, err = pcall(fn)
    print("TIERS " .. label .. " " .. (ok and "allowed" or (tostring(err):find("permission denied", 1, true) and "denied" or ("error " .. tostring(err)))))
end
try("exec", function() xt.exec("echo TIERS exec ran") end)
try("storage", function() xt.save("test", "x") end)
try("cvar_write", function() xt.setcvar("fov", "90") end)
try("engine", function() return xt.engine.cvar("fov") end)
xt.command("probe", function()
    local unsafe = pcall(function() xt.exec("quit") end)
    print("TIERS allowlist " .. (unsafe and "FAIL" or "PASS"))
    try("probe engine", function() return xt.engine.cvar("fov") end)
    print("TIERS private " .. tostring(pcall(xt.engine.cvar, "rcon_password") and xt.engine.cvar("rcon_password")))
end)
