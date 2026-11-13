#!/bin/bash
# The windowed reliable channel and the fast signon end to end, headless, on
# one machine (docs/NETWORK.md): a dedicated server with 4 bots and one
# offscreen client on 127.0.0.1 inside an unprivileged network namespace
# (unshare -rn), tc netem on its loopback for latency, loss, reordering and
# duplication. Each case joins, plays (cmd join) for DURATION seconds, and the
# server changes map halfway with the client connected (the most reliable
# data at once: every connected client gets a new signon). It passes when:
#   - the client reached the game on both maps (server log: connected twice,
#     client trace: signon 4 twice when it is a Godnotic client),
#   - neither log shows a protocol error (ERRORS below) and the client was
#     not dropped for anything but its own quit,
#   - the window is what the peers negotiate: 32 between Godnotic peers, 0
#     (stock stop-and-wait) as soon as one side is stock; a packet capture
#     shows the connect/accept strings each side sent.
#
#   BASEDIR=<Godnotic basedir> scripts/test-net.sh [CASE...]
#
# CASE: gn-0 gn-100 gn-200 gn-100-loss2 gn-100-loss10 gn-60-reorder
# gn-60-dup (Godnotic client and server, RTT in ms), gn-off-100 (both with
# net_reliable_window 0 net_signon_fast 0), gn-to-stock (Godnotic client,
# stock server), stock-to-gn (stock client, Godnotic server), stock-to-stock.
# Default: all of them.
#
# Environment:
#   ENGINE        Godnotic client (default build/bin/xonotic)
#   SERVER        Godnotic dedicated server (default engine/darkplaces/darkplaces-dedicated)
#   STOCK_ENGINE  stock DarkPlaces client, STOCK_SERVER stock dedicated server
#                 (built from upstream/vendor, see docs/NETWORK.md); the
#                 stock cases are skipped without them
#   STOCKBASE     basedir with upstream Xonotic data only (no Godnotic
#                 pk3dir), used by every interop case on both sides so the
#                 game code is the same on both
#   DURATION      seconds in the game per case (default 60)
#   LIBS          extra LD_LIBRARY_PATH (libjpeg.so.62)
#   OUT           keep the logs and captures here
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
SERVER="${SERVER:-$ROOT/engine/darkplaces/darkplaces-dedicated}"
BASEDIR="${BASEDIR:?set BASEDIR to a Godnotic basedir (data/*.pk3 plus the Godnotic pk3dir)}"
STOCKBASE="${STOCKBASE:-}"
DURATION="${DURATION:-60}"
MAP="${MAP:-stormkeep}"
ERRORS='Illegible|Host_Error|SZ_GetSpace|overflow|Overflow|too big for message buffer|reliable queue full|faster than they were read|Buffer overflow|timed out|Segmentation|AddressSanitizer|runtime error|desync|CL_ParseServerMessage: bad|unknown command char'

if [ -z "${NET_TEST_INSIDE:-}" ]; then
	command -v unshare > /dev/null || { echo "SKIP: no unshare"; exit 77; }
	command -v tc > /dev/null || { echo "SKIP: no tc (iproute2)"; exit 77; }
	if ! unshare -rn true 2> /dev/null; then
		echo "SKIP: unprivileged user/network namespaces are not allowed here"
		exit 77
	fi
	exec unshare -rn env NET_TEST_INSIDE=1 bash "$0" "$@"
fi

