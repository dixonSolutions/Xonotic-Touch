local function try(label, f, ...)
  local ok, err = pcall(f, ...)
  print("probe_cl: " .. label .. " " .. (ok and "ok" or ("err " .. tostring(err))))
  return ok, err
end

-- JSON, time, vectors, locale, game info: no game needed
local t = {a = 1, b = {1, 2, 3}, s = "x\"y\n", f = 1.5, yes = true}
local back = xt.json.decode(xt.json.encode(t))
print("probe_cl: json roundtrip " .. tostring(back.a == 1 and back.b[3] == 3 and back.s == "x\"y\n" and back.f == 1.5 and back.yes == true))
local deep = {}
local cur = deep
for i = 1, 40 do cur.x = {} cur = cur.x end
try("json deep", xt.json.encode, deep)
local v, err = xt.json.decode("{\"a\": [1, 2,")
print("probe_cl: json bad " .. tostring(v) .. " " .. tostring(err))
print("probe_cl: date " .. xt.time.date("%Y") .. " vec " .. tostring(xt.vec(1, 2, 3) + xt.vec(1, 1, 1)) .. " len " .. xt.vec(3, 4, 0):len())
local info = xt.game.info()
print("probe_cl: game platform=" .. info.platform .. " api=" .. info.api .. " http=" .. tostring(info.caps.http) .. " lang=" .. xt.locale.lang())
try("http without internet", xt.http.get, "https://example.org/", function() end)
try("engine without engine_full", xt.engine.cvar, "hostname")

local hud = 0
xt.hook("HUD_Draw_overlay", function(h) hud = hud + 1 end)
xt.hook("DrawCrosshair", function(h) end)
local overlay = 0
xt.on("hud_overlay", function() overlay = overlay + 1 end)
local events = {}
for _, ev in ipairs({"weapon", "damage", "spawn", "death", "score", "kill", "notification", "match", "connect", "chat", "print", "centerprint", "pickup", "hit"}) do
  xt.on(ev, function() events[ev] = (events[ev] or 0) + 1 end)
end
local notes = {}
xt.on("notification", function(kind, name, text)
  if #notes < 12 then notes[#notes + 1] = kind .. ":" .. name end
end)
xt.command("count", function()
  print("probe_cl: notifications " .. table.concat(notes, " "))
  local list = {}
  for k, n in pairs(events) do list[#list + 1] = k .. "=" .. n end
  table.sort(list)
  print("probe_cl: hud hooks " .. hud .. " overlay " .. overlay .. " csqc=" .. xt.csqc() .. " events " .. table.concat(list, ","))
  local ents = xt.entities()
  local kinds, hidden = {}, 0
  for _, e in ipairs(ents) do
    kinds[e.kind] = (kinds[e.kind] or 0) + 1
    if e.kind ~= "item" and not e.visible then hidden = hidden + 1 end
  end
  print("probe_cl: hidden listed " .. hidden)
  print("probe_cl: entities players=" .. (kinds.player or 0) .. " items=" .. (kinds.item or 0) .. " projectiles=" .. (kinds.projectile or 0) .. " offline=" .. tostring((xt.offline())))
end, "counts")

local ch = xt.channel("probe")
xt.command("flood", function()
  local sent = 0
  for i = 1, 50 do if ch:send({i = i}) then sent = sent + 1 end end
  print("probe_cl: flood sent " .. sent .. " ready=" .. tostring(ch:ready()))
  try("oversize", function() ch:send({s = string.rep("x", 2000)}) end)
end, "channel flood")
print("probe_cl: loaded")
