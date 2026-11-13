-- a to-be-closed variable whose __close loops forever, run while the mod is being stopped
local function f()
  local x <close> = setmetatable({}, {__close = function() while true do end end})
  while true do end
end
while true do pcall(f) end
