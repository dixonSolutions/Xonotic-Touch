#!/bin/bash
# In-game HTTP download time (the curl path a server's sv_curl_defaulturl
# uses for a map pack) under simulated links, with Godnotic's parallel
# ranges off (curl_segments 1, curl_maxdownloads 3: stock) and on (defaults).
# docs/NETWORK.md has the results.
#
#   BASEDIR=<basedir> scripts/bench-download.sh [-s MB] [-n RUNS] [CASE...]
#
# CASE is NAME:NETEM:SERVERKBPS with _ for spaces in NETEM, e.g.
#   wifi:delay_20ms_rate_20mbit:0     20 Mbit/s link, 40 ms RTT
#   lossy:delay_50ms_loss_0.5%_rate_50mbit:0
#   capped:delay_25ms_rate_100mbit:1024   a host serving 1 MB/s per connection
# The client downloads a valid pk3 of -s MB (incompressible, stored) with
# `curl --pak` from tests/net/range_http.py, and the file it ends up with is
# compared with the original.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
BASEDIR="${BASEDIR:?set BASEDIR}"
MB=40
RUNS=1
while getopts "s:n:" o; do case $o in s) MB=$OPTARG ;; n) RUNS=$OPTARG ;; *) exit 1 ;; esac; done
shift $((OPTIND - 1))
[ $# -gt 0 ] || set -- lan::0 wifi:delay_20ms_rate_20mbit:0 mobile:delay_40ms_loss_0.5%_rate_30mbit:0 lossy:delay_50ms_loss_1%_rate_100mbit:0 capped:delay_25ms_rate_100mbit:1024

if [ -z "${NET_TEST_INSIDE:-}" ]; then
	unshare -rn true 2> /dev/null || { echo "SKIP: no unprivileged network namespaces"; exit 77; }
	exec unshare -rn env NET_TEST_INSIDE=1 bash "$0" -s "$MB" -n "$RUNS" "$@"
fi
ip link set lo up
ip link set lo mtu 1500   # Ethernet-sized packets, as on a real link
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-dl.XXXXXX")"
trap 'jobs -p | xargs -r kill 2> /dev/null; rm -rf "$WORK"' EXIT

mkdir -p "$WORK/www"
python3 - "$WORK/www/zzbenchmap.pk3" "$MB" <<'EOF'
import os, sys, zipfile
path, mb = sys.argv[1], float(sys.argv[2])
with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
    left, i = int(mb * 1048576), 0
    while left > 0:
        n = min(left, 4 << 20)
        z.writestr("benchdata/blob%03d.bin" % i, os.urandom(n)); left -= n; i += 1
EOF
want=$(sha256sum "$WORK/www/zzbenchmap.pk3" | cut -d' ' -f1)
size=$(stat -c %s "$WORK/www/zzbenchmap.pk3")

run_one() { # netem server_kbps args -> prints seconds or FAIL
	local cfg=$1 port=8123 u="$WORK/u" t0 t1 line got
	rm -rf "$u"; mkdir -p "$u/data"
	python3 "$ROOT/tests/net/range_http.py" "$WORK/www" $port "$2" 2> "$WORK/http.log" & local hp=$!
	sleep 0.5
	echo "curl --pak http://127.0.0.1:$port/zzbenchmap.pk3" > "$u/data/dl.cfg"
	t0=""; t1=""
	# shellcheck disable=SC2086
	timeout 600 "$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$u" +vid_fullscreen 0 +vid_width 320 +vid_height 240 \
		+set _touch_assets_ready 1 +set developer 1 +set curl_enabled 1 +set cl_curl_enabled 1 $cfg +exec dl.cfg 2>&1 |
	while IFS= read -r line; do
		case $line in
			*"Downloading http"*) echo "START $(date +%s.%N)" ;;
			*"Download of "*) echo "END $(date +%s.%N) $line"; pkill -f "userdir $u" ; break ;;
		esac
	done > "$WORK/times"
	kill $hp 2> /dev/null; wait $hp 2> /dev/null
	t0=$(awk '/^START/{print $2; exit}' "$WORK/times"); t1=$(awk '/^END/{print $2; exit}' "$WORK/times")
	got=$(sha256sum "$u/data/dlcache/zzbenchmap.pk3" 2> /dev/null | cut -d' ' -f1)
	if [ -n "$t0" ] && [ -n "$t1" ] && [ "$got" = "$want" ] && grep -q ": OK" "$WORK/times"; then
		awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b - a}'
	else
		echo "FAIL($(grep -o 'Download of.*' "$WORK/times" | head -1))"
	fi
}

printf '| link | per-connection cap | %s MB, stock (1 connection) | %s MB, Godnotic (curl_segments 4) |\n|---|---|---|---|\n' "$MB" "$MB"
for c in "$@"; do
	IFS=: read -r name netem kbps <<< "$c"
	tc qdisc del dev lo root 2> /dev/null
	[ -n "$netem" ] && tc qdisc add dev lo root netem ${netem//_/ }
	a=(); b=()
	for r in $(seq 1 "$RUNS"); do
		a+=("$(run_one "+set curl_segments 1 +set curl_maxdownloads 3" "$kbps")")
		b+=("$(run_one "" "$kbps")")
	done
	printf '| %s (%s) | %s | %s s | %s s |\n' "$name" "${netem//_/ }" "$( [ "$kbps" = 0 ] && echo none || echo "$kbps KiB/s")" "${a[*]}" "${b[*]}"
done
echo "(file $size bytes; each figure: from the start of the transfer to the pack mounted, sha256 checked)"
