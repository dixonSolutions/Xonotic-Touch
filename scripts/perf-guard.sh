#!/usr/bin/env bash
# Load-time regression guard: runs the desktop load scenarios several times
# and compares their medians with a recorded baseline of the same machine.
#
#   scripts/perf-guard.sh [-n RUNS] [-t PERCENT] [-s MS] [-b FILE] [-r] [-p CPUS]
#
#   -n RUNS     runs per scenario (default 5)
#   -t PERCENT  allowed slowdown of a median over the baseline (default 15)
#   -s MS       ... plus this many milliseconds of slack (default 40)
#   -b FILE     baseline (default ~/.cache/godnotic-perf/baseline[-pinCPUS].txt)
#   -r          record: write the medians as the new baseline, compare nothing
#   -p CPUS     pin the engine to these CPUs (taskset -c), e.g. 2,3, to come
#               closer to a phone's few slow cores; pinned and unpinned runs
#               keep separate baselines
#
# Scenarios (each run in a fresh copy of one returning player's userdir: it
# played stormkeep once, so its preload list, edict sizes and warm-up
# targets exist, as on a phone after the first game):
#   menu   process start -> first frame of the main menu
#   cold   menu -> first game frame of stormkeep with 4 bots, Play pressed
#          one second after the menu shows (touch_warm has not started)
#   warm   the same after six idle seconds on the menu (touch_warm done)
# Measured: menu_ready.proc, game_ready.load, and the worst frame of the
# first five seconds of play (after_ready.worst_frame) - load-time wins must
# not move cost into play. Exit status 1 when a median is over its limit.
#
# Environment: BASEDIR (game data dir holding data/, required), ENGINE
# (default build/bin/xonotic), LIBS (extra LD_LIBRARY_PATH).
#
# Not a CI job: a shared runner's timings vary by more than the tolerance.
# Run it on a quiet desktop before a release, against a baseline recorded on
# the same machine from the last release.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RUNS=5; TOL=15; SLACK=40; BASE=""; RECORD=0; PIN=""
while getopts "n:t:s:b:rp:h" opt; do
	case $opt in
		n) RUNS=$OPTARG ;;
		t) TOL=$OPTARG ;;
		s) SLACK=$OPTARG ;;
		b) BASE=$OPTARG ;;
		r) RECORD=1 ;;
		p) PIN=$OPTARG ;;
		*) sed -n '2,33p' "$0"; exit 2 ;;
	esac
done
: "${BASEDIR:?set BASEDIR to a game data dir (holding data/)}"
export BASEDIR
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
BASE="${BASE:-$HOME/.cache/godnotic-perf/baseline${PIN:+-pin$PIN}.txt}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/perf-guard.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

if [ -n "$PIN" ]; then
	cat > "$WORK/engine.sh" <<EOF
#!/bin/sh
exec taskset -c $PIN "$ENGINE" "\$@"
EOF
	chmod +x "$WORK/engine.sh"
	RUN_ENGINE="$WORK/engine.sh"
else
	RUN_ENGINE="$ENGINE"
fi

GAME="+bot_number 4 +skill 4 +set g_maplist stormkeep +set cl_welcome 0"
bench() { # out menu_idle scenario extra
	ENGINE="$RUN_ENGINE" MENU_IDLE="$2" "$ROOT/scripts/bench-load.sh" -n 1 -o "$1" -e "$GAME $4" "$3" > /dev/null 2>&1 || true
}

# the returning player: one recorded game, its config and preload list kept
echo "perf-guard: preparing a returning player's userdir"
bench "$WORK/rec" 1 map:stormkeep ""
[ -f "$WORK/rec/u-map-stormkeep-1/data/config.cfg" ] || { echo "perf-guard: the recording game failed (see $WORK/rec)" >&2; trap - EXIT; exit 2; }
cp -a "$WORK/rec/userdir-template" "$WORK/tmpl"
cp "$WORK/rec/u-map-stormkeep-1/data/config.cfg" "$WORK/tmpl/data/config.cfg"
cp -a "$WORK/rec/u-map-stormkeep-1/data/touch_preload" "$WORK/tmpl/data/" 2>/dev/null || true

