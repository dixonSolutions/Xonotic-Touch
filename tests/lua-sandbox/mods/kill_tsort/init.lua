-- table.sort of 2^31 - 2 equal elements that do not exist and are never stored:
-- quicksort in C, no Lua code, no memory
local t = setmetatable({}, {__len = function() return 2147483646 end, __index = rawlen, __newindex = rawlen})
table.sort(t)
print("SANDBOX FAIL kill_tsort returned")
