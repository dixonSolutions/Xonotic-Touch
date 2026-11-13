-- data -> ../../luastate/mods/grants (made by the test): a mod with storage
-- tries to rewrite its own grant file
local ok, err = pcall(xt.save, "storelink_new", "legacy 7ff")
print("STORE new " .. (ok and "FAIL saved through the symlink" or "PASS refused: " .. tostring(err)))
