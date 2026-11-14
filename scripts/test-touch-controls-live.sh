#!/bin/bash
# The in-match touch controls in a running game, offscreen, with scripted
# fingers put through the engine's real SDL event path (an LD_PRELOAD shim,
# tests/touch-controls/finj.c). Both HUD copies are driven: the port's CSQC
# HUD in a local match, and the engine HUD on a dedicated server that runs
# STOCK Xonotic game code (what every public server is).
#
#   input   a slow swipe, FIRE held then dragged, HOP held then dragged and
#           kept down, the stick, a flick; at a tablet's and a phone's
#           density. Passes when: dragging off FIRE turns the view while it
#           keeps firing; the same for HOP; the same swipe in centimetres
#           turns the same angle at both densities; held HOP jumps again and
#           again; the stick moves at the thumb's angle and stops on lift; a
#           flick turns further than a slow drag of the same length.
#   join    Deathmatch and CTF on the stock server: the join dialog is up
#           for an observer at match start; Spectate keeps observing and it
#           does not come back; MENU > Join game reopens it; choosing Join
#           (a team in CTF) gives mode playing and the dialog closes itself.
#   rects   on the stock server at 1920x1200, 2340x1080 and 1280x720:
#           spectating, playing, scoreboard, dead, intermission and the map
#           vote; no two controls (nor the score list, clock and info
#           messages) intersect in any of them.
#   local   a local Deathmatch puts the player straight into the game.
#
#   BASEDIR=<Godnotic basedir> STOCKBASE=<basedir with upstream Xonotic data only> \
#       scripts/test-touch-controls-live.sh [input] [join] [rects] [local]
#
# Environment: ENGINE (default build/bin/xonotic; it also runs the server),
# LIBS (extra LD_LIBRARY_PATH), OUT (keep logs and screenshots there), CPUS
# (a taskset list on a loaded machine). Exit 77: skipped (no engine or data).
# Nothing here is a finger on glass: timing and geometry only.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
BASEDIR="${BASEDIR:-}"
STOCKBASE="${STOCKBASE:-}"
T="$ROOT/tests/touch-controls"
[ -x "$ENGINE" ] && [ -d "$BASEDIR/data" ] && [ -d "$STOCKBASE/data" ] || { echo "SKIP: set ENGINE, BASEDIR and STOCKBASE"; exit 77; }
command -v sdl2-config > /dev/null || { echo "SKIP: sdl2-config (SDL2 headers) is needed to build the finger shim"; exit 77; }
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
RUN=()
[ -n "${CPUS:-}" ] && RUN=(taskset -c "$CPUS")
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-controls.XXXXXX")"
cleanup() {
	jobs -p | xargs -r kill 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
cc -shared -fPIC -O2 -o "$WORK/finj.so" "$T/finj.c" $(sdl2-config --cflags) -ldl || exit 1
fail=0

# the helpers that move and aim for the player are off: the fingers are measured
QUIET='set cl_touch_practice_pilot 0; set cl_touch_autoshoot 0; set cl_touch_aimassist 0; set cl_touch_ledgeguard 0; set cl_touch_autododge 0; set cl_welcome 0; set xt_go 0'

dims() { # dev -> W H and the touch cvars
	case $1 in
	tablet) W=1920 H=1200 DPI=283 ;;
	phone) W=2340 H=1080 DPI=395 ;;
	desk) W=1280 H=720 DPI=0 ;;
	esac
}

client() { # name dev tour.txt "console text" timeout
	local d="$WORK/$1"; dims "$2"
	mkdir -p "$d/data"
	printf '%s\n' "$4" | tr ';' '\n' > "$d/data/tour.cfg"
	LD_PRELOAD="$WORK/finj.so" XT_INJECT="$3" timeout -k 5 "$5" "${RUN[@]}" "$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$d" \
		+vid_fullscreen 0 +vid_width $W +vid_height $H +set vid_touchscreen 1 +set vid_touchscreen_detected 1 \
		+set vid_touchscreen_xdpi $DPI +set vid_touchscreen_ydpi $DPI +set _touch_assets_ready 1 +set touch_setup_done 1 \
		+set touch_profile_done 1 +set cl_allow_uid2name 0 +scr_screenshot_jpeg 0 +scr_screenshot_png 1 +scr_screenshot_timestamp 0 \
		+exec tour.cfg > "$d/client.log" 2>&1
}

server() { # name port gametype map bots [maplist]
	local d="$WORK/$1"
	mkdir -p "$d"
	"${RUN[@]}" "$ENGINE" -dedicated -xonotic -basedir "$STOCKBASE" -userdir "$d" +set port "$2" +set sv_public 0 \
		+set bot_number "$5" +set skill 1 +set g_maplist "${6:-$4}" +set g_warmup 0 +set g_dm 0 +set "g_$3" 1 +set g_balance_teams 0 \
		+set rcon_password xtctl +set g_maplist_votable 4 +set g_maplist_votable_timeout 60 +set timelimit_override 5 +map "$4" > "$d/server.log" 2>&1 &
	local n; for n in $(seq 1 600); do grep -q "Server spawned" "$d/server.log" 2> /dev/null && break; sleep 0.1; done
	sleep 2
}

port() { echo $((26700 + RANDOM % 250)); }

