-- helpers for hud_clock, loaded with require("format")
local M = {}
function M.clock(seconds)
  local s = math.floor(seconds)
  return string.format("%02d:%02d:%02d", s // 3600 % 24, s // 60 % 60, s % 60)
end
return M
