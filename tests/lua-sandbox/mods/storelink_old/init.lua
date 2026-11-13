-- data -> ../.permissions (the old grant folder, made by the test): with only
-- storage this used to rewrite another mod's grants
local ok, err = pcall(xt.save, "scoped", "v1 7ff")
print("STORE old " .. (ok and "FAIL saved through the symlink" or "PASS refused: " .. tostring(err)))