one() { # scenario run
	local d="$WORK/$1-$2"
	mkdir -p "$d"; cp -a "$WORK/tmpl" "$d/userdir-template"
	case $1 in
		menu) bench "$d" 1 menu ""; cp "$d/menu-1.log" "$WORK/$1.$2.log" ;;
		cold) bench "$d" 1 map:stormkeep ""; cp "$d/map-stormkeep-1.log" "$WORK/$1.$2.log" ;;
		warm) bench "$d" 6 map:stormkeep ""; cp "$d/map-stormkeep-1.log" "$WORK/$1.$2.log" ;;
	esac
	rm -rf "$d"
}
for r in $(seq 1 "$RUNS"); do
	for sc in menu cold warm; do
		printf '\rperf-guard: run %d/%d %s ' "$r" "$RUNS" "$sc"
		one "$sc" "$r"
	done
done
echo

python3 - "$WORK" "$BASE" "$RECORD" "$TOL" "$SLACK" <<'EOF'
import glob, os, re, statistics, sys
work, base, record, tol, slack = sys.argv[1], sys.argv[2], sys.argv[3] == "1", float(sys.argv[4]), float(sys.argv[5])
pats = {
    "menu_ready.proc": r"LOADTIME menu_ready proc=([\d.]+)",
    "game_ready.load": r"LOADTIME game_ready load=([\d.]+)",
    "worst_frame": r"LOADTIME after_ready worst_frame=([\d.]+)",
}
want = {"menu": ["menu_ready.proc"], "cold": ["game_ready.load", "worst_frame"], "warm": ["game_ready.load", "worst_frame"]}
now = {}
for sc, keys in want.items():
    for k in keys:
        vals = []
        for f in glob.glob(os.path.join(work, f"{sc}.*.log")):
            t = re.sub(r"\x1b\[[0-9;]*m", "", open(f, errors="replace").read())
            m = re.search(pats[k], t)
            if m:
                vals.append(float(m.group(1)))
        if vals:
            now[f"{sc}.{k}"] = (statistics.median(vals), min(vals), max(vals), len(vals))
if record:
    os.makedirs(os.path.dirname(base) or ".", exist_ok=True)
    with open(base, "w") as f:
        for k, (med, lo, hi, n) in sorted(now.items()):
            f.write(f"{k} {med:.1f} {lo:.1f} {hi:.1f} {n}\n")
    for k, (med, lo, hi, n) in sorted(now.items()):
        print(f"  {k:<28} {med:8.1f} ms  ({lo:.1f} - {hi:.1f}, n={n})")
    print(f"perf-guard: baseline written to {base}")
    sys.exit(0)
if not os.path.exists(base):
    print(f"perf-guard: no baseline at {base}; record one with -r", file=sys.stderr)
    sys.exit(2)
old = {}
for line in open(base):
    p = line.split()
    if len(p) >= 2:
        old[p[0]] = float(p[1])
bad = 0
for k, (med, lo, hi, n) in sorted(now.items()):
    if k not in old:
        print(f"  {k:<28} {med:8.1f} ms  (no baseline)")
        continue
    limit = old[k] * (1 + tol / 100.0) + slack
    flag = "OVER" if med > limit else "ok"
    bad += med > limit
    print(f"  {k:<28} {med:8.1f} ms  baseline {old[k]:8.1f}  limit {limit:8.1f}  {flag}  ({lo:.1f} - {hi:.1f}, n={n})")
missing = [k for k in old if k not in now]
for k in missing:
    print(f"  {k:<28} not measured (the scenario failed)")
sys.exit(1 if bad or missing else 0)
EOF
