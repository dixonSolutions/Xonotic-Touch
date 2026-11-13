-- Chat keys: F5 and F6 send short chat lines. Returning true from the key event
-- keeps the key from its normal binding.
local lines = { F5 = "gg", F6 = "nice shot" }

xt.on("key", function(key, down)
  local line = lines[key]
  if not line then return false end
  if down and xt.ingame() then
    local ok, err = pcall(xt.exec, "say " .. line)
    if not ok then print(err) end
  end
  return true
end)
