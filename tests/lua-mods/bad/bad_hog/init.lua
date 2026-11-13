-- eats memory a little every frame until the memory limit stops it
local keep = {}
xt.on("frame", function()
  for i = 1, 2000 do keep[#keep + 1] = string.rep("x", 1000) .. #keep end
end)
print("bad_hog: started")
