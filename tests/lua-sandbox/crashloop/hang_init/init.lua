-- loads for as long as the limits let it (the time limit, 1 s for loading:
-- each pass copies 100 KB, so the instruction limit comes much later); the
-- test kills the engine meanwhile
print("CRASH hang_init loading")
while true do local s = string.rep("x", 100000) end
