#!/bin/bash
# The Lua mod suite: every example mod built, installed through the real
# install path, turned on, run in a headless bot match and checked by what it
# does, then uninstalled; plus the API probes (tiers, stale handles, private
# cvars, re-entrancy, channel limits, client hooks) and the misbehaving trio.
#
#   scripts/test-mods.sh                 static checks only (no game data needed):
#                                          generators --check, Lua syntax of every
#                                          example and every ```lua block in the docs,
#                                          manifests, archive build determinism
#   BASEDIR=<basedir> scripts/test-mods.sh   and the match scenarios
#
# BASEDIR is a Xonotic basedir (data/*.pk3) whose game code is this checkout's
# (link engine/data/xonotic-data.pk3dir into data/ under a name that sorts
# last). ENGINE defaults to engine/darkplaces/darkplaces-sdl; KEEP=1 keeps the
# logs; ONLY=<scenario> runs one of: install, persist, online. See docs/MODDING.md,
# "Testing your mod".
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-mods.XXXXXX")"
HTTP_PID=""
cleanup() {
	[ -n "$HTTP_PID" ] && kill "$HTTP_PID" 2>/dev/null
	if [ -n "${KEEP:-}" ]; then echo "logs kept in $WORK"; else rm -rf "$WORK"; fi
}
trap cleanup EXIT

declare -a RESULTS
fail=0
check() { # name, condition result (0 ok)
	if [ "$2" = 0 ]; then RESULTS+=("PASS  $1"); else RESULTS+=("FAIL  $1"); fail=1; fi
}
has() { grep -aqE -- "$2" "$1"; }

# --- static checks -------------------------------------------------------------
python3 "$ROOT/scripts/gen-lua-api.py" --check > "$WORK/gen.txt" 2>&1; check "API table and docs/LUA_API.md up to date (gen-lua-api.py --check)" $?
python3 "$ROOT/scripts/gen-lua-hooks.py" --check >> "$WORK/gen.txt" 2>&1; check "hook bridge and docs/LUA_HOOKS.md up to date (gen-lua-hooks.py --check)" $?

mkdir -p "$WORK/bin"
if cc -O1 -o "$WORK/bin/lua-syntax-check" "$ROOT/scripts/lua-syntax-check.c" -I"$ROOT/engine/darkplaces" -lm > "$WORK/cc.txt" 2>&1; then
	find "$ROOT/touch/lua/examples" "$ROOT/tests/lua-mods" -name '*.lua' -print0 | xargs -0 "$WORK/bin/lua-syntax-check" > "$WORK/syntax.txt" 2>&1
	check "every example and fixture compiles ($(tail -1 "$WORK/syntax.txt"))" $?
	python3 "$ROOT/scripts/check-mod-docs.py" extract "$ROOT/docs" > "$WORK/docblocks.lua"
	"$WORK/bin/lua-syntax-check" - < "$WORK/docblocks.lua" > "$WORK/docsyntax.txt" 2>&1
	check "every \`\`\`lua block in docs/ compiles ($(tail -1 "$WORK/docsyntax.txt"))" $?
else
	check "build the Lua syntax checker" 1
fi
python3 "$ROOT/scripts/check-mod-docs.py" manifests "$ROOT/touch/lua/examples" "$ROOT/tests/lua-mods" > "$WORK/manifests.txt" 2>&1
check "every example's mod.txt is valid ($(tail -1 "$WORK/manifests.txt"))" $?
python3 "$ROOT/scripts/check-mod-docs.py" links "$ROOT/docs/MOD_COOKBOOK.md" "$ROOT/touch/lua/examples" > "$WORK/links.txt" 2>&1
check "every cookbook recipe links an example that exists, every example has a README" $?
"$ROOT/scripts/build-example-mods.sh" "$WORK/a" > "$WORK/build1.txt" 2>&1 && "$ROOT/scripts/build-example-mods.sh" "$WORK/b" > /dev/null 2>&1
check "example archives build" $?
diff -r "$WORK/a" "$WORK/b" > /dev/null 2>&1
check "example archives are byte-identical when built twice" $?

