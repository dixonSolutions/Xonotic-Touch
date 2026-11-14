#!/bin/bash
# The menu a native front end shows over a running match, offscreen: a
# Godnotic client with a native front end attached through the front-end
# bridge (`fe attach`, a heartbeat, a UI-bridge hello, events as `fe ui`),
# connected to a dedicated server that runs STOCK Xonotic game code (what
# every public server is). Nothing is drawn by Android here: the test reads
# what the engine publishes (the app topic's state, the bridge's modal list).
#
#   uid2name  The game code asks "use your name in player statistics?" while
#             cl_allow_uid2name is -1 and asks again on every frame the menu
#             is not on screen. With a native front end that used to show the
#             bare pause page, and Resume closed it only for the menu to come
#             back on the next frame (owner's report, 2026-11-14: "pressing
#             resume on a live match flashes the game but leaves the pause
#             screen"). Now: the question is the bridge's own dialog
#             (modal "uid2name"); No sets the cvar to 0 and the match is back;
#             then MENU > Resume leaves the menu down. Back answers No too.
#
#   BASEDIR=<Godnotic basedir> STOCKBASE=<basedir with upstream Xonotic data only> \
#       scripts/test-native-menu-live.sh [uid2name]
#
# Environment: ENGINE (default build/bin/xonotic; it also runs the server),
# LIBS (extra LD_LIBRARY_PATH), OUT (keep logs there), CPUS (a taskset list).
# Exit 77: skipped (no engine or data).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
BASEDIR="${BASEDIR:-}"
STOCKBASE="${STOCKBASE:-}"
[ -x "$ENGINE" ] && [ -d "$BASEDIR/data" ] && [ -d "$STOCKBASE/data" ] || { echo "SKIP: set ENGINE, BASEDIR and STOCKBASE"; exit 77; }
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
RUN=()
[ -n "${CPUS:-}" ] && RUN=(taskset -c "$CPUS")
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-nativemenu.XXXXXX")"
cleanup() {
	jobs -p | xargs -r kill 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
fail=0

# one `fe ui <message>` line: the message percent-encoded, as fe/FeArgs.kt sends it
ui() { python3 -c 'import json,sys,urllib.parse; print("fe ui " + urllib.parse.quote(json.dumps(json.loads(sys.argv[1]), separators=(",", ":")), safe=""))' "$1"; }

server() { # name port
	local d="$WORK/$1"
	mkdir -p "$d"
	"${RUN[@]}" "$ENGINE" -dedicated -xonotic -basedir "$STOCKBASE" -userdir "$d" +set port "$2" +set sv_public 0 \
		+set bot_number 2 +set skill 1 +set g_warmup 0 +map stormkeep > "$d/server.log" 2>&1 &
	local n; for n in $(seq 1 600); do grep -q "Server spawned" "$d/server.log" 2> /dev/null && break; sleep 0.1; done
	sleep 2
}

client() { # name port answer(no|back)
	local d="$WORK/$1" answer
	mkdir -p "$d/data"
	if [ "$3" = back ]; then answer=$(ui '{"uib":1,"t":"ev","id":3,"ev":"back"}')
	else answer=$(ui '{"uib":1,"t":"ev","id":3,"ev":"action","node":"uid2name/no","action":"uid2name.no"}'); fi
	{
		echo 'set cl_welcome 0; set touch_join_dialog_auto 0; set developer 1; set fe_echo 1'
		echo 'alias xbeat "fe beat; defer 1 xbeat"'
		echo 'alias xshow "cl_allow_uid2name"'   # a cvar's name alone prints its value when it runs
		ui '{"uib":1,"t":"hello","id":1,"schema":"1.0","renderer":{"name":"test"},"caps":{"widthClass":"expanded"}}' | sed 's/^/alias xhello "/; s/$/"/'
		ui '{"uib":1,"t":"ev","id":4,"ev":"action","node":"pause/actions/resume","action":"resume"}' | sed 's/^/alias xresume "/; s/$/"/'
		echo "alias xanswer \"$answer\""
		echo "connect 127.0.0.1:$2"
		echo 'defer 3 "fe attach 1; xbeat; xhello"'
		# the stock menu initialises in the frames the native UI is away (the device
		# does this on every return from the background): that is when it began to
		# reopen itself for the question
		echo 'defer 28 "fe detach test; defer 0.5 \"fe attach 1; xhello\""'
		echo 'defer 31 "echo MARK asked; xshow; uib_dump"'
		echo 'defer 32 "echo MARK answer; xanswer"'
		echo 'defer 33.5 "echo MARK answered; xshow; uib_dump"'
		echo 'defer 34 "echo MARK pause; menu_showgamemenudialog"'
		echo 'defer 35 "echo MARK resume; xresume"'
		echo 'defer 37 "echo MARK end; quit"'
	} > "$d/data/tour.cfg"
	timeout -k 5 70 "${RUN[@]}" "$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$d" \
		+vid_fullscreen 0 +vid_width 1920 +vid_height 1200 +set vid_touchscreen 1 +set vid_touchscreen_detected 1 \
		+set _touch_assets_ready 1 +set touch_setup_done 1 +set touch_profile_done 1 +set touch_native_frontend 1 \
		+exec tour.cfg > "$d/client.log" 2>&1
}

case_uid2name() {
	local p answer
	for answer in no back; do
		p=$((26700 + RANDOM % 250))
		server "srv_$answer" $p
		client "uid2name_$answer" $p $answer
		kill %1 2> /dev/null; wait 2> /dev/null
		python3 "$ROOT/tests/native-menu/check_uid2name.py" "$WORK/uid2name_$answer/client.log" "$answer" || fail=1
	done
}

cases=("$@")
[ ${#cases[@]} -eq 0 ] && cases=(uid2name)
for c in "${cases[@]}"; do "case_$c"; done
[ $fail = 0 ] && echo "native menu live: all checks passed" || echo "native menu live: FAILED (OUT=<dir> keeps the logs)"
exit $fail
