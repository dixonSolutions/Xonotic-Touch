#!/bin/bash
# A join that fails says why, and the client does not lock itself out
# (engine/darkplaces/touch_joinfail.c, docs/NETWORK.md "When a join fails").
#   1. tests/joinfail/joinfail_test.c: the classification and the wording
#      (no engine).
#   2. With ENGINE and BASEDIR, a dedicated server on 127.0.0.1 and offscreen
#      clients:
#      two-at-once     two clients behind one address join in the same second.
#                      A stock server ignores the second one's connect for 5 s
#                      and every retry renews that; both must get in (the
#                      second after its back-off).
#      no-backoff      the same with net_connect_backoff 0: the second fails
#                      and is told the server answered but did not let it in,
#                      naming it "<name> (<address>)": its hostname, colour
#                      codes out (touch_joinname.c asks the server for it).
#      no-answer       a port nobody listens on: "No answer from ...".
#      full            a server with one slot taken: the server's own words.
#
#   BASEDIR=<Godnotic basedir> scripts/test-joinfail.sh
# Environment: ENGINE (default engine/darkplaces/darkplaces-sdl; also the
# server, -dedicated), LIBS (extra LD_LIBRARY_PATH), OUT (keep the logs).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-joinfail.XXXXXX")"
cleanup() {
	jobs -p | xargs -r kill 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I"$ROOT/engine/darkplaces" "$ROOT/tests/joinfail/joinfail_test.c" -o "$WORK/logic-test" || exit 1
"$WORK/logic-test" || exit 1
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
if [ ! -x "$ENGINE" ] || [ ! -d "${BASEDIR:-/nonexistent}/data" ]; then
	echo "SKIP: end to end (set ENGINE and BASEDIR)"
	exit 0
fi
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

server() { # dir port extra...
	local d=$1 port=$2 n; shift 2
	mkdir -p "$d/data"
	"$ENGINE" -dedicated -xonotic -basedir "$BASEDIR" -userdir "$d" +set port "$port" +set sv_public 0 +set g_maplist stormkeep "$@" +map stormkeep > "$d.log" 2>&1 &
	for n in $(seq 1 600); do grep -q "Server spawned" "$d.log" 2> /dev/null && break; sleep 0.1; done
	sleep 1
}
client() { # dir address seconds extra...  (background)
	local d=$1 addr=$2 secs=$3; shift 3
	mkdir -p "$d/data"
	printf 'seta touch_profile_done 1\nseta _cl_name "%s"\nseta cl_allow_uid2name 0\n' "$(basename "$d")" > "$d/data/autoexec.cfg"
	printf 'connect %s\ndefer %d quit\n' "$addr" "$secs" > "$d/data/join.cfg"
	timeout $((secs + 60)) "$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$d" +vid_fullscreen 0 +vid_width 320 +vid_height 180 \
		+set _touch_assets_ready 1 +set touch_joinfail_dialog 0 "$@" +exec join.cfg > "$d.log" 2>&1 &
}
port=$((26800 + RANDOM % 400))

# --- two at once, back-off on
d="$WORK/two"; mkdir -p "$d"
server "$d/sv" $port
client "$d/a" 127.0.0.1:$port 26; pa=$!
client "$d/b" 127.0.0.1:$port 26; pb=$!
wait $pa $pb
kill %1 2> /dev/null; wait 2> /dev/null
n=$(cat "$d/a.log" "$d/b.log" | grep -ac "Connection established")
if [ "$n" = 2 ]; then ok "two at once: both clients got in"; else bad "two at once: $n of 2 clients got in (logs: $d)"; fi
if cat "$d/a.log" "$d/b.log" | grep -aq "Join failed"; then bad "two at once: a join was reported as failed"; fi
if cat "$d/a.log" "$d/b.log" | grep -aq "waiting .* for its connect flood block"; then ok "two at once: the second one backed off instead of renewing the block"; else echo "note two at once: the server let both in without a back-off this time"; fi

# --- the same without the back-off: the stock behaviour, and what the player is told
port=$((port + 1)); d="$WORK/nobackoff"; mkdir -p "$d"
server "$d/sv" $port +set hostname "^2Busy^x0f0 test"
client "$d/a" 127.0.0.1:$port 22 +set net_connect_backoff 0; pa=$!
client "$d/b" 127.0.0.1:$port 22 +set net_connect_backoff 0; pb=$!
wait $pa $pb
kill %1 2> /dev/null; wait 2> /dev/null
if cat "$d/a.log" "$d/b.log" | grep -aq "Join failed (busy): Busy test (127.0.0.1:$port) answered but did not let you in"; then
	ok "no back-off: the blocked client is told the server (by name and address) answered but did not let it in"
else
	bad "no back-off: no 'busy' report (logs: $d)"
fi

# --- nobody there
port=$((port + 1)); d="$WORK/noanswer"; mkdir -p "$d"
client "$d/a" 127.0.0.1:$port 16; wait $!
if grep -aq "Join failed (timeout): No answer from 127.0.0.1:$port" "$d/a.log"; then ok "no answer: reported as no answer from the address"; else bad "no answer: no 'timeout' report (log: $d/a.log)"; fi

# --- refused: the server is full
port=$((port + 1)); d="$WORK/full"; mkdir -p "$d"
server "$d/sv" $port +maxplayers 1
client "$d/a" 127.0.0.1:$port 22; pa=$!
sleep 9
client "$d/b" 127.0.0.1:$port 8; pb=$!
wait $pa $pb
kill %1 2> /dev/null; wait 2> /dev/null
if grep -a "Join failed (refused)" "$d/b.log" | grep -aqi "The server says: .*full"; then ok "full: refused with the server's words ($(grep -a 'Join failed' "$d/b.log" | sed 's/.*The server says: //; s/\x1b.*//' | head -1))"; else bad "full: no 'refused' report with the server's reason (log: $d/b.log)"; fi

[ $fail = 0 ] && echo "joinfail: all checks passed"
exit $fail
