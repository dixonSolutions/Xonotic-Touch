-- leaderboard_http: the Internet tier.
--
-- xt.http only reaches the hosts the manifest's "http:" line names, over
-- https (plain http only to this machine, for testing), and only once the
-- player turned the Internet tier on for this mod. The answer comes back to
-- the callback a few frames later; the game never waits for it.

local function scores()
  local list = {}
  for _, id in ipairs(xt.players()) do
    local p = xt.player(id)
    list[#list + 1] = {name = p.name, frags = p.frags, bot = p.bot}
  end
  return {map = xt.map(), time = xt.time.unix(), players = list}
end

local function post(why)
  if not xt.game.has("http") then
    print("leaderboard_http: no HTTP in this build")
    return
  end
  local ok, err = pcall(xt.http.post, xt.setting("url"), xt.json.encode(scores()), function(status, body)
    print("leaderboard_http: posted (" .. why .. "), answer " .. status .. " " .. body:sub(1, 60))
  end)
  if not ok then print("leaderboard_http: " .. tostring(err)) end
end

xt.on("matchend", function() post("match end") end)
xt.command("post", function() post("command") end, "post the scores now")