if [ -z "${BASEDIR:-}" ] || [ ! -x "$ENGINE" ]; then
	echo "(BASEDIR or ENGINE not set: the match scenarios are skipped)"
	printf '%s\n' "${RESULTS[@]}"
	[ "$fail" = 0 ] && echo "static checks passed" || { cat "$WORK"/gen.txt "$WORK"/syntax.txt "$WORK"/docsyntax.txt "$WORK"/manifests.txt "$WORK"/links.txt 2>/dev/null | grep -v "^ok" | tail -30; }
	exit "$fail"
fi

# --- the match scenarios ----------------------------------------------------------
UD="$WORK/home"
mkdir -p "$UD/data"
ZIPS="$WORK/a"
CLIENT_MODS="panel_clock touch_weapon hit_feedback quiet_center round_hud mod_menu radar"
SERVER_MODS="round_timer vampire chat_vote item_placer bot_control player_stats leaderboard_http ticker"
FIXTURES_SV="probe_sv bad_loop bad_hook bad_hog"
FIXTURES_CL="probe_cl bad_clhook"
MAP="${MAP:-stormkeep}"

run_engine() { # log, timeout, args...
	local log="$1" t="$2"; shift 2
	SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout -k 10 "$t" "$ENGINE" -xonotic -nosound \
		-basedir "$BASEDIR" -userdir "${RUN_UD:-$UD}" +vid_width 1280 +vid_height 720 +vid_fullscreen 0 "$@" > "$log" 2>&1
}

# a receiver for leaderboard_http (POST bodies to a file)
HPORT=$((27000 + RANDOM % 2000))
python3 - "$WORK/posts.txt" "$HPORT" > "$WORK/http.log" 2>&1 <<'PY' &
import http.server, sys
out = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        with open(out, "ab") as f:
            f.write(self.path.encode() + b" " + body + b"\n")
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"stored")
    def log_message(self, *a):
        pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[2])), H).serve_forever()
PY
HTTP_PID=$!

