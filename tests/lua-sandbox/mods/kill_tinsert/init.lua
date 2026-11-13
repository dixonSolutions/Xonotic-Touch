-- table.insert at the front of a table whose __len lies: the loop that moves
-- the elements up runs in C, and rawlen as __index/__newindex runs no Lua code
local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end, __index = rawlen, __newindex = rawlen})
table.insert(t, 1, "x")
print("SANDBOX FAIL kill_tinsert returned")
