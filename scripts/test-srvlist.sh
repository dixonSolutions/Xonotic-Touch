#!/bin/bash
# The fast server list (docs/NETWORK.md, "The server list").
#   1. tests/srvlist/srvlist_logic_test.c: the saved list's format, the query
#      budget, tries, percentiles (no engine).
#   2. tests/srvlist/test_srvlist.py: the engine, offscreen, against a fake
#      master and 160 fake servers on loopback with latency and loss
#      (tests/srvlist/fakenet.py): time to first row / 50% / 95% / settled,
#      the saved rows on the second open, stale rows refreshed and removed,
#      the bridge's servers source and the summary, and how politely the
#      servers were asked. Skipped (exit 77 if nothing else ran) without
#      ENGINE and BASEDIR.
#
#   BASEDIR=<Godnotic basedir> scripts/test-srvlist.sh
# Environment: ENGINE (default engine/darkplaces/darkplaces-sdl), LIBS (extra
# LD_LIBRARY_PATH), OUT (keep the logs there).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-srvlist.XXXXXX")"
cleanup() {
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -I"$ROOT/engine/darkplaces" "$ROOT/tests/srvlist/srvlist_logic_test.c" -o "$WORK/logic-test" || exit 1
"$WORK/logic-test" || exit 1
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
if [ ! -x "$ENGINE" ] || [ ! -d "${BASEDIR:-/nonexistent}/data" ]; then
	echo "SKIP: end to end (set ENGINE and BASEDIR)"
	exit 0
fi
command -v python3 > /dev/null || { echo "SKIP: end to end (no python3)"; exit 0; }
python3 "$ROOT/tests/srvlist/test_srvlist.py" --engine "$ENGINE" --basedir "$BASEDIR" --work "$WORK/e2e" --libs "${LIBS:-}" --port $((27960 + RANDOM % 500))
exit $?
