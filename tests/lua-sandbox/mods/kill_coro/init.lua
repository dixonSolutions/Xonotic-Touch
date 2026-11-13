-- coroutine.resume catches errors like pcall
while true do
  local co = coroutine.create(function() while true do end end)
  coroutine.resume(co)
  coroutine.close(co)
end
