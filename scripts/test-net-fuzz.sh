#!/bin/bash
# Hostile peers against the windowed reliable channel, under AddressSanitizer
# and UBSan (docs/NETWORK.md): tests/net/hostile_peer.py offers bogus and
# giant windows ("rwin"), acknowledges sequences never sent, sends fragments
# far outside the window, out of order, duplicated, truncated, with lying
# length fields; a dedicated server takes it as a client, a client takes it
# as a server. Passes when neither engine crashes or reports a memory error,
# the server still answers a query afterwards, and memory stays bounded.
#
#   BASEDIR=<Godnotic basedir> scripts/test-net-fuzz.sh
#
# ASAN_DIR keeps the sanitizer build between runs (default: a temp dir, built
# from engine/darkplaces in about three minutes). SECONDS_EACH (default 60).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASEDIR="${BASEDIR:?set BASEDIR to a Godnotic basedir}"
SECS="${SECONDS_EACH:-60}"
MAP="${MAP:-stormkeep}"

if [ -z "${NET_TEST_INSIDE:-}" ]; then
	command -v unshare > /dev/null || { echo "SKIP: no unshare"; exit 77; }
	unshare -rn true 2> /dev/null || { echo "SKIP: no unprivileged network namespaces"; exit 77; }
	exec unshare -rn env NET_TEST_INSIDE=1 bash "$0" "$@"
fi
ip link set lo up
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
# a small quarantine, or freed connections count as growth
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=1:quarantine_size_mb=8" UBSAN_OPTIONS="print_stacktrace=1"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-netfuzz.XXXXXX")"
trap 'jobs -p | xargs -r kill 2> /dev/null; [ -n "${OUT:-}" ] && mkdir -p "$OUT" && cp "$WORK"/*.log "$OUT/" 2> /dev/null; rm -rf "$WORK"' EXIT
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

ASAN_DIR="${ASAN_DIR:-$WORK/asan}"
if [ ! -x "$ASAN_DIR/darkplaces-dedicated" ] || [ ! -x "$ASAN_DIR/darkplaces-sdl" ]; then
	echo "building the engine with -fsanitize=address,undefined in $ASAN_DIR"
	mkdir -p "$ASAN_DIR"
	rsync -a --delete --exclude 'build-obj' --exclude 'darkplaces-*' "$ROOT/engine/darkplaces/" "$ASAN_DIR/"
	cc="cc -fsanitize=address,undefined -fno-omit-frame-pointer -g"
	(cd "$ASAN_DIR" && PATH="/usr/bin:$PATH" make sv-release DP_SSE=0 -j"$(nproc)" CC="$cc" STRIP=: DP_LINK_JPEG=dlopen > build-sv.log 2>&1 \
		&& PATH="/usr/bin:$PATH" make sdl-release DP_SSE=0 -j"$(nproc)" CC="$cc" STRIP=: DP_LINK_JPEG=dlopen > build-sdl.log 2>&1) \
		|| { echo "FAIL sanitizer build (see $ASAN_DIR/build-*.log)"; exit 1; }
fi

# sanitizer reports from the code under test: an ASan error anywhere, or a
# UBSan one whose first frame is in the network code; other UBSan reports
# (upstream's PRVM_Prog_Load, say) are listed as already there
report() { # log name
	local log=$1 name=$2
	if grep -q "AddressSanitizer" "$log"; then
		bad "$name: AddressSanitizer:"; grep -A20 -m1 "AddressSanitizer" "$log"
	elif grep -A1 "runtime error" "$log" | grep -q "touch_net\|touch_curl\|touch_lan\|netconn\.c\|libcurl\.c"; then
		bad "$name: UBSan in the network code:"; grep -B1 -A3 "runtime error" "$log" | grep -A4 "touch_net\|touch_curl\|touch_lan\|netconn\|libcurl" | head -20
	else
		ok "$name: no sanitizer report from the network code"
		grep -o "[a-z_0-9]*\.c:[0-9]*:[0-9]*: runtime error: [^0-9]*" "$log" | sort -u | sed 's/^/     already there: /'
	fi
}

rss_kb() { awk '/VmRSS/{print $2}' "/proc/$1/status" 2> /dev/null || echo 0; }

query() { # port -> prints the infoResponse or nothing
	python3 - "$1" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.sendto(b"\xff\xff\xff\xffgetinfo x", ("127.0.0.1", int(sys.argv[1])))
try:
    print(s.recvfrom(4096)[0][4:40])
except OSError:
    pass
EOF
}

# --- a hostile client against a server
port=26300
mkdir -p "$WORK/sv"
"$ASAN_DIR/darkplaces-dedicated" -dedicated -xonotic -basedir "$BASEDIR" -userdir "$WORK/sv" +set port $port +set sv_public 0 \
	+set maxplayers 8 +set net_connecttimeout 3 +set bot_number 2 +map "$MAP" > "$WORK/server.log" 2>&1 & svpid=$!
for n in $(seq 1 600); do grep -q "Server spawned" "$WORK/server.log" 2> /dev/null && break; sleep 0.1; done
sleep 2
rss0=$(rss_kb $svpid)
python3 "$ROOT/tests/net/hostile_peer.py" client 127.0.0.1 $port "$SECS" 1 > "$WORK/hostile-client.log" 2>&1
sleep 4
rss1=$(rss_kb $svpid)
tail -1 "$WORK/hostile-client.log"
if kill -0 $svpid 2> /dev/null; then ok "server alive after $SECS s of hostile packets"; else bad "server died (see $WORK/server.log)"; fi
if [ -n "$(query $port)" ]; then ok "server still answers getinfo"; else bad "server does not answer getinfo"; fi
report "$WORK/server.log" server
echo "     server RSS ${rss0} kB before, ${rss1} kB after"
[ "${rss1:-0}" -lt $(( ${rss0:-0} + 65536 )) ] && ok "server memory bounded (+$(( (${rss1:-0} - ${rss0:-0}) / 1024 )) MB)" || bad "server grew by $(( (${rss1:-0} - ${rss0:-0}) / 1024 )) MB"
kill $svpid 2> /dev/null; wait $svpid 2> /dev/null

# --- a hostile server against a client that keeps reconnecting
port=26301
mkdir -p "$WORK/cl/data"
cat > "$WORK/cl/data/net-fuzz.cfg" <<EOF
alias _nf_again "connect 127.0.0.1:$port; defer 2 _nf_again"
_nf_again
defer $((SECS + 5)) quit
EOF
python3 "$ROOT/tests/net/hostile_peer.py" server $port "$((SECS + 3))" 2 > "$WORK/hostile-server.log" 2>&1 & hspid=$!
sleep 0.5
timeout $((SECS + 120)) "$ASAN_DIR/darkplaces-sdl" -xonotic -basedir "$BASEDIR" -userdir "$WORK/cl" +vid_fullscreen 0 +vid_width 320 +vid_height 240 \
	+set _touch_assets_ready 1 +set touch_profile_done 1 +exec net-fuzz.cfg > "$WORK/client.log" 2>&1
rc=$?
wait $hspid
tail -1 "$WORK/hostile-server.log"
[ $rc = 0 ] && ok "client quit normally after $SECS s of a hostile server" || bad "client exit status $rc (see $WORK/client.log)"
report "$WORK/client.log" client
echo "     client: $(grep -c "Connection established" "$WORK/client.log") connections, $(grep -c "Illegible\|Host_Error\|disconnect" "$WORK/client.log") dropped"

[ $fail = 0 ] && echo "all net fuzz tests passed" || echo "some net fuzz tests FAILED"
exit $fail
