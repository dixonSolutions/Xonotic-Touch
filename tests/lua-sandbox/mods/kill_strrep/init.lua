-- one allocation over the memory limit in a single C call (100 MB)
while true do pcall(string.rep, "x", 1e8) end
