-- table.move loops in C over 2^63 elements that do not exist; the instruction
-- hook never fires there (the engine used to hang, and hang again at every start)
table.move({}, 1, math.maxinteger, 1, {})
print("SANDBOX FAIL kill_tmove returned")
