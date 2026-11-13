-- grow memory without end, catching every failure
local t = {}
while true do pcall(function() for i = 1, 1e7 do t[#t + 1] = string.rep("x", 1000) .. i end end) end
