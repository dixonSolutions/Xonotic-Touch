-- an error inside a hook handler: the mod stops, the hook and the game go on
xt.hook("PlayerPreThink", function(h)
  print("bad_hook: erroring now")
  local t = nil
  return t.field
end)
