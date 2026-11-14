#!/bin/bash
# The touch HUD mode on a server that runs its own game code (the engine-HUD
# path every public server takes): a dedicated server with STOCK Xonotic data
# and bots, one offscreen Godnotic client. The client joins as every player
# does (observer first), joins the game, dies, and goes back to spectating;
# `touch_hud_state` is sampled all the way. It passes when, in Deathmatch and
# in CTF (a team game: the join goes through a team):
#   - a fresh join is "spectating" (the join control is offered),
#   - after joining the mode is "playing" at every sample, never a spectator
#     mode (the bug report: "I just see follow, even though I am playing"),
#   - a dead player is "dead", not a spectator,
#   - back in the spectators it is "spectating" again.
#
#   BASEDIR=<Godnotic basedir> STOCKBASE=<basedir with upstream Xonotic data only> \
#       scripts/test-touch-hudmode-online.sh [dm] [ctf]
#
# Environment: ENGINE (default engine/darkplaces/darkplaces-sdl; it also runs
# the server, -dedicated), LIBS (extra LD_LIBRARY_PATH, libjpeg.so.62), OUT
# (keep the logs there). Exit 77: skipped (no engine or data).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
BASEDIR="${BASEDIR:-}"
STOCKBASE="${STOCKBASE:-}"
[ -x "$ENGINE" ] && [ -d "$BASEDIR/data" ] && [ -d "$STOCKBASE/data" ] || { echo "SKIP: set ENGINE, BASEDIR and STOCKBASE"; exit 77; }
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-hudmode.XXXXXX")"
cleanup() {
	jobs -p | xargs -r kill 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# modes sampled between two PHASE markers of the client log, one per line
phase() { # log name
	awk -v p="PHASE $2" '{sub(/[ \r]+$/, "")} $0 ~ /^PHASE /{on = ($0 == p)} on && /^TOUCHHUD mode=/{sub(/^TOUCHHUD mode=/, ""); print $1}' "$1" | sort | uniq -c | awk '{printf "%s x%s ", $2, $1}'
}
expect() { # case log phase mode
	local got; got="$(phase "$2" "$3")"
	if [ -n "$got" ] && [ -z "$(echo "$got" | sed "s/$4 x[0-9]* //g")" ]; then ok "$1: $3 -> $4 ($got)"; else bad "$1: $3 should be $4 only, sampled: ${got:-nothing} (log: $2)"; fi
}

run_case() { # gametype map
	local gt=$1 map=$2 port=$((26600 + RANDOM % 300)) d="$WORK/$1" n
	mkdir -p "$d/sv/data" "$d/cl/data"
	"$ENGINE" -dedicated -xonotic -basedir "$STOCKBASE" -userdir "$d/sv" +set port $port +set sv_public 0 \
		+set bot_number 3 +set skill 3 +set g_maplist "$map" +set g_warmup 0 +set g_dm 0 +set "g_$gt" 1 \
		+set g_balance_teams 0 +set timelimit_override 20 +map "$map" > "$d/server.log" 2>&1 &
	for n in $(seq 1 600); do grep -q "Server spawned" "$d/server.log" 2> /dev/null && break; sleep 0.1; done
	sleep 2
	printf 'seta touch_profile_done 1\nseta _cl_name "hudtest"\nseta cl_allow_uid2name 0\n' > "$d/cl/data/autoexec.cfg"
	# one script, on the client's own clock: sample every second, act at fixed steps
	{
		echo "connect 127.0.0.1:$port"
		for n in $(seq 1 110); do echo "defer $n \"touch_hud_state\""; done
		echo 'defer 40 "echo PHASE spectator"'
		echo 'defer 46 "echo PHASE joining; togglemenu 0; cmd selectteam auto; cmd join"'
		echo 'defer 53 "echo PHASE playing"'
		echo 'defer 68 "echo PHASE dying; kill"'
		echo 'defer 71 "echo PHASE dead"'
		echo 'defer 74 "echo PHASE leaving; cmd spectate"'
		echo 'defer 80 "echo PHASE spectator2"'
		echo 'defer 88 "echo PHASE end; quit"'
	} > "$d/cl/data/hud.cfg"
	timeout 200 "$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$d/cl" +vid_fullscreen 0 +vid_width 640 +vid_height 360 \
		+set _touch_assets_ready 1 +exec hud.cfg > "$d/client.log" 2>&1
	kill %1 2> /dev/null; wait 2> /dev/null
	if ! grep -aq "is now playing" "$d/client.log"; then bad "$gt: the client never joined the game (log: $d/client.log)"; return; fi
	if [ "$gt" = ctf ]; then
		if grep -aq "is now playing on the" "$d/client.log"; then ok "$gt: joined a team"; else bad "$gt: no team join in the log"; fi
	fi
	if grep -aq "TOUCHHUD .* ours=1" "$d/client.log"; then bad "$gt: the client ran Godnotic's CSQC, not the server's (not the public-server path)"; fi
	expect "$gt" "$d/client.log" spectator spectating
	expect "$gt" "$d/client.log" playing playing
	expect "$gt" "$d/client.log" dead dead
	expect "$gt" "$d/client.log" spectator2 spectating
}

cases="${*:-dm ctf}"
for c in $cases; do
	case $c in
	dm) run_case dm stormkeep ;;
	ctf) run_case ctf implosion ;;
	*) echo "unknown case $c"; exit 2 ;;
	esac
done
[ $fail = 0 ] && echo "touch hud mode online: all checks passed"
exit $fail
