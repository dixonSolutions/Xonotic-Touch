#!/bin/bash
# How long a join takes when the server leaves the first N connect requests
# unanswered (tests/net/drop_connects.py), with the failed-join back-off
# (net_connect_backoff, touch_joinfail.c) on and off. The back-off waits out
# a stock server's 5 s connect flood block after four challenges answered
# with a connect and no accept; it must not make lost connects (up to three
# in a row) slower than stock DarkPlaces' one-second retries, and it must
# get through the flood block ("flood": a second device behind the same
# address joined just before), which stock never does.
#
#   BASEDIR=<basedir> scripts/test-net-connect.sh [N...]     (default 0 1 2 3 4 flood)
# ENGINE (default build/bin/xonotic), LIBS.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
BASEDIR="${BASEDIR:?set BASEDIR}"
if [ -z "${NET_TEST_INSIDE:-}" ]; then
	unshare -rn true 2> /dev/null || { echo "SKIP: no unprivileged network namespaces"; exit 77; }
	exec unshare -rn env NET_TEST_INSIDE=1 bash "$0" "$@"
fi
ip link set lo up
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-connect.XXXXXX")"
trap 'jobs -p | xargs -r kill 2> /dev/null; rm -rf "$WORK"' EXIT
fail=0
[ $# -gt 0 ] || set -- 0 1 2 3 4 flood

join_time() { # drops backoff -> seconds from the first getchallenge to the accept, or "none"
	local n=$1 b=$2 u="$WORK/u-$1-$2"
	mkdir -p "$u/data"
	printf 'set net_connect_backoff %s\nconnect 127.0.0.1:26400\ndefer 25 quit\n' "$b" > "$u/data/c.cfg"
	python3 "$ROOT/tests/net/drop_connects.py" 26400 "$n" 30 > "$WORK/fake-$n-$b.log" &
	local fp=$!
	sleep 0.3
	timeout 40 "$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$u" +vid_fullscreen 0 +vid_width 320 +vid_height 200 \
		+set _touch_assets_ready 1 +exec c.cfg > "$WORK/client-$n-$b.log" 2>&1 &
	local cp=$!
	wait $fp
	kill $cp 2> /dev/null; wait $cp 2> /dev/null
	awk '/accepted/{print $1; f=1} END{if (!f) print "none"}' "$WORK/fake-$n-$b.log"
}

printf '| connects dropped | stock timing (net_connect_backoff 0) | Godnotic (net_connect_backoff 1) |\n|---|---|---|\n'
for n in "$@"; do
	a=$(join_time "$n" 0)
	b=$(join_time "$n" 1)
	printf '| %s | %s s | %s s |\n' "$n" "$a" "$b"
	# up to 3 drops the back-off must not add anything (half a second's jitter allowed)
	if [ "$n" != flood ] && [ "$n" -le 3 ] && [ "$a" != none ] && [ "$b" != none ] && awk -v a="$a" -v b="$b" 'BEGIN{exit !(b > a + 0.5)}'; then
		echo "FAIL $n dropped: the back-off made the join slower ($b s against $a s)"; fail=1
	fi
	[ "$b" = none ] && { echo "FAIL $n dropped: never accepted with the back-off"; fail=1; }
	# the flood block: stock never gets in, the back-off does
	[ "$n" = flood ] && [ "$a" != none ] && echo "note flood: the stock timing got in too ($a s): the fake block did not hold"
done
[ $fail = 0 ] && echo "connect timing: ok" || echo "connect timing: FAILED"
exit $fail
