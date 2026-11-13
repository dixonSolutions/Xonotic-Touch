#!/usr/bin/env bash
# Load-time benchmark: process start to main menu, menu to first game frame of
# a local match, and a second client joining a server over loopback, with and
# without simulated latency. Each scenario runs RUNS times; the summary gives
# the median and the spread (min-max) of every LOADTIME figure the engine
# prints (touch_loadtime 1, engine/darkplaces/touch_loadtime.c).
#
#   scripts/bench-load.sh [-n RUNS] [-c warm|cold] [-o OUTDIR] [-e "extra +args"] SCENARIO...
#
# SCENARIO:
#   menu              process start -> first frame of the main menu
#   map:NAME          main menu -> first game frame of `map NAME` (local game)
#   join:NAME[:MS]    a dedicated server runs NAME; a second process connects
#                     to 127.0.0.1 from its main menu; MS adds that much
#                     one-way delay to loopback (tc netem in a user+network
#                     namespace, so RTT = 2*MS). LOSS=<percent> adds loss.
#
# Environment:
#   BASEDIR   game data dir holding data/ (required)
#   ENGINE    engine binary (default: build/bin/xonotic of this checkout)
#   LIBS      extra LD_LIBRARY_PATH (e.g. a dir with libjpeg.so.62)
#   W, H      window size (default 1280x720, offscreen SDL)
#   MENU_IDLE seconds spent in the menu before the load starts (default 1)
#
# Cold runs evict the pk3s, the engine and its data dir from the page cache
# with posix_fadvise(DONTNEED) (no root needed); warm runs read them once
# first. Every run gets a fresh copy of a userdir made by one throwaway run,
# so config.cfg exists as on a real install.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNS=5
CACHE=warm
OUT=""
EXTRA=""
while getopts "n:c:o:e:h" opt; do
	case $opt in
		n) RUNS=$OPTARG ;;
		c) CACHE=$OPTARG ;;
		o) OUT=$OPTARG ;;
		e) EXTRA=$OPTARG ;;
		*) sed -n '2,33p' "$0"; exit 1 ;;
	esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { sed -n '2,33p' "$0"; exit 1; }
: "${BASEDIR:?set BASEDIR to a game data dir (holding data/)}"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
W="${W:-1280}"; H="${H:-720}"
MENU_IDLE="${MENU_IDLE:-1}"
OUT="${OUT:-$(mktemp -d "${TMPDIR:-/tmp}/bench-load.XXXXXX")}"
mkdir -p "$OUT"

export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy

evict() {
	python3 - "$@" <<'EOF'
import os, sys
for root in sys.argv[1:]:
    paths = [root] if os.path.isfile(root) else []
    for d, _, files in os.walk(root, followlinks=True):
        paths += [os.path.join(d, f) for f in files]
    for p in paths:
        try:
            fd = os.open(os.path.realpath(p), os.O_RDONLY)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            os.close(fd)
        except OSError:
            pass
EOF
}
warm() {
	find -L "$@" -type f -print0 2>/dev/null | xargs -0 cat > /dev/null 2>&1 || true
}
prepare_cache() {
	if [ "$CACHE" = cold ]; then
		evict "$BASEDIR/data/" "$ENGINE"
	else
		warm "$BASEDIR/data/" "$ENGINE"
	fi
}

# engine command line shared by every run
engine_args() { # userdir
	echo -xonotic -basedir "$BASEDIR" -userdir "$1" +exec xonotic.cfg +vid_fullscreen 0 \
		+vid_width "$W" +vid_height "$H" +set _touch_assets_ready 1 +set touch_loadtime 1 $EXTRA
}

TEMPLATE="$OUT/userdir-template"
make_template() {
	[ -d "$TEMPLATE" ] && return
	mkdir -p "$TEMPLATE/data/touch"
	# touch/startup.cfg present, or CSQC init falls back to exec'ing battery.cfg
	echo "echo startup.cfg" > "$TEMPLATE/data/touch/startup.cfg"
	cat > "$TEMPLATE/data/bench-quit.cfg" <<'EOF'
alias _bq_0 "defer 0.05 _bq_poll"
alias _bq_1 "defer 0.5 quit"
alias _bq_poll "_bq_$_touch_loadtime_menu"
_bq_poll
EOF
	# shellcheck disable=SC2046
	timeout 120 "$ENGINE" $(engine_args "$TEMPLATE") +exec bench-quit.cfg > "$OUT/template.log" 2>&1 || true
	[ -f "$TEMPLATE/data/config.cfg" ] || { echo "template run made no config.cfg (see $OUT/template.log)" >&2; exit 1; }
	# a returning player: no first-run dialog over the main menu
	printf 'seta touch_profile_done 1\nseta _cl_name "Bench"\nseta cl_allow_uid2name 0\n' >> "$TEMPLATE/data/config.cfg"
}

fresh_userdir() { # name
	local u="$OUT/u-$1"
	rm -rf "$u"; cp -a "$TEMPLATE" "$u"
	echo "$u"
}

# job cfg: wait for the menu, idle, run $1, wait for the game, quit 4 s later
# (after the engine's 3 s after_ready line)
write_job() { # userdir command
	cat > "$1/data/bench-job.cfg" <<EOF
alias _bj_w0 "defer 0.05 _bj_wait"
alias _bj_w1 "defer $MENU_IDLE _bj_go"
alias _bj_wait "_bj_w\$_touch_loadtime_menu"
alias _bj_go "touch_loadtime_start; $2; _bj_gwait"
alias _bj_g0 "defer 0.05 _bj_gwait"
alias _bj_g1 "defer 4 quit"
alias _bj_gwait "_bj_g\$_touch_loadtime_game"
_bj_wait
EOF
}

