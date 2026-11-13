-- data/ is real; data/link.txt is a symlink and data/hard.txt a hard link to
-- enabled.txt, evil.lua a symlink out of the folder (all made by the test)
local function line(label, ok) print("STORE " .. label .. " " .. (ok and "PASS" or "FAIL")) end
line("symlinked file save refused", not pcall(xt.save, "link", "x"))
line("symlinked file load is nil", xt.load("link") == nil)
line("hard-linked file save refused", not pcall(xt.save, "hard", "x"))
line("hard-linked file load is nil", xt.load("hard") == nil)
line("symlinked module refused", not pcall(require, "evil"))
line("plain save works", pcall(xt.save, "plain", "ok") and xt.load("plain") == "ok")
