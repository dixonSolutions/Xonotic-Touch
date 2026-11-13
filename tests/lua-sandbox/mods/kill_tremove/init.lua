-- table.remove from the front: the same loop downwards
local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end, __index = rawlen, __newindex = rawlen})
table.remove(t, 1)
print("SANDBOX FAIL kill_tremove returned")
