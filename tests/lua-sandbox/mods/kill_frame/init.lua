-- loads fine, then loops in its frame event
print("SANDBOX kill_frame loaded")
xt.on("frame", function() local n = 0; while true do n = n + 1 end end)
