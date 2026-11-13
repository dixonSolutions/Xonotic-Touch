-- an endless loop that keeps catching its own death with pcall
while true do pcall(function() while true do end end) end
