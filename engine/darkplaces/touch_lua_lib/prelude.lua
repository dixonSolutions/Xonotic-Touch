-- Godnotic: Lua parts of the xt API, run in every mod's state before its init.lua
-- (touch_lua_api.c). The same limits as the mod's own code apply.
local xt = xt
local sqrt, atan, cos, sin, pi = math.sqrt, math.atan, math.cos, math.sin, math.pi

-- vectors: {x, y, z} with arithmetic
local V = {}
V.__index = V
local function vec(x, y, z)
  if type(x) == "table" then x, y, z = x.x or x[1], x.y or x[2], x.z or x[3] end
  return setmetatable({x = x or 0, y = y or 0, z = z or 0}, V)
end
local function asv(a)
  if type(a) == "number" then return a, a, a end
  return a.x or a[1] or 0, a.y or a[2] or 0, a.z or a[3] or 0
end
V.__add = function(a, b) local ax, ay, az = asv(a) local bx, by, bz = asv(b) return vec(ax + bx, ay + by, az + bz) end
V.__sub = function(a, b) local ax, ay, az = asv(a) local bx, by, bz = asv(b) return vec(ax - bx, ay - by, az - bz) end
V.__mul = function(a, b) local ax, ay, az = asv(a) local bx, by, bz = asv(b) return vec(ax * bx, ay * by, az * bz) end
V.__div = function(a, b) local ax, ay, az = asv(a) local bx, by, bz = asv(b) return vec(ax / bx, ay / by, az / bz) end
V.__unm = function(a) return vec(-a.x, -a.y, -a.z) end
V.__eq = function(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
V.__tostring = function(a) return string.format("(%g %g %g)", a.x, a.y, a.z) end
function V:len2() return self.x * self.x + self.y * self.y + self.z * self.z end
function V:len() return sqrt(self:len2()) end
function V:normalize() local l = self:len() if l == 0 then return vec(0, 0, 0) end return vec(self.x / l, self.y / l, self.z / l) end
function V:dot(b) local bx, by, bz = asv(b) return self.x * bx + self.y * by + self.z * bz end
function V:cross(b) local bx, by, bz = asv(b) return vec(self.y * bz - self.z * by, self.z * bx - self.x * bz, self.x * by - self.y * bx) end
function V:dist(b) return (self - vec(b)):len() end
function V:copy() return vec(self.x, self.y, self.z) end
xt.vec = vec

-- angles {pitch, yaw, roll} (degrees, as the game uses them) to a direction
function xt.forward(a)
  local p, y = asv(a)
  p, y = p * pi / 180, y * pi / 180
  return vec(cos(p) * cos(y), cos(p) * sin(y), -sin(p))
end

function xt.vectoangles(d)
  local x, y, z = asv(d)
  local yaw, pitch = 0, 0
  if x == 0 and y == 0 then
    pitch = z > 0 and -90 or 90
  else
    yaw = atan(y, x) * 180 / pi
    if yaw < 0 then yaw = yaw + 360 end
    pitch = -atan(z, sqrt(x * x + y * y)) * 180 / pi
  end
  return vec(pitch, yaw, 0)
end

-- tables in the mod's storage, as JSON
xt.data = {}
function xt.data.save(key, value)
  return xt.save(key, xt.json.encode(value))
end
function xt.data.load(key, default)
  local text = xt.load(key)
  if not text then return default end
  local v = xt.json.decode(text)
  if v == nil then return default end
  return v
end