case_input() {
	local p dev
	for dev in tablet phone; do
		dims $dev
		python3 "$T/gen_input.py" $W $DPI 2 > "$WORK/input_$dev.txt"
		client "input_local_$dev" $dev "$WORK/input_$dev.txt" "$QUIET; set g_warmup 0; set bot_number 0; map stormkeep; defer 16 \"set xt_go 1\"" 70 &
		p=$(port); server "srv_input_$dev" $p dm stormkeep 0
		client "input_remote_$dev" $dev "$WORK/input_$dev.txt" "$QUIET; connect 127.0.0.1:$p; defer 35 \"set xt_go 1\"" 90
		wait %1 2> /dev/null; kill %2 2> /dev/null; wait 2> /dev/null
	done
	python3 "$T/check_live.py" input "$WORK/input_local_tablet/client.log" "$WORK/input_local_phone/client.log" || fail=1
	python3 "$T/check_live.py" input "$WORK/input_remote_tablet/client.log" "$WORK/input_remote_phone/client.log" || fail=1
}

mark() { echo "$1 cmd echo MARK $2"; echo "$1.05 cmd touch_hud_state"; }

case_join() {
	local gt map choice p
	for gt in dm ctf; do
		if [ $gt = ctf ]; then map=implosion choice=red; else map=stormkeep choice=auto; fi
		{
			mark 0 arrived
			echo "1 cmd touch_join spectate"; echo "1.2 cmd togglemenu 0"
			mark 2 spectate
			mark 6 later
			echo "7 cmd touch_action join"
			mark 8 reopened
			echo "9 cmd touch_join $choice"
			mark 13 joined
			echo "14 cmd quit"
		} > "$WORK/join_$gt.txt"
		p=$(port); server "srv_join_$gt" $p $gt $map 3
		client "join_$gt" tablet "$WORK/join_$gt.txt" "$QUIET; connect 127.0.0.1:$p; defer 30 \"set xt_go 1\"" 80
		kill %1 2> /dev/null; wait 2> /dev/null
		python3 "$T/check_live.py" join "$WORK/join_$gt/client.log" $gt || fail=1
	done
}

rmark() { echo "$1 cmd echo MARK $2"; echo "$1.05 cmd touch_hud_state rects"; echo "$1.1 cmd screenshot shots/$2.png"; }

case_rects() {
	local dev p
	for dev in tablet phone desk; do
		{
			echo "0 cmd touch_join spectate"; echo "0.2 cmd togglemenu 0"
			rmark 3 spectating
			echo "4 cmd touch_join auto"
			rmark 8 playing
			echo "9 cmd +showscores"
			rmark 10 scoreboard
			echo "10.5 cmd -showscores"
			echo "11 cmd kill"
			rmark 14 dead
			echo "15 cmd rcon endmatch"
			rmark 19 intermission
			rmark 38 mapvote
			echo "39 cmd quit"
		} > "$WORK/rects_$dev.txt"
		p=$(port); server "srv_rects_$dev" $p dm stormkeep 2 "stormkeep boil glowplant finalrage silentsiege"
		client "rects_$dev" $dev "$WORK/rects_$dev.txt" "$QUIET; set rcon_password xtctl; set rcon_address 127.0.0.1:$p; connect 127.0.0.1:$p; defer 30 \"set xt_go 1\"" 110
		kill %1 2> /dev/null; wait 2> /dev/null
		python3 "$T/check_live.py" rects "$WORK/rects_$dev/client.log" || fail=1
	done
}

case_local() {
	{ mark 0 start; mark 3 later; echo "4 cmd quit"; } > "$WORK/local.txt"
	client local tablet "$WORK/local.txt" "$QUIET; set g_warmup 0; set bot_number 2; set skill 1; map stormkeep; defer 16 \"set xt_go 1\"" 60
	if grep -a "^TOUCHHUD" "$WORK/local/client.log" | tail -1 | grep -q "mode=playing\|mode=dead"; then
		echo "ok   local: a Deathmatch hosted here puts the player straight into the game"
	else
		echo "FAIL local: still $(grep -a '^TOUCHHUD' "$WORK/local/client.log" | tail -1 | cut -d' ' -f2) (log: $WORK/local/client.log)"; fail=1
	fi
	if grep -a "^TOUCHMENU" "$WORK/local/client.log" | tail -1 | grep -q "up=0"; then echo "ok   local: no dialog in the way"; else echo "FAIL local: a menu is up"; fail=1; fi
}

# A case that fails is run once more: these are timed scripts against a
# real engine, and a machine busy with other work can stall one for a second.
for c in ${*:-input join rects local}; do
	case $c in input|join|rects|local) ;; *) echo "unknown case $c"; exit 2 ;; esac
	fail=0
	"case_$c" > "$WORK/$c.out" 2>&1
	if [ $fail != 0 ]; then
		echo "-- $c: failed once, running it again:"; grep "^FAIL" "$WORK/$c.out"
		fail=0
		"case_$c" > "$WORK/$c.out" 2>&1
	fi
	cat "$WORK/$c.out"
	[ $fail = 0 ] || failed=1
done
fail=${failed:-0}
[ $fail = 0 ] && echo "touch controls live: all checks passed"
exit $fail