# 1. install everything through the installer, turn it on, play
{
	t=0.5
	for m in $CLIENT_MODS; do
		echo "defer $t \"lua_mod_install $ZIPS/mods/$m.zip\""; t=$(echo "$t + 0.3" | bc)
		echo "defer $t \"lua_install_confirm\""; t=$(echo "$t + 0.3" | bc)
	done
	for m in $SERVER_MODS; do
		echo "defer $t \"sv_lua_plugin_install $ZIPS/plugins/$m.zip\""; t=$(echo "$t + 0.3" | bc)
		echo "defer $t \"lua_install_confirm\""; t=$(echo "$t + 0.3" | bc)
	done
	for f in probe_sv; do echo "defer $t \"sv_lua_plugin_install $ROOT/tests/lua-mods/probe/$f\""; t=$(echo "$t + 0.3" | bc); echo "defer $t \"lua_install_confirm\""; t=$(echo "$t + 0.3" | bc); done
	for f in probe_cl; do echo "defer $t \"lua_mod_install $ROOT/tests/lua-mods/probe/$f\""; t=$(echo "$t + 0.3" | bc); echo "defer $t \"lua_install_confirm\""; t=$(echo "$t + 0.3" | bc); done
	for f in bad_loop bad_hook bad_hog; do echo "defer $t \"sv_lua_plugin_install $ROOT/tests/lua-mods/bad/$f\""; t=$(echo "$t + 0.3" | bc); echo "defer $t \"lua_install_confirm\""; t=$(echo "$t + 0.3" | bc); done
	echo "defer $t \"lua_mod_install $ROOT/tests/lua-mods/bad/bad_clhook\""; t=$(echo "$t + 0.3" | bc); echo "defer $t \"lua_install_confirm\""; t=$(echo "$t + 0.3" | bc)
	for m in $CLIENT_MODS $FIXTURES_CL; do echo "defer $t \"lua_mod_enable $m\""; done
	for m in $SERVER_MODS $FIXTURES_SV; do echo "defer $t \"sv_lua_plugin_enable $m\""; done
	t=$(echo "$t + 0.5" | bc)
	# the game rules mods get their tier; probe_sv's first checks run without it
	for m in vampire chat_vote item_placer bot_control; do echo "defer $t \"sv_lua_plugin_permission $m grant gamerules\""; done
	echo "defer $t \"lua_mods\""
	echo "defer $t \"sv_lua_plugins\""
	echo "defer $t \"set lua_touch_weapon_nextx 600; set lua_touch_weapon_nexty 300; set lua_quiet_center_words Prepare; set sv_lua_leaderboard_http_url http://127.0.0.1:$HPORT/scores\""
	echo "defer $t \"set sv_cheats 1; set maxplayers 8; set bot_number 3; set skill 6; set g_warmup 0; set slowmo 2; set g_chat_flood_spl 0; set g_chat_flood_lmax 0; set sv_clientcommand_antispam_time -1; set sv_clientcommand_antispam_count 10000\""
	t=$(echo "$t + 0.5" | bc)
	echo "defer $t \"map $MAP\""
	B=$(echo "$t + 9" | bc)
	cat <<EOF
defer $B "togglemenu 0"
defer $(echo "$B + 1" | bc) "cmd join"
defer $(echo "$B + 2" | bc) "togglemenu 0"
defer $(echo "$B + 3" | bc) "cmd join; togglemenu 0"
defer $(echo "$B + 4" | bc) "sv_lua_probe_sv_tiers"
defer $(echo "$B + 4.5" | bc) "sv_lua_plugin_permission probe_sv grant all"
defer $(echo "$B + 5" | bc) "sv_lua_probe_sv_tiers"
defer $(echo "$B + 5.5" | bc) "sv_lua_plugin_permission probe_sv grant engine_full"
defer $(echo "$B + 6" | bc) "sv_lua_probe_sv_tiers"
defer $(echo "$B + 6.5" | bc) "sv_lua_probe_sv_stale"
defer $(echo "$B + 7" | bc) "sv_lua_probe_sv_private"
defer $(echo "$B + 7.5" | bc) "sv_lua_probe_sv_reentry"
defer $(echo "$B + 8" | bc) "sv_lua_probe_sv_world"
defer $(echo "$B + 8.5" | bc) "lua_probe_cl_flood"
defer $(echo "$B + 9.5" | bc) "sv_lua_probe_sv_chancount"
defer $(echo "$B + 9.8" | bc) "togglemenu 0"
defer $(echo "$B + 10" | bc) "say !time"
defer $(echo "$B + 10.5" | bc) "say !drop"
defer $(echo "$B + 11" | bc) "say !mark"
defer $(echo "$B + 11.5" | bc) "say !mark"
defer $(echo "$B + 12" | bc) "say !portal"
defer $(echo "$B + 12.5" | bc) "say !clear"
defer $(echo "$B + 13" | bc) "say !bots 2"
defer $(echo "$B + 13.5" | bc) "say !skill 5"
defer $(echo "$B + 14" | bc) "say !round"
defer $(echo "$B + 15" | bc) "lua_pages"
defer $(echo "$B + 15.5" | bc) "lua_page_event server.round_timer.menu next press"
defer $(echo "$B + 16" | bc) "lua_page_event panel_clock.main seconds change 0"
defer $(echo "$B + 16.5" | bc) "lua_panel_clock_seconds"
defer $(echo "$B + 17" | bc) "lua_page_event panel_clock.main reset press"
defer $(echo "$B + 17.5" | bc) "lua_page_json panel_clock.settings"
defer $(echo "$B + 18" | bc) "lua_page_event mod_menu.notes draft change hello"
defer $(echo "$B + 18.5" | bc) "lua_page_event mod_menu.notes add press"
defer $(echo "$B + 19" | bc) "lua_page_event mod_menu.notes list select n1"
defer $(echo "$B + 19.5" | bc) "lua_page_json mod_menu.notes"
defer $(echo "$B + 20" | bc) "lua_mod_simulate finger 0 620 320 1"
defer $(echo "$B + 21" | bc) "lua_mod_simulate finger 0 620 320 0"
defer $(echo "$B + 21.5" | bc) "lua_mod_simulate key F7 1"
defer $(echo "$B + 21.6" | bc) "lua_mod_simulate key F7 0"
defer $(echo "$B + 22" | bc) "sv_lua_leaderboard_http_post"
defer $(echo "$B + 22.5" | bc) "sv_lua_plugin_permission leaderboard_http grant internet"
defer $(echo "$B + 23" | bc) "sv_lua_leaderboard_http_post"
defer $(echo "$B + 24" | bc) "_lua_hud_panels"
defer $(echo "$B + 30" | bc) "lua_probe_cl_count"
defer $(echo "$B + 30.5" | bc) "lua_quiet_center_count"
defer $(echo "$B + 31" | bc) "sv_lua_hooks"
defer $(echo "$B + 31.2" | bc) "lua_hooks"
defer $(echo "$B + 31.5" | bc) "lua_mod_top"
defer $(echo "$B + 32" | bc) "lua_mods"
defer $(echo "$B + 32.2" | bc) "sv_lua_plugins"
defer $(echo "$B + 33" | bc) "quit"
EOF
} > "$UD/data/mods1.cfg"

