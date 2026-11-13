-- an endless loop in an event: stopped by the time limit, the match goes on
local n = 0
xt.on("frame", function()
  n = n + 1
  if n == 30 then
    print("bad_loop: looping now")
    while true do end
  end
end)
