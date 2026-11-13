-- the message handler runs inside the hook (hooks off): an endless handler must not hang the game
while true do xpcall(function() while true do end end, function() while true do end end) end