LOG1="$WORK/install.log"
if [ -z "${ONLY:-}" ] || [ "${ONLY:-}" = install ]; then
	run_engine "$LOG1" 240 +exec mods1.cfg
	RC=$?
	check "scenario 1: the engine ran the match and quit cleanly (exit $RC)" "$([ "$RC" = 0 ] && echo 0 || echo 1)"
	n=$(grep -ac "installed" "$LOG1"); check "installer: every example and fixture installed ($n)" "$([ "$n" -ge 21 ] && echo 0 || echo 1)"
	for m in $CLIENT_MODS probe_cl; do has "$LOG1" "Lua mod $m loaded"; check "$m loads (standard tier: no grant asked)" $?; done
	for m in $SERVER_MODS probe_sv; do has "$LOG1" "Lua (plugin|server mod) $m loaded"; check "$m loads" $?; done
	# examples, by what they do
	has "$LOG1" "panel_clock: drawn at"; check "panel_clock: HUD panel drawn" $?
	has "$LOG1" "panel_clock: seconds change false"; check "panel_clock: a settings page switch reaches the mod's callback" $?
	has "$LOG1" 'lua_panel_clock_seconds.* is "0"'; check "panel_clock: the switch is bound to its setting (cvar 0, kept in the config)" $?
	has "$LOG1" "panel_clock: reset pressed"; check "panel_clock: a page button runs its callback" $?
	has "$LOG1" '"id":"panel_clock.settings".*"setting":"lua_panel_clock_size"'; check "panel_clock: manifest settings become a settings page (JSON)" $?
	has "$LOG1" "_lua_hud_panels.*\"id\":\"clock\""; check "panel_clock: its panel is listed in _lua_hud_panels" $?
	has "$LOG1" "touch_weapon: button pressed"; check "touch_weapon: a touch button press reaches the mod" $?
	has "$LOG1" "touch_weapon: long press"; check "touch_weapon: hold runs every frame" $?
	has "$LOG1" "touch_weapon: button released"; check "touch_weapon: release" $?
	has "$LOG1" "touch_weapon: key F7"; check "touch_weapon: a key bind takes the key" $?
	has "$LOG1" "hit_feedback: kill "; check "hit_feedback: kill feed entries arrive ($(grep -ac 'hit_feedback: kill ' "$LOG1"))" $?
	has "$LOG1" "round_timer: hello from player 1"; check "round_timer/round_hud: a client mod's request reaches the server mod" $?
	has "$LOG1" "round_hud: round [0-9]+, [0-9]+ s left"; check "round_timer/round_hud: the server mod's messages reach the client mod" $?
	has "$LOG1" "round_timer: page shown to player 1"; check "round_timer: !round shows a page from the server" $?
	has "$LOG1" "server.round_timer.menu"; check "round_timer: the page is in the client's page store, marked as from the server" $?
	has "$LOG1" "round_timer: next round asked by player 1"; check "round_timer: a press on the server page comes back to the server mod" $?
	has "$LOG1" "mod_menu: added 'hello', 1 notes"; check "mod_menu: text field and button reach the mod" $?
	has "$LOG1" "mod_menu: selected n1"; check "mod_menu: list selection" $?
	has "$LOG1" '"id":"list".*"label":"hello"'; check "mod_menu: page:set updated the list the renderers see" $?
	has "$LOG1" "radar: offline=true \(offline match\), [1-9][0-9]* players listed"; check "radar: offline against bots it lists the players" $?
	has "$LOG1" "vampire: healed [0-9]+"; check "vampire: a write hook and an entity write change the game" $?
	has "$LOG1" "chat_vote: !time by player 1"; check "chat_vote: !time chat command" $?
	has "$LOG1" "Server time: "; check "chat_vote: the answer reached the player" $?
	has "$LOG1" "item_placer: spawned entity [0-9]+"; check "item_placer: spawned an item" $?
	has "$LOG1" "item_placer: teleported player 1"; check "item_placer: teleported between two markers" $?
	has "$LOG1" "item_placer: removed 1"; check "item_placer: removed what it spawned" $?
	has "$LOG1" "bot_control: bot_number now 2"; check "bot_control: !bots changed the bots" $?
	has "$LOG1" "bot_control: skill 5"; check "bot_control: !skill" $?
	has "$LOG1" "player_stats: loaded 0 players"; check "player_stats: starts empty" $?
	has "$LOG1" "player_stats: saved [1-9][0-9]* players \(unload\)"; check "player_stats: saved at quit" $?
	has "$LOG1" "leaderboard_http.*internet tier is not turned on"; check "leaderboard_http: no HTTP before the Internet tier is on" $?
	has "$LOG1" "leaderboard_http: posted \(command\), answer 200 stored"; check "leaderboard_http: posted once it was on, answer read" $?
	grep -aq '"players"' "$WORK/posts.txt" 2>/dev/null; check "leaderboard_http: the receiver got the scores as JSON" $?
	has "$LOG1" "ticker: loop 10"; check "ticker: a coroutine loop keeps running" $?
	has "$LOG1" "ticker: every 3" && ! has "$LOG1" "ticker: every 4"; check "ticker: xt.every stops when it returns false" $?
	has "$LOG1" "ticker: after 4 s"; check "ticker: xt.after" $?
	# the misbehaving trio and a hot client hook: stopped, everyone else goes on
	has "$LOG1" "Lua (plugin|server mod) bad_loop stopped: (time limit|instruction limit)"; check "bad_loop: an endless loop is stopped by the limits" $?
	has "$LOG1" "Lua (plugin|server mod) bad_hook stopped: .*attempt to index"; check "bad_hook: an error in a hook stops only that mod" $?
	has "$LOG1" "Lua (plugin|server mod) bad_hog stopped: memory limit"; check "bad_hog: the memory limit stops a hog" $?
	has "$LOG1" "Lua mod bad_clhook stopped: (time limit|hook budget)"; check "bad_clhook: a runaway client hook is stopped" $?
	n=$(grep -ac "ticker: loop" "$LOG1"); check "the others keep running after the bad ones stopped (ticker loops: $n)" "$([ "$n" -ge 4 ] && echo 0 || echo 1)"
	# the probes
	has "$LOG1" "probe_sv: spawn err .*permission denied.*gamerules"; check "probe: spawning needs Game rules" $?
	has "$LOG1" "probe_sv: write err .*permission denied.*gamerules"; check "probe: entity writes need Game rules" $?
	has "$LOG1" "probe_sv: spawn ok"; check "probe: spawning works with Game rules" $?
	n=$(grep -ac "probe_sv: enginecvar err" "$LOG1"); check "probe: engine_full not granted by \"grant all\" ($n refusals before its own grant)" "$([ "$n" = 2 ] && echo 0 || echo 1)"
	has "$LOG1" "probe_sv: enginecvar ok"; check "probe: engine_full works once granted (installed from a file: one step)" $?
	has "$LOG1" "probe_sv: stale before true" && has "$LOG1" "probe_sv: stale after false"; check "probe: a removed entity's handle turns invalid" $?
	has "$LOG1" "probe_sv: stale read err .*stale entity handle" && has "$LOG1" "probe_sv: stale write err .*stale entity handle"; check "probe: a stale handle can be neither read nor written" $?
	has "$LOG1" "probe_sv: rcon_password=nil" && has "$LOG1" "probe_sv: plain rcon_password=nil"; check "probe: private cvars stay hidden, with engine_full too" $?
	has "$LOG1" "probe_sv: manager cmd err .*manager's commands"; check "probe: engine_full cannot drive the mod manager" $?
	has "$LOG1" 'probe_sv: expansion err .*\$'; check "probe: engine_full cannot expand \$cvars" $?
	has "$LOG1" "probe_sv engine_full echo ran"; check "probe: engine_full runs commands" $?
	has "$LOG1" "probe_sv: reentry nested=0 spawned=true"; check "probe: hooks a mod's own API call causes are not delivered (re-entrancy)" $?
	has "$LOG1" "probe_sv: trace fraction"; check "probe: world trace, contents, gametype" $?
	has "$LOG1" "probe_sv: player entity 1"; check "probe: player handle fields" $?
	has "$LOG1" "probe_cl: flood sent ([1-9]|10) "; check "probe: a client mod's channel flood is cut at the rate limit ($(grep -ao 'probe_cl: flood sent [0-9]*' "$LOG1"))" $?
	has "$LOG1" "probe_cl: oversize err .*over 1024"; check "probe: oversize channel messages are refused" $?
	n=$(grep -ao "probe_sv: channel received [0-9]*" "$LOG1" | grep -o "[0-9]*$" | tail -1); check "probe: the server got only what passed the limit (${n:-none})" "$([ -n "$n" ] && [ "$n" -ge 1 ] && [ "$n" -le 10 ] && echo 0 || echo 1)"
	has "$LOG1" "probe_cl: json roundtrip true"; check "probe: xt.json round trip" $?
	has "$LOG1" "probe_cl: json deep err .*nested deeper"; check "probe: xt.json depth limit" $?
	has "$LOG1" "probe_cl: json bad nil"; check "probe: bad JSON returns nil and an error" $?
	has "$LOG1" "probe_cl: http without internet err .*permission denied.*internet"; check "probe: xt.http needs the Internet tier" $?
	has "$LOG1" "probe_cl: engine without engine_full err .*permission denied"; check "probe: xt.engine needs Full engine access" $?
	has "$LOG1" "probe_cl: hud hooks [1-9][0-9]* overlay [1-9][0-9]* csqc=godnotic"; check "probe: client hooks and CSQC events fire with this game's client code" $?
	has "$LOG1" "probe_cl: hud hooks.*events.*kill=[1-9]"; check "probe: client game events arrive" $?
	has "$LOG1" "probe_cl: entities players=[1-9][0-9]* .* offline=true"; check "probe: xt.entities lists the players offline" $?
	has "$LOG1" "Lua mods, the last second"; check "lua_mod_top prints per-mod cost" $?
	! has "$LOG1" "the UI bridge refused page"; check "every mod page is accepted by the UI bridge's store" $?
