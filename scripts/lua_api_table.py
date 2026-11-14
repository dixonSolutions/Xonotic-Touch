"""The Lua mod API, declared once (scripts/gen-lua-api.py reads it).

Every function a mod can call is one entry here. The generator writes from it
  engine/darkplaces/touch_lua_api_gen.h   the registration table: name, C function,
                                          tier (checked on every call), realm
  docs/LUA_API.md                         the reference
and with --check fails when either is stale, when a "legacy" entry no longer
matches the TLua_SetField line that registers it, or when an event list in
the C code differs from the one here.

Fields of an entry:
  realm   "both", "client" or "server"
  name    "draw.line" (xt.draw.line) or "on" (xt.on)
  sig     the call as a mod writes it, and what it returns
  tier    standard, gamerules, internet, engine_full (docs/PLUGIN_STANDARD.md)
  impl    "c"        a TLuaAPI_<name> function, registered from the table
          "legacy:F" registered by hand with TLua_SetField(L, "<last>", F)
          "lua"      defined in Lua (engine/darkplaces/touch_lua_lib/prelude.lua)
  public  where it works: "yes" everywhere it applies; for client mods
          "godnotic-csqc" when it needs this game's client code, "offline" when
          it only answers in an offline match against bots, "host" for server mods
          (they only run on a server this game hosts)
  web     True when the web build has it
  limits  what bounds it (shown in the reference)
  doc     one or two sentences
"""

STD, RULES, NET, FULL = "standard", "gamerules", "internet", "engine_full"


def fn(realm, name, sig, doc, tier=STD, impl="c", public="yes", web=True, limits=""):
    return dict(realm=realm, name=name, sig=sig, doc=doc, tier=tier, impl=impl, public=public, web=web, limits=limits)


