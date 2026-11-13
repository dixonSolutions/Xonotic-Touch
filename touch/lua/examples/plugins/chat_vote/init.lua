-- chat_vote: commands players type in chat (!name) or the console (cmd name),
-- and a vote on one of the game's own vote commands.

xt.chatcommand("time", function(player, args)
  xt.tell(player, "Server time: " .. xt.time.date("%H:%M") .. ", map " .. xt.map() .. " (" .. xt.world.gametype() .. ")")
  print("chat_vote: !time by player " .. player)
end, "the server's clock and the map")

xt.chatcommand("help", function(player)
  xt.tell(player, "!time, !shuffle, !help")
end, "the commands")

xt.chatcommand("shuffle", function(player)
  -- a vote as the player who asked, on the stock vote command
  xt.match.vote("shuffleteams", player)
  print("chat_vote: vote shuffleteams called by player " .. player)
end, "call a vote to shuffle the teams")