fi

# 2. a second start: what was kept, then uninstall everything
if [ -z "${ONLY:-}" ] || [ "${ONLY:-}" = persist ]; then
	{
		echo "set maxplayers 8; set bot_number 2"
		echo "map $MAP"
		echo "defer 8 \"togglemenu 0\""
		t=10
		for m in $CLIENT_MODS probe_cl bad_clhook; do echo "defer $t \"lua_mod_uninstall $m\""; t=$(echo "$t + 0.2" | bc); done
		for m in $SERVER_MODS $FIXTURES_SV; do echo "defer $t \"sv_lua_plugin_uninstall $m\""; t=$(echo "$t + 0.2" | bc); done
		echo "defer $t \"lua_mods\""
		echo "defer $(echo "$t + 0.2" | bc) \"sv_lua_plugins\""
		echo "defer $(echo "$t + 1" | bc) \"quit\""
	} > "$UD/data/mods2.cfg"
	LOG2="$WORK/persist.log"
	run_engine "$LOG2" 120 +exec mods2.cfg
	check "scenario 2: second start ran and quit cleanly" $?
	has "$LOG2" "player_stats: loaded [1-9][0-9]* players"; check "player_stats: the numbers survived a restart" $?
	has "$LOG2" "mod_menu: page registered with 1 notes"; check "mod_menu: its storage survived a restart" $?
	left=$(ls "$UD/mods" "$UD/plugins" 2>/dev/null | grep -vE "^(enabled.txt|$|/.*:)" | grep -v ":" | tr '\n' ' ')
	check "uninstall removed every example's folder (left: ${left:-none})" "$([ -z "$left" ] && echo 0 || echo 1)"
