xt.command("plain", function() print("REMOTE FAIL plain ran") end)
xt.command("open", function() print("REMOTE open ran") end, "a command servers may run", {fromserver = true})
xt.command("local", function() print("REMOTE plain-local ran") end)