export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-net.XXXXXX")"
cleanup() {
	jobs -p | xargs -r kill 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
ip link set lo up

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

netem() { # options...; none = plain loopback
	tc qdisc del dev lo root 2> /dev/null
	[ $# -gt 0 ] && tc qdisc add dev lo root netem "$@"
}

# run_case NAME CLIENT CLIENTBASE SERVER SERVERBASE "CLIENT ARGS" "SERVER ARGS" EXPECTED_WINDOW
run_case() {
	local name=$1 cl=$2 cb=$3 sv=$4 sb=$5 cargs=$6 sargs=$7 want=$8 port=26200
	local cu="$WORK/$name/cl" su="$WORK/$name/sv" clog="$WORK/$name/client.log" slog="$WORK/$name/server.log"
	local half=$((DURATION / 2)) svpid clpid cappid n
	mkdir -p "$cu/data" "$su/data"
	# server: change map halfway with the client in the game, quit after
	cat > "$su/data/net-test.cfg" <<EOF
defer $((half + 15)) "changelevel $MAP"
defer $((DURATION + 60)) quit
EOF
	# client: join the game once in, quit after DURATION (+ the join)
	cat > "$cu/data/net-test.cfg" <<EOF
connect 127.0.0.1:$port
defer 12 "togglemenu 0; cmd join"
defer $((half + 25)) "togglemenu 0; cmd join"
defer $((DURATION + 20)) quit
EOF
	printf 'seta touch_profile_done 1\nseta _cl_name "Bench"\nseta cl_allow_uid2name 0\n' > "$cu/data/autoexec.cfg"
	# -Z root: in the user namespace tcpdump cannot switch to its own user
	tcpdump -Z root -i lo -U -s 0 -w - "udp port $port" > "$WORK/$name/capture.pcap" 2> /dev/null & cappid=$!
	# shellcheck disable=SC2086
	"$sv" -dedicated -xonotic -basedir "$sb" -userdir "$su" +set port $port +set sv_public 0 \
		+set bot_number 4 +set minplayers 0 +set g_maplist "$MAP" $sargs +map "$MAP" +exec net-test.cfg > "$slog" 2>&1 & svpid=$!
	for n in $(seq 1 300); do grep -q "Server spawned\|SpawnServer" "$slog" 2> /dev/null && break; sleep 0.1; done
	sleep 1
	# shellcheck disable=SC2086
	timeout $((DURATION + 90)) "$cl" -xonotic -basedir "$cb" -userdir "$cu" +vid_fullscreen 0 +vid_width 640 +vid_height 360 \
		+set _touch_assets_ready 1 $cargs +exec net-test.cfg > "$clog" 2>&1 & clpid=$!
	wait $clpid
	kill $svpid 2> /dev/null; wait $svpid 2> /dev/null
	kill $cappid 2> /dev/null; wait $cappid 2> /dev/null

	local joins errs window acc
	joins=$(grep -c "^Bench connected" "$slog")
	errs=$(grep -hE "$ERRORS" "$clog" "$slog" | sort | uniq -c | head -5)
	if [ "$joins" -ge 2 ]; then ok "$name: joined both maps ($joins connects)"; else bad "$name: joined $joins time(s), want 2 (logs: $clog $slog)"; fi
	if grep 'Client "Bench" dropped' "$slog" | grep -vq "Disconnect by user"; then
		bad "$name: client dropped: $(grep 'Client "Bench" dropped' "$slog" | head -2 | tr '\n' ' ')"
	fi
	if [ -n "$errs" ]; then bad "$name: errors in the logs:"; echo "$errs" | sed 's/^/       /'; else ok "$name: no protocol errors"; fi
	if grep -q "NETTRACE cl .* est window=" "$clog"; then
		window=$(grep -o "est window=[0-9]*" "$clog" | head -1 | cut -d= -f2)
		[ "$window" = "$want" ] && ok "$name: client negotiated window $window" || bad "$name: client window $window, want $want"
		n=$(grep -c "NETTRACE cl .* signon 4" "$clog")
		[ "$n" -ge 2 ] && ok "$name: client trace reached signon 4 on both maps" || bad "$name: client trace reached signon 4 $n time(s)"
	fi
	# what went over the wire in the handshake
	if [ -s "$WORK/$name/capture.pcap" ]; then
		acc=$(tcpdump -Z root -r "$WORK/$name/capture.pcap" -A 2> /dev/null | grep -ao 'accept[\\a-z0-9]*\|rwin\\[0-9]*' | sort | uniq -c | tr -s ' \n' ' ;')
		echo "     $name handshake on the wire: ${acc:-no rwin/accept strings captured}"
	fi
}

GN_TRACE="+set touch_net_trace 1"
have_stock() { [ -n "${STOCK_ENGINE:-}" ] && [ -n "${STOCK_SERVER:-}" ] && [ -n "$STOCKBASE" ]; }

run() {
	case $1 in
		gn-0) netem; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" "$GN_TRACE" "" 32 ;;
		gn-100) netem delay 50ms; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" "$GN_TRACE" "" 32 ;;
		gn-200) netem delay 100ms; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" "$GN_TRACE" "" 32 ;;
		gn-100-loss2) netem delay 50ms loss 2%; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" "$GN_TRACE" "" 32 ;;
		gn-100-loss10) netem delay 50ms loss 10%; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" "$GN_TRACE" "" 32 ;;
		gn-60-reorder) netem delay 30ms 10ms reorder 25% 50%; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" "$GN_TRACE" "" 32 ;;
		gn-60-dup) netem delay 30ms duplicate 10%; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" "$GN_TRACE" "" 32 ;;
		gn-off-100) netem delay 50ms; run_case "$1" "$ENGINE" "$BASEDIR" "$SERVER" "$BASEDIR" \
			"$GN_TRACE +set net_reliable_window 0 +set net_signon_fast 0" "+set net_reliable_window 0 +set net_signon_fast 0" 0 ;;
		gn-to-stock|stock-to-gn|stock-to-stock)
			if ! have_stock; then echo "skip $1: set STOCK_ENGINE, STOCK_SERVER and STOCKBASE"; return; fi
			netem delay 50ms
			case $1 in
				gn-to-stock) run_case "$1" "$ENGINE" "$STOCKBASE" "$STOCK_SERVER" "$STOCKBASE" "$GN_TRACE" "" 0 ;;
				stock-to-gn) run_case "$1" "$STOCK_ENGINE" "$STOCKBASE" "$SERVER" "$STOCKBASE" "" "+set touch_net_trace 1" 0 ;;
				stock-to-stock) run_case "$1" "$STOCK_ENGINE" "$STOCKBASE" "$STOCK_SERVER" "$STOCKBASE" "" "" 0 ;;
			esac ;;
		*) bad "unknown case $1" ;;
	esac
}

[ $# -gt 0 ] || set -- gn-0 gn-100 gn-200 gn-100-loss2 gn-100-loss10 gn-60-reorder gn-60-dup gn-off-100 gn-to-stock stock-to-gn stock-to-stock
for c in "$@"; do run "$c"; done
netem
[ $fail = 0 ] && echo "all net tests passed" || echo "some net tests FAILED"
exit $fail
