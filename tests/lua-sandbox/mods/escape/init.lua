-- Sandbox escape checks. Every line printed is "SANDBOX PASS <check>" or
-- "SANDBOX FAIL <check>"; the run ends with "SANDBOX DONE <passed>/<total>".
local total, passed = 0, 0
local function check(name, ok)
  total = total + 1
  if ok then passed = passed + 1; print("SANDBOX PASS " .. name)
  else print("SANDBOX FAIL " .. name) end
end
local function fails(f, ...) return not pcall(f, ...) end

-- libraries and functions that must not exist
for _, n in ipairs({"io", "os", "debug", "package", "dofile", "loadfile", "warn", "module", "jit", "ffi", "newproxy", "setfenv", "getfenv"}) do
  check("absent " .. n, rawget(_G, n) == nil)
end
check("absent string.dump", string.dump == nil)
check("string metatable has no dump", getmetatable("").__index.dump == nil)

-- load: text only, strings only
local binary = "\27LuaT\0\25\147\r\n\26\n\4\8\8xV\0\0\0\0\0\0\0\0\0\0\0(w@\1"
local f, err = load(binary)
check("binary chunk refused", f == nil and tostring(err):find("binary") ~= nil)
f = load(binary, "x", "b")
check("binary chunk refused with mode b", f == nil)
check("load(function) refused", fails(load, function() return nil end))
check("load text works", load("return 2 + 3")() == 5)
check("load with env", load("return x", "c", "t", {x = 7})() == 7)
check("load chunk cannot see io", load("return io")() == nil)

-- require: only this mod's folder
local h = require("sub.helper")
check("require own file", type(h) == "table" and h.value == 42)
check("require cached", require("sub.helper") == h)
check("require ../ refused", fails(require, "../escape/init"))
check("require /etc/passwd refused", fails(require, "/etc/passwd"))
check("require dotdot refused", fails(require, "sub..helper"))
check("require os refused", fails(require, "os"))
check("require io refused", fails(require, "io"))

-- metatables: no finalizers
check("__gc refused", fails(setmetatable, {}, {__gc = function() end}))
check("setmetatable plain works", getmetatable(setmetatable({}, {__index = {a = 1}})).__index.a == 1)
check("protected metatable kept", fails(setmetatable, setmetatable({}, {__metatable = "no"}), {}))

-- garbage collector cannot be stopped
check("collectgarbage stop refused", fails(collectgarbage, "stop"))
check("collectgarbage generational refused", fails(collectgarbage, "generational"))
check("collectgarbage count", type(collectgarbage("count")) == "number")

-- pattern functions are bounded
check("long pattern refused", fails(string.find, "abc", string.rep("a", 600)))
check("huge subject refused", fails(string.find, string.rep("a", 1100000), "b"))
check("patterns still work", ("hello world"):match("(%w+) world") == "hello")
check("gsub still works", ("a-b-c"):gsub("-", "+") == "a+b+c")

-- cvars: secrets unreadable, writes by allowlist only
check("rcon_password unreadable", xt.cvar("rcon_password") == nil)
check("_cl_password unreadable", xt.cvar("_cl_password") == nil)
check("sv_adminnick-like private unreadable", xt.cvar("crypto_keyfp") == nil)
check("public cvar readable", xt.cvar("vid_conwidth") ~= nil)
check("log_dest_udp not writable", fails(xt.setcvar, "log_dest_udp", "127.0.0.1:26000"))
check("rcon_password not writable", fails(xt.setcvar, "rcon_password", "x"))
check("cl_touch_esp_enabled not writable", fails(xt.setcvar, "cl_touch_esp_enabled", "1"))
check("cl_touch_aim_assist not writable", fails(xt.setcvar, "cl_touch_aim_assist", "1"))
check("sv_cheats not writable", fails(xt.setcvar, "sv_cheats", "1"))
check("cl_lua_enable not writable", fails(xt.setcvar, "cl_lua_enable", "0"))
check("_cl_lua_mods not writable", fails(xt.setcvar, "_cl_lua_mods", "x"))
check("value with ; refused", fails(xt.setcvar, "crosshair_size", "1;quit"))
check("value with $ refused", fails(xt.setcvar, "crosshair_size", "$rcon_password"))
check("value with newline refused", fails(xt.setcvar, "crosshair_size", "1\nquit"))
check("allowlisted cvar writable", pcall(xt.setcvar, "crosshair_size", "0.6") and xt.cvar("crosshair_size") == "0.6")
check("own cvar created", pcall(xt.setcvar, "lua_escape_test", "1") and xt.cvar("lua_escape_test") == "1")
check("other mod's cvar refused", fails(xt.setcvar, "lua_othermod_test", "1"))