API = [
    # ---------------------------------------------------------------- both realms
    fn("both", "on", "xt.on(event, fn)", "Calls fn on every `event` (the event tables below). Several handlers per event run in the order added.", impl="legacy:tlua_on"),
    fn("both", "command", "xt.command(name, fn [, help [, {fromserver=true}]]) -> full name", "Adds the console command `lua_<mod>_<name>` (`sv_lua_<mod>_<name>` for server mods); fn gets the arguments as strings.", impl="legacy:tlua_command", limits="32 per mod; names a-z 0-9"),
    fn("both", "cvar", "xt.cvar(name) -> string|nil", "Reads a cvar. Private cvars and ones named like passwords, keys and tokens read as nil.", impl="legacy:tlua_cvar"),
    fn("both", "setcvar", "xt.setcvar(name, value) -> true", "Sets one of the mod's own cvars (`lua_<mod>_<name>`, created on first use) or an allowlisted game cvar.", impl="legacy:tlua_setcvar", limits="64 own cvars per mod; values up to 256 characters without ; \" $ \\"),
    fn("both", "exec", "xt.exec(line)", "Queues a console command from the realm's list (client: chat, weapon switching, scoreboard, ready, echo...; server: say, echo, and with Game rules restart, kick, match control).", impl="legacy:tlua_exec", limits="8 per call; no ; \" $ \\ or line breaks"),
    fn("both", "realtime", "xt.realtime() -> seconds", "Seconds since the game started.", impl="legacy:tlua_realtime"),
    fn("both", "save", "xt.save(key, text) -> true", "Stores text under key in the mod's own `data/` folder.", impl="legacy:tlua_save", limits="64 KB per key, 64 keys"),
    fn("both", "load", "xt.load(key) -> text|nil", "Reads what xt.save stored.", impl="legacy:tlua_load_saved"),
    fn("both", "setting", "xt.setting(name) -> string", "The current value of a `setting.<name>` the manifest declares.", impl="legacy:tlua_setting"),
    fn("both", "after", "xt.after(seconds, fn) -> id", "Runs fn once, later.", impl="legacy:tlua_after", limits="256 timers and coroutines per mod"),
    fn("both", "every", "xt.every(seconds, fn) -> id", "Runs fn repeatedly (0: every frame); fn returning false stops it.", impl="legacy:tlua_every"),
    fn("both", "cancel", "xt.cancel(id) -> bool", "Stops a timer or coroutine.", impl="legacy:tlua_cancel"),
    fn("both", "spawn", "xt.spawn(fn) -> id", "Runs fn as a coroutine the engine resumes; inside it `xt.sleep` pauses. This is how a mod runs a `while true do ... end` loop.", impl="legacy:tlua_spawn"),
    fn("both", "sleep", "xt.sleep([seconds])", "Inside xt.spawn: pause (0 or nothing: until the next frame).", impl="legacy:tlua_sleep"),
    fn("both", "hook", "xt.hook(name, fn)", "Subscribes to one of the game code's mutator hooks (server: LUA_HOOKS.md; client: the CSQC hooks, LUA_HOOKS.md). fn gets one table with the arguments; changing outputs and cancelling need Game rules on the server.", impl="c", public="godnotic-csqc"),
    fn("both", "hooknames", "xt.hooknames() -> {names}", "Every hook this realm can subscribe to.", impl="c"),
    fn("both", "json.encode", "xt.json.encode(value) -> string", "Tables (arrays and string-keyed), strings, numbers, booleans to JSON.", limits="depth 32, 256 KB"),
    fn("both", "json.decode", "xt.json.decode(text) -> value | nil, error", "JSON to Lua values (null becomes nil).", limits="depth 32, 256 KB of text"),
    fn("both", "time.now", "xt.time.now() -> seconds", "Seconds since the game started (as xt.realtime)."),
    fn("both", "time.clock", "xt.time.clock() -> seconds", "A high-resolution clock for measuring."),
    fn("both", "time.unix", "xt.time.unix() -> seconds", "Wall-clock time, seconds since 1970 (UTC)."),
    fn("both", "time.date", "xt.time.date([format [, unixtime]]) -> string", "Local date and time; format uses %Y %m %d %H %M %S %a %b %j (default \"%Y-%m-%d %H:%M:%S\").", limits="format up to 64 characters, those directives only"),
    fn("both", "game.info", "xt.game.info() -> table", "version, api, apimax, platform (\"android\", \"linux\", \"windows\", \"mac\", \"web\"), touch (bool), realm, csqc (client: \"godnotic\", \"foreign\" or \"none\"), offline (client: the offline-only rule is open), caps (table: http, vibrate, clipboard, channel, pages, hooks)."),
    fn("both", "game.has", "xt.game.has(capability) -> bool", "Feature test: \"http\", \"vibrate\", \"clipboard\", \"channel\", \"pages\", \"hooks\", \"csqc_hooks\", \"engine_full\", \"offline\"."),
    fn("both", "locale.lang", "xt.locale.lang() -> code", "The game's language (`prvm_language`), e.g. \"en\", \"de\"."),
    fn("both", "locale.t", "xt.locale.t(key [, ...]) -> string", "Looks key up in the mod's `locale/<lang>.txt` (then `locale/en.txt`; lines `key = text`), formats `%s` with the extra arguments, falls back to the key.", limits="locale files up to 64 KB, 512 keys"),
    fn("both", "data.save", "xt.data.save(key, value) -> true", "xt.save of a table, as JSON.", impl="lua", limits="as xt.save"),
    fn("both", "data.load", "xt.data.load(key [, default]) -> value", "xt.load and decode; default when missing or unreadable.", impl="lua"),
    fn("both", "vec", "xt.vec(x, y, z) -> vector", "A vector table {x, y, z} with + - * / unary -, ==, tostring, and :len() :len2() :normalize() :dot(v) :cross(v) :dist(v) :copy().", impl="lua"),
    fn("both", "forward", "xt.forward(angles) -> vector", "The unit direction of {pitch, yaw, roll} angles (x/y/z or 1/2/3), as the game uses them.", impl="lua"),
    fn("both", "vectoangles", "xt.vectoangles(dir) -> vector", "The angles {x=pitch, y=yaw, z=0} that look along dir.", impl="lua"),
    fn("both", "channel", "xt.channel(name) -> channel", "A named message channel between a server mod and client mods (see \"Messages between server and client mods\"). channel:send(...) and channel:on(fn).", limits="name a-z 0-9 _ . up to 32; 1 KB per message; rates in the channel section", public="yes"),
    fn("both", "settings.page", "xt.settings.page(desc) -> page", "Registers a settings page (rows: section, label, switch, slider, choice, text, button, list, image, progress, keybind) shown under Settings > Mods; see \"UI descriptions\".", limits="64 rows, depth 4, strings 256 bytes, 8 pages per mod"),
    fn("both", "menu.page", "xt.menu.page(desc) -> page", "Registers a page of its own in the Mods menu; same rows as settings pages.", limits="as settings.page"),
    fn("both", "page.get", "xt.page.get(pageid) -> page|nil", "A page this mod registered, to update it (page:set(row, field, value), page:rows(rows), page:remove())."),
    fn("both", "top", "xt.top() -> {cpu_ms, calls, mem_kb, throttled}", "This mod's own cost over the last second (lua_mod_top shows all mods)."),
    fn("both", "http.get", "xt.http.get(url, fn [, {headers}])", "HTTPS GET to a host the manifest lists (`http: host, host`); fn(status, body) runs when it is done. Plain http only to localhost. On the web the host must also allow web pages (CORS); a refused request answers -1.", tier=NET, limits="256 KB response, 15 s, 4 at a time and 30 a minute per mod"),
    fn("both", "http.post", "xt.http.post(url, body, fn [, {type=..., headers}])", "HTTPS POST, as http.get.", tier=NET, limits="body up to 64 KB, as http.get"),
    fn("both", "engine.cmd", "xt.engine.cmd(line)", "Full engine access: runs any console command (queued like typed input). `$` expansion is refused.", tier=FULL, limits="16 per call, 1024 characters"),
    fn("both", "engine.cvar", "xt.engine.cvar(name) -> string|nil", "Full engine access: reads any cvar that is not private.", tier=FULL),
    fn("both", "engine.setcvar", "xt.engine.setcvar(name, value) -> bool", "Full engine access: sets any cvar that is not private or read-only.", tier=FULL),
    # ---------------------------------------------------------------- client
    fn("client", "screen", "xt.screen() -> w, h", "The 2D size xt.draw uses.", impl="legacy:tlua_screen"),
    fn("client", "reserved", "xt.reserved() -> {{name, x, y, w, h}}", "Screen areas the HUD and touch controls use.", impl="legacy:tlua_reserved"),
    fn("client", "freespot", "xt.freespot(w, h [, side]) -> x, y", "A place for a w x h panel clear of them (\"left\" or \"right\").", impl="legacy:tlua_freespot"),
    fn("client", "ingame", "xt.ingame() -> bool", "In a match (connected and signed on).", impl="legacy:tlua_ingame"),
    fn("client", "islocal", "xt.islocal() -> bool", "This game runs the server.", impl="legacy:tlua_islocal"),
    fn("client", "map", "xt.map() -> name", "The map, \"\" outside a match.", impl="legacy:tlua_map"),
    fn("client", "time", "xt.time() -> seconds", "The match clock.", impl="legacy:tlua_gametime"),
    fn("client", "players", "xt.players() -> {{name, frags, color, slot, me}}", "The scoreboard.", impl="legacy:tlua_players"),
    fn("client", "me", "xt.me() -> {health, armor, weapon, alive, name, frags, color}", "The player's own state.", impl="legacy:tlua_me"),
    fn("client", "fingers", "xt.fingers() -> {{id, x, y}}", "Fingers on the screen in 2D coordinates.", impl="legacy:tlua_fingers"),
    fn("client", "mouse", "xt.mouse() -> x, y", "The pointer.", impl="legacy:tlua_mouse"),
    fn("client", "keydown", "xt.keydown(key) -> bool", "A game key is held.", impl="legacy:tlua_keydown"),
    fn("client", "sound", "xt.sound(path) -> bool", "Plays a sound of the game (`sound/...`).", impl="legacy:tlua_sound", limits="16 a second from all mods, 64 different files"),
    fn("client", "draw.fill", "xt.draw.fill(x, y, w, h [, r, g, b, a])", "A rectangle (inside \"hud\"/\"draw\" handlers and HUD panels).", impl="legacy:tlua_draw_fill"),
    fn("client", "draw.text", "xt.draw.text(x, y, text [, size [, r, g, b, a]]) -> width", "Text; ^ colour codes work.", impl="legacy:tlua_draw_text", limits="1024 characters"),
    fn("client", "draw.textwidth", "xt.draw.textwidth(text [, size]) -> width", "Width of text.", impl="legacy:tlua_draw_textwidth"),
    fn("client", "draw.pic", "xt.draw.pic(path, x, y, w, h [, r, g, b, a]) -> bool", "A picture of the game (`gfx/...`).", impl="legacy:tlua_draw_pic", limits="64 different pictures"),
    fn("client", "draw.clip", "xt.draw.clip(x, y, w, h)", "Clips later drawing.", impl="legacy:tlua_draw_clip"),
    fn("client", "draw.noclip", "xt.draw.noclip()", "Ends clipping.", impl="legacy:tlua_draw_noclip"),
    fn("client", "draw.line", "xt.draw.line(x1, y1, x2, y2 [, width [, r, g, b, a]])", "A line."),
    fn("client", "draw.rect", "xt.draw.rect(x, y, w, h [, thickness [, r, g, b, a]])", "An outlined rectangle."),
    fn("client", "draw.gradient", "xt.draw.gradient(x, y, w, h, {r,g,b,a}, {r,g,b,a} [, vertical])", "A rectangle shading from the first colour to the second."),
    fn("client", "draw.circle", "xt.draw.circle(x, y, radius [, r, g, b, a [, filled [, segments]]])", "A circle or ring.", limits="64 segments"),
    fn("client", "draw.textsize", "xt.draw.textsize(text [, size]) -> w, h", "Text metrics: width and line height."),
    fn("client", "draw.image", "xt.draw.image(file, x, y, w, h [, r, g, b, a]) -> bool", "A PNG or TGA from the mod's own package (`img/logo.png`).", limits="1024 x 1024, 32 images per mod, 4 MB of pixels per mod"),
    fn("client", "draw.project", "xt.draw.project(v) -> x, y, onscreen", "Where a world point appears on the 2D screen this frame."),
    fn("client", "view", "xt.view() -> {origin, angles, forward, fov_x, fov_y, w, h}", "The camera, read only."),
    fn("client", "hud.panel", "xt.hud.panel{id, title, w, h, side, draw=fn} -> panel", "A HUD panel the engine places clear of the touch controls and the stock HUD (xt.freespot), draws inside the \"hud\" pass, and lets the player move (cvars `lua_<mod>_<id>x/y`, listed in `_lua_hud_panels`).", limits="8 panels per mod"),
    fn("client", "touch.button", "xt.touch.button{id, label, icon, w, h, side, press=fn, hold=fn, release=fn} -> button", "An on-screen touch button placed like a HUD panel; press/release on the finger, hold every frame while held. icon is an image of the mod's package.", limits="8 buttons per mod; 48 px minimum"),
    fn("client", "touch.remove", "xt.touch.remove(id)", "Removes one of the mod's buttons or panels."),
    fn("client", "bind", "xt.bind(key, down [, up])", "Calls down/up when the key (keyboard or gamepad name, e.g. \"F5\", \"JOY3\") is pressed in the game, instead of its binding. Keys bound to the console or the menu stay theirs.", limits="16 keys per mod"),
    fn("client", "unbind", "xt.unbind(key)", "Gives the key back."),
    fn("client", "effect", "xt.effect(name, v [, count])", "A particle effect of the game (effectinfo name) at a point, seen only by this player.", limits="32 a second from all mods"),
    fn("client", "clipboard", "xt.clipboard(text) -> bool", "Puts text on the clipboard; only inside a key, bind or touch button callback (a player's action).", web=False, limits="4 KB"),
    fn("client", "vibrate", "xt.vibrate(ms) -> bool", "Vibrates the phone when the platform offers it (game.has(\"vibrate\")).", limits="10 to 1000 ms, 2 a second"),
    fn("client", "entities", "xt.entities([kind]) -> {{id, kind, x, y, z, team, name, visible}}", "Entities around: kind \"player\", \"projectile\", \"item\" or nil for all. Online, players and projectiles are listed only while they are on screen and not behind a wall (what the player sees); offline against bots everything is listed.", public="offline", limits="256 entries"),
    fn("client", "offline", "xt.offline() -> bool, why", "Whether the offline-only rule is open: this game runs the server, no other human is connected, no demo plays."),
    fn("client", "csqc", "xt.csqc() -> \"godnotic\"|\"foreign\"|\"none\"", "Whose client game code runs: client hooks and the CSQC events need \"godnotic\"."),
    # ---------------------------------------------------------------- server
    fn("server", "players", "xt.players() -> {ids}", "Player ids (1..maxplayers) of connected clients and bots.", impl="legacy:tlua_sv_players"),
    fn("server", "player", "xt.player(id) -> {id, name, bot, frags, spectator, team, health, armor, alive, origin}", "A snapshot of one player (xt.ent(id) is the live handle).", impl="legacy:tlua_sv_player"),
    fn("server", "tell", "xt.tell(id, text)", "A chat line to one player.", impl="legacy:tlua_sv_tell", limits="10 a second per player"),
    fn("server", "centerprint", "xt.centerprint(id, text)", "A centered message to one player.", impl="legacy:tlua_sv_centerprint", limits="as tell"),
    fn("server", "broadcast", "xt.broadcast(text)", "A chat line to everyone.", impl="legacy:tlua_sv_broadcast", limits="5 a second"),
    fn("server", "map", "xt.map() -> name", "The map.", impl="legacy:tlua_sv_map"),
    fn("server", "time", "xt.time() -> seconds", "The server clock.", impl="legacy:tlua_sv_time"),
    fn("server", "maxplayers", "xt.maxplayers() -> n", "Player slots.", impl="legacy:tlua_sv_maxplayers"),
    fn("server", "entity", "xt.entity(n) -> table|nil", "A snapshot of an entity number (classname, netname, player, owner, health, team, origin).", impl="legacy:tlua_entity"),
    fn("server", "ent", "xt.ent(n) -> handle|nil", "A live handle to entity n (a player's is their id). Reading `h.<field>` reads any QC field by name; see \"Entity handles\" for methods. A handle to an entity that was removed (even if its slot is reused) is invalid: h:valid() is false and every other use is an error.", public="host"),
    fn("server", "ents.find", "xt.ents.find{class=, field=, value=, near=v, radius=r, limit=} -> {handles}", "Entities by classname, a field's value, and/or distance.", public="host", limits="256 results"),
    fn("server", "ents.spawn", "xt.ents.spawn(classname, {origin=v, angles=v, fields...}) -> handle", "Spawns an item (`item_*`, `weapon_*`), a marker (`target_position`, `info_teleport_destination`, `info_notnull`) or `func_pointparticles` like the map would. Removed when the mod stops.", tier=RULES, public="host", limits="64 live entities per mod, 16 a second"),
    fn("server", "world.trace", "xt.world.trace(from, to [, ignore]) -> {fraction, endpos, normal, ent, startsolid, allsolid}", "A traceline (players and monsters block it; ignore is a handle or id).", public="host"),
    fn("server", "world.contents", "xt.world.contents(v) -> {solid, water, slime, lava, sky}", "What is at a point.", public="host"),
    fn("server", "world.sound", "xt.world.sound(v, sample [, volume [, attenuation]]) -> bool", "Plays a sound of the game at a point for everyone.", public="host", limits="8 a second"),
    fn("server", "world.effect", "xt.world.effect(name, v [, count]) -> bool", "A particle effect (effectinfo name) at a point for everyone.", public="host", limits="16 a second"),
    fn("server", "world.gametype", "xt.world.gametype() -> name", "The game type (\"dm\", \"ctf\", ...).", public="host"),
    fn("server", "chatcommand", "xt.chatcommand(name, fn [, help])", "Players run fn(id, args) by typing `!name args` in chat (the line is not shown) or `cmd name args`.", public="host", limits="32 per mod; names a-z 0-9"),
    fn("server", "match.vote", "xt.match.vote(command [, by])", "Starts a vote on a stock vote command (`restart`, `gotomap <map>`, `endmatch`, `shuffleteams`, `kick #<id>` ...), as the server or as player `by`.", tier=RULES, public="host"),
    fn("server", "match.changemap", "xt.match.changemap(map)", "Goes to a map now.", tier=RULES, public="host"),
    fn("server", "match.endmatch", "xt.match.endmatch()", "Ends the match.", tier=RULES, public="host"),
    fn("server", "match.restart", "xt.match.restart()", "Restarts the map.", tier=RULES, public="host"),
    fn("server", "match.setcvar", "xt.match.setcvar(name, value) -> bool", "A gameplay cvar on the server list (g_balance_*, timelimit, fraglimit, g_instagib ...).", tier=RULES, public="host"),
    fn("server", "bots.add", "xt.bots.add([n [, skill]]) -> n", "Adds bots (raises bot_number).", tier=RULES, public="host", limits="up to the free slots"),
    fn("server", "bots.remove", "xt.bots.remove([n]) -> n", "Removes bots (lowers bot_number).", tier=RULES, public="host"),
    fn("server", "bots.skill", "xt.bots.skill([skill]) -> skill", "Reads or (with Game rules) sets bot skill 0-10.", public="host"),
    fn("server", "ban.add", "xt.ban.add(id, minutes [, reason])", "Kicks and bans a player's address.", tier=RULES, public="host"),
    fn("server", "ban.remove", "xt.ban.remove(index)", "Lifts a ban (the number `bans` lists).", tier=RULES, public="host"),
    fn("server", "page.show", "xt.page.show(id, desc) -> bool", "Shows a page (a UI description, as settings pages) on a player's screen when that player's game allows server pages; buttons and changes come back to the page's callbacks.", public="host", limits="16 KB, 64 rows, 2 a second per player, replies 20 a second"),
    fn("server", "page.close", "xt.page.close(id)", "Closes it.", public="host"),
]

