-- catastrophic backtracking inside one C call, where the instruction hook cannot fire
local s = string.rep("a", 1000000)
while true do pcall(string.find, s, ".-.-.-.-.-.-.-.-b") end