run_menu() { # run
	local u log; u=$(fresh_userdir "menu-$1"); log="$OUT/menu-$1.log"
	prepare_cache
	# shellcheck disable=SC2046
	timeout 120 "$ENGINE" $(engine_args "$u") +exec bench-quit.cfg > "$log" 2>&1 || true
	echo "$log"
}

run_map() { # map run
	local u log; u=$(fresh_userdir "map-$1-$2"); log="$OUT/map-$1-$2.log"
	write_job "$u" "map $1"
	prepare_cache
	# shellcheck disable=SC2046
	timeout 180 "$ENGINE" $(engine_args "$u") +exec bench-job.cfg > "$log" 2>&1 || true
	echo "$log"
}

run_join() { # map delay_ms run
	local map=$1 ms=$2 r=$3 u su log slog port=26123
	u=$(fresh_userdir "join-$map-$ms-$r"); su="$OUT/u-joinsv-$map-$ms-$r"
	rm -rf "$su"; mkdir -p "$su"
	log="$OUT/join-$map-$ms-$r.log"; slog="$OUT/joinsv-$map-$ms-$r.log"
	write_job "$u" "connect 127.0.0.1:$port"
	prepare_cache
	# shellcheck disable=SC2016
	unshare -rn bash -c '
		ip link set lo up
		if [ "$1" != 0 ] || [ -n "${LOSS:-}" ]; then
			tc qdisc add dev lo root netem delay "${1}ms" ${LOSS:+loss ${LOSS}%}
		fi
		shift
		sv=$1; slog=$2; cl=$3; log=$4; map=$5; port=$6; shift 6
		"$sv" -dedicated -xonotic -basedir "$BASEDIR" -userdir "$SVU" +set port "$port" +set sv_public 0 +map "$map" > "$slog" 2>&1 &
		svpid=$!
		# the server takes a few seconds to load its map; the client only
		# connects once at its menu, and retries until the server answers
		for i in $(seq 1 200); do grep -q "Server spawned\|SpawnServer" "$slog" 2>/dev/null && break; sleep 0.05; done
		sleep 1
		timeout 180 "$cl" "$@" > "$log" 2>&1 || true
		kill $svpid 2>/dev/null; wait $svpid 2>/dev/null || true
	' bash "$ms" "$ENGINE" "$slog" "$ENGINE" "$log" "$map" "$port" $(engine_args "$u") +exec bench-job.cfg
	echo "$log"
}

summarise() { # label logs...
	python3 - "$@" <<'EOF'
import re, sys, statistics
label, logs = sys.argv[1], sys.argv[2:]
rows = {}
order = []
def add(k, v):
    if k not in rows:
        rows[k] = []; order.append(k)
    rows[k].append(v)
for path in logs:
    stage = None
    for line in open(path, errors="replace"):
        if "LOADTIME" not in line:
            continue
        line = re.sub(r"\x1b\[[0-9;]*m", "", line).strip()
        m = re.match(r"LOADTIME (menu_ready|game_ready|after_ready)\s+(.*)", line)
        if m:
            stage = m.group(1)
            for k, v in re.findall(r"(\w+)=([-0-9.]+)", m.group(2)):
                add(f"{stage}.{k}", float(v))
            continue
        m = re.match(r"LOADTIME\s+signon (\d) at\s+([-0-9.]+) ms", line)
        if m and stage:
            add(f"{stage}.signon{m.group(1)}_ms", float(m.group(2))); continue
        m = re.match(r"LOADTIME\s+(.+?)\s+([-0-9.]+) (ms|MB)\s+(\d+) calls", line)
        if m and stage:
            add(f"{stage}.{m.group(1).strip()} [{m.group(3)}]", float(m.group(2))); continue
        m = re.match(r"LOADTIME\s+(.+?)\s+([-0-9.]+)( MB)?$", line)
        if m and stage:
            add(f"{stage}.{m.group(1).strip()}{' [MB]' if m.group(3) else ''}", float(m.group(2)))
print(f"== {label}: {len(logs)} runs")
for k in order:
    v = rows[k]
    med = statistics.median(v)
    print(f"  {k:<52} median {med:10.1f}   spread {min(v):9.1f} - {max(v):9.1f}   n={len(v)}")
EOF
}

make_template
for sc in "$@"; do
	logs=()
	case $sc in
		menu)
			for r in $(seq 1 "$RUNS"); do logs+=("$(run_menu "$r")"); done ;;
		map:*)
			m=${sc#map:}
			for r in $(seq 1 "$RUNS"); do logs+=("$(run_map "$m" "$r")"); done ;;
		join:*)
			rest=${sc#join:}; m=${rest%%:*}; ms=0
			[ "$rest" != "$m" ] && ms=${rest#*:}
			export SVU="$OUT/u-joinsv"; mkdir -p "$SVU"
			for r in $(seq 1 "$RUNS"); do logs+=("$(run_join "$m" "$ms" "$r")"); done ;;
		*) echo "unknown scenario $sc" >&2; exit 1 ;;
	esac
	summarise "$sc ($CACHE cache)" "${logs[@]}" | tee -a "$OUT/summary.txt"
done
echo "logs and summary in $OUT"