fi

# 3. online: a dedicated server in another process, the client over UDP. The
# offline-only rule must be closed, and channels must work over the network.
if [ -z "${ONLY:-}" ] || [ "${ONLY:-}" = online ]; then
	SUD="$WORK/server"
	OUD="$WORK/online"
	mkdir -p "$SUD/plugins" "$SUD/data" "$OUD/mods" "$OUD/data"
	cp -r "$ROOT/touch/lua/examples/plugins/round_timer" "$SUD/plugins/"
	echo round_timer > "$SUD/plugins/enabled.txt"
	cp -r "$ROOT/touch/lua/examples/mods/round_hud" "$ROOT/tests/lua-mods/probe/probe_cl" "$ROOT/touch/lua/examples/mods/radar" "$OUD/mods/"
	printf 'round_hud\nprobe_cl\nradar\n' > "$OUD/mods/enabled.txt"
	PORT=$((26200 + RANDOM % 500))
	printf 'set bot_number 4\nset skill 8\nset g_warmup 0\nset slowmo 2\nset sv_public 0\nport %s\nmap %s\n' "$PORT" "$MAP" > "$SUD/data/server.cfg"
	SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout -k 10 150 "$ENGINE" -xonotic -nosound -dedicated \
		-basedir "$BASEDIR" -userdir "$SUD" +exec server.cfg > "$WORK/server.log" 2>&1 &
	SPID=$!
	cat > "$OUD/data/online.cfg" <<EOF