-- console commands: allowlist, no separators
check("quit refused", fails(xt.exec, "quit"))
check("; refused", fails(xt.exec, "echo a; quit"))
check("newline refused", fails(xt.exec, "echo a\nquit"))
check("quote refused", fails(xt.exec, 'echo "a" ; quit'))
check("$ refused", fails(xt.exec, "echo $rcon_password"))
check("backslash refused", fails(xt.exec, "echo a\\"))
check("+attack refused", fails(xt.exec, "+attack"))
check("bind refused", fails(xt.exec, "bind x quit"))
check("alias refused", fails(xt.exec, "alias x quit"))
check("exec refused", fails(xt.exec, "exec autoexec.cfg"))
check("rcon refused", fails(xt.exec, "rcon status"))
check("lua_mod_enable refused", fails(xt.exec, "lua_mod_enable kill_loop"))
check("cl_cmd refused", fails(xt.exec, "cl_cmd x"))
check("echo allowed", pcall(xt.exec, "echo SANDBOX exec reached the console"))

-- the API offers no positions, entities, traces, aim or view angles
local allowed = {on=1, command=1, cvar=1, setcvar=1, exec=1, realtime=1, modname=1, realm=1, apiversion=1,
  save=1, load=1, screen=1, ingame=1, islocal=1, map=1, time=1, players=1, me=1, draw=1,
  fingers=1, mouse=1, keydown=1, sound=1}
local extra = {}
for k in pairs(xt) do if not allowed[k] then extra[#extra + 1] = k end end
check("xt has only the documented functions (" .. table.concat(extra, ",") .. ")", #extra == 0)
check("no draw outside the hud event", fails(xt.draw.fill, 0, 0, 1, 1))
check("unknown event refused", fails(xt.on, "think", function() end))
check("command names checked", fails(xt.command, "a;b", function() end))

-- persistence stays in the mod's own data folder
check("save works", pcall(xt.save, "settings", "a=1") and xt.load("settings") == "a=1")
check("load of a missing key is nil", xt.load("nothing_here") == nil)
check("save key with .. refused", fails(xt.save, "../init", "x"))
check("save key with / refused", fails(xt.save, "a/b", "x"))
check("load key with .. refused", fails(xt.load, "../init"))
check("save over 64 KB refused", fails(xt.save, "big", string.rep("x", 70000)))
check("save does not reach other files", xt.load("init") == nil)

-- sounds and pictures only from their folders
check("sound outside sound/ refused", fails(xt.sound, "../../config.cfg"))
check("sound with .. refused", fails(xt.sound, "sound/../../x.wav"))
check("pic outside gfx/ refused", fails(xt.draw.pic, "maps/x", 0, 0, 1, 1))
check("clip outside the hud event refused", fails(xt.draw.clip, 0, 0, 1, 1))

-- the built-in widget library
local okui, ui = pcall(require, "xt.ui")
check("require xt.ui", okui and type(ui) == "table" and type(ui.button) == "function")
check("other xt modules refused", fails(require, "xt.secret"))

-- normal Lua still works
check("pcall catches ordinary errors", pcall(error, "x") == false)
local co = coroutine.wrap(function(a) local b = coroutine.yield(a + 1); return b * 2 end)
check("coroutines work", co(1) == 2 and co(5) == 10)
check("utf8 works", utf8.len("héllo") == 5)
check("math works", math.floor(3.7) == 3)

xt.command("selftest", function(...) print("SANDBOX command args " .. table.concat({...}, ",")) end, "sandbox self test")
xt.on("frame", function() end)
print(string.format("SANDBOX DONE %d/%d", passed, total))
