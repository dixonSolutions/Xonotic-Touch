-- finalizers run with hooks off; they are refused outright
setmetatable({}, {__gc = function() while true do end end})
print("SANDBOX FAIL kill_gc reached the end")