# Events (xt.on). The C code's lists must equal these, in this order.
EVENTS = {
    "client": [
        ("frame", "dt", "every frame, with the seconds since the last one", "yes"),
        ("hud", "w, h", "the HUD pass, in a match: draw here", "yes"),
        ("draw", "w, h", "every frame, also in menus", "yes"),
        ("key", "key, down, char", "a game key; return true to take it", "yes"),
        ("menu", "open", "the menu opened or closed", "yes"),
        ("map", "name", "a new map (\"\" when leaving)", "yes"),
        ("unload", "", "the mod is stopping: save here", "yes"),
        ("print", "text", "a console line from the server; return false to hide it", "yes"),
        ("chat", "text", "a chat line from the server; return false to hide it", "yes"),
        ("centerprint", "text", "a centered message from the server; return false to hide it", "yes"),
        ("notification", "kind, name, text, args", "a game notification (kind \"info\", \"center\", \"announce\", \"multi\", \"choice\"); return false to hide it", "godnotic-csqc"),
        ("kill", "victim, attacker, name", "a kill-feed entry (the notification's name, e.g. DEATH_MURDER_FRAG)", "godnotic-csqc"),
        ("weapon", "new, old", "the player's weapon changed", "yes"),
        ("damage", "amount, health, armor", "the player took damage", "yes"),
        ("hit", "amount", "the player hit someone (damage dealt)", "godnotic-csqc"),
        ("pickup", "item", "the player picked an item up", "godnotic-csqc"),
        ("spawn", "", "the player (re)spawned", "yes"),
        ("death", "", "the player died", "yes"),
        ("spectate", "id, old", "the spectated player changed (0: none)", "yes"),
        ("match", "state, old", "\"warmup\", \"countdown\", \"playing\", \"overtime\", \"ended\"", "godnotic-csqc"),
        ("score", "frags, old", "the player's own score changed", "yes"),
        ("hud_overlay", "w, h", "inside the game's own HUD pass, over its panels", "godnotic-csqc"),
        ("scoreboard", "w, h", "while the scoreboard shows, over it", "godnotic-csqc"),
        ("touch", "id, x, y, phase", "a finger went down (phase \"down\"), moved (\"move\") or up (\"up\"); the touch controls see it too (put buttons with xt.touch.button where they are not)", "yes"),
        ("connect", "address", "joined a server (\"local\" for a game this game hosts)", "yes"),
        ("disconnect", "", "left it", "yes"),
    ],
    "server": [
        ("frame", "dt", "every server frame", "host"),
        ("map", "name", "a new map", "host"),
        ("connect", "id", "a player connected", "host"),
        ("disconnect", "id", "a player left", "host"),
        ("spawn", "id", "a player spawned", "host"),
        ("death", "victim, attacker, deathtype", "a player died (attacker 0: the world)", "host"),
        ("damage", "target, attacker, damage, deathtype", "a player takes damage; return a number to replace it (Game rules)", "host"),
        ("chat", "id, text, team", "a chat line; return false to hold it back", "host"),
        ("pickup", "id, item", "an item touched; return false to keep it from being picked up", "host"),
        ("clientcmd", "id, command, args", "`cmd <command>`; return true when handled", "host"),
        ("matchend", "", "the match ended", "host"),
        ("unload", "", "the mod is stopping", "host"),
    ],
}

