#!/usr/bin/env bash
# Join time over a simulated network, with Godnotic's network features off
# (stock DarkPlaces behaviour: net_reliable_window 0, net_signon_fast 0 on
# both sides) and on (the defaults), using bench-load.sh's join scenario and
# a packet trace of each join (touch_net_trace 1, net-trace-report.py).
#
#   BASEDIR=... [SV_ENGINE=...] scripts/bench-net.sh [-n RUNS] [-m MAP] [-o OUTDIR] [-c CONFIGS] [POINT...]
#
# POINT: one-way delay in ms, optionally :LOSS (percent) and :NETEM options
# with _ for spaces, e.g. 50, 50:2, 50:0:reorder_25%_50%. RTT = 2 x delay.
# Default points: 0 25 50 100 50:2.
# CONFIGS: comma list of stock, godnotic (the defaults) and wN (defaults with
# net_reliable_window N on both sides); default stock,godnotic.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNS=3
MAP=stormkeep
OUT=""
CONFIGS=stock,godnotic
while getopts "n:m:o:c:h" opt; do
	case $opt in
		n) RUNS=$OPTARG ;;
		m) MAP=$OPTARG ;;
		o) OUT=$OPTARG ;;
		c) CONFIGS=$OPTARG ;;
		*) sed -n '2,15p' "$0"; exit 1 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || set -- 0 25 50 100 50:2
: "${BASEDIR:?set BASEDIR to a game data dir (holding data/)}"
OUT="${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/bench-net.XXXXXX")}"
mkdir -p "$OUT"
export BASEDIR

TABLE="$OUT/results.md"
printf '| config | RTT ms | loss | netem | join ms (median) | connect | serverinfo | precache | signon2 | spawn | begin | wait/RTT |\n|---|---|---|---|---|---|---|---|---|---|---|---|\n' > "$TABLE"

for point in "$@"; do
	IFS=: read -r ms loss netem <<< "$point"
	loss=${loss:-0}; netem=${netem:-}
	netem=${netem//_/ }
	rtt=$((ms * 2))
	for cfg in ${CONFIGS//,/ }; do
		case $cfg in
			stock) args="+set net_reliable_window 0 +set net_signon_fast 0" ;;
			godnotic) args="" ;;
			w*) args="+set net_reliable_window ${cfg#w}" ;;
			*) echo "unknown config $cfg" >&2; exit 1 ;;
		esac
		dir="$OUT/$cfg-$ms-$loss${netem:+-$(echo "$netem" | tr -c 'a-z0-9' _)}"
		if [ "$loss" != 0 ]; then export LOSS=$loss; else unset LOSS; fi
		if [ -n "$netem" ]; then export NETEM=$netem; else unset NETEM; fi
		export SV_EXTRA="+set touch_net_trace 1 $args"
		"$ROOT/scripts/bench-load.sh" -n "$RUNS" -o "$dir" -e "+set touch_net_trace 1 $args" "join:$MAP:$ms" > "$dir.bench.txt" 2>&1 || true
		load=$(awk '/game_ready.load /{print $3}' "$dir.bench.txt")
		python3 "$ROOT/scripts/net-trace-report.py" --rtt "$rtt" "$dir"/join-*.log > "$dir.report.txt" 2>&1 || true
		python3 "$ROOT/scripts/net-trace-report.py" --rtt "$rtt" --json "$dir"/join-*.log > "$dir.report.json" 2>/dev/null || echo '{}' > "$dir.report.json"
		python3 - "$dir.report.json" "$cfg" "$rtt" "$loss" "$netem" "${load:-?}" >> "$TABLE" <<'EOF'
import json, sys
path, cfg, rtt, loss, netem, load = sys.argv[1:]
try:
    ph = json.load(open(path))["phases"]
except Exception:
    print(f"| {cfg} | {rtt} | {loss}% | {netem} | {load} | (no complete join) | | | | | | |"); sys.exit()
cells = " | ".join(f"{ph[p]['wall']:.0f}" for p in ["connect", "serverinfo", "precache", "signon2", "spawn", "begin"])
w = ph["total"].get("wait_rtt")
print(f"| {cfg} | {rtt} | {loss}% | {netem} | {load} | {cells} | {w:.1f} |" if w is not None else f"| {cfg} | {rtt} | {loss}% | {netem} | {load} | {cells} | - |")
EOF
		echo "== $cfg RTT ${rtt} ms loss ${loss}% ${netem}: game_ready.load median ${load:-?} ms"
		cat "$dir.report.txt"
	done
done
echo
cat "$TABLE"
echo "logs in $OUT"
