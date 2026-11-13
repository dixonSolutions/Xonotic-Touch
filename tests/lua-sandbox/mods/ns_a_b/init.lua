xt.command("c", function() print("NS ns_a_b command ran") end)
print("NS " .. (pcall(xt.setcvar, "lua_ns_a_b_d", "1") and "PASS ns_a_b set its cvar" or "FAIL ns_a_b could not set its cvar"))
