-- ticker: work that keeps going. A literal endless loop would be stopped (the
-- game must keep drawing frames); a loop that sleeps is fine.

local ticks = 0
xt.spawn(function()
  while true do
    ticks = ticks + 1
    if ticks % 5 == 0 then print("ticker: loop " .. ticks) end
    xt.sleep(1)
  end
end)

local every = 0
local id
id = xt.every(2, function()
  every = every + 1
  print("ticker: every " .. every)
  if every == 3 then return false end -- stops itself
end)

xt.after(4, function() print("ticker: after 4 s") end)