# Hooks a client mod may write (outputs) and cancel without Game rules: they only
# change what this player's own screen shows. Every other client hook is read only.
CLIENT_WRITABLE = {"HUD_Draw_overlay", "DrawCrosshair", "DrawScoreboard", "DrawInfoMessages", "DrawViewModel", "DrawReticle", "ShowNames_Draw", "HUD_Command", "CSQC_ConsoleCommand", "WantEventchase", "CustomizeEventchase", "ForcePlayermodels_Skip", "ForcePlayercolors_Skip", "UpdateCrosshair", "HUD_Damage_show", "TeamRadar_Draw", "ShowRaceTimer", "DrawScoreboardAccuracy", "DrawScoreboardItemStats", "DrawDeathScoreboard", "HUD_Contents", "ShowRankings", "AnnouncerOption"}

TIERS = [
    ("standard", "on with the mod", "events and hooks (read), drawing, HUD panels, touch buttons and binds, sounds and effects, its own settings, storage and pages, reading game settings, the curated console commands, chat and notifications, messages with the server you joined"),
    ("gamerules", "a switch", "server: hook outputs and cancelling, changing players and entities, spawning, votes, map and match control, bots, kicks and bans (only in games you host)"),
    ("internet", "a switch", "HTTPS to the hosts the manifest lists (`http:`); on the web build, hosts that allow web pages (CORS)"),
    ("engine_full", "a switch with a warning; never by \"grant all\"; a mod installed from a link needs a second confirmation", "any console command, any cvar that is not private (passwords and keys stay hidden)"),
]