defer 12 "connect 127.0.0.1:$PORT"
defer 30 "togglemenu 0"
defer 31 "cmd join"
defer 32 "togglemenu 0"
defer 60 "quit"
EOF
	OLOG="$WORK/online.log"
	RUN_UD="$OUD" run_engine "$OLOG" 120 +exec online.cfg
	check "scenario 3: the client joined a dedicated server and quit cleanly" $?
	kill "$SPID" 2>/dev/null; wait "$SPID" 2>/dev/null
	has "$OLOG" "probe_cl: entities players=[0-9]+ .* offline=false"; check "online: the offline-only rule is closed" $?
	has "$OLOG" "probe_cl: hidden listed 0"; check "online: no player or projectile behind a wall or off screen is listed" $?
	has "$OLOG" "radar: offline=false"; check "online: the radar sees the rule closed" $?
	has "$OLOG" "round_hud: round [0-9]+, [0-9]+ s left"; check "online: a server mod's messages reach the client mod over the network" $?
	has "$WORK/server.log" "round_timer: hello from player"; check "online: the client mod's hello reaches the dedicated server" $?
fi

echo
printf '%s\n' "${RESULTS[@]}"
pass=$(printf '%s\n' "${RESULTS[@]}" | grep -c '^PASS')
total=${#RESULTS[@]}
echo "$pass of $total checks passed"
exit "$fail"
