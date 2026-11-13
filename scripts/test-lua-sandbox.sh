#!/bin/bash
# Sandbox escape tests for the Lua mods runtime (engine/darkplaces/touch_lua*.c).
#
# Starts the real engine headless (SDL offscreen) with a fresh userdir holding the
# mods in tests/lua-sandbox/mods, all enabled, and checks:
#   - escape: every check prints SANDBOX PASS (no FAIL), and its command runs
#   - kill_*: each one ends in the error state with the expected reason, while
#     the engine keeps running (no hang, no crash) and quits cleanly
#
#   BASEDIR=<dir with data/*.pk3 and the Touch xonotic-data.pk3dir> scripts/test-lua-sandbox.sh
#
# ENGINE defaults to engine/darkplaces/darkplaces-sdl. See docs/MODDING.md.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
BASEDIR="${BASEDIR:?set BASEDIR to a Xonotic basedir (data/*.pk3)}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-lua-sandbox.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
UD="$WORK/home"
mkdir -p "$UD/mods" "$UD/data"
cp -r "$ROOT/tests/lua-sandbox/mods/." "$UD/mods/"
(cd "$UD/mods" && ls -d */ | tr -d / > enabled.txt)
# a folder that is not enabled must stay off
mkdir -p "$UD/mods/notenabled" && echo 'print("SANDBOX FAIL notenabled ran")' > "$UD/mods/notenabled/init.lua"

cat > "$UD/data/sandbox.cfg" <<'EOF'
defer 20 "lua_escape_selftest one two"
defer 21 "lua_mods"
defer 22 "lua_mod_disable escape"
defer 23 "lua_mods"
defer 24 "quit"
EOF

LOG="$WORK/out.log"
START=$(date +%s)
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout 120 "$ENGINE" -xonotic -nosound \
	-basedir "$BASEDIR" -userdir "$UD" +vid_width 640 +vid_height 480 +vid_fullscreen 0 \
	+cl_lua_enable 1 +exec sandbox.cfg > "$LOG" 2>&1
RC=$?
ELAPSED=$(( $(date +%s) - START ))

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

[ "$RC" = 0 ] && ok "engine exited cleanly after ${ELAPSED}s" || bad "engine exit code $RC after ${ELAPSED}s (124 = hung)"

pass=$(grep -c "SANDBOX PASS" "$LOG")
fails=$(grep "SANDBOX FAIL" "$LOG")
done_line=$(grep -o "SANDBOX DONE [0-9]*/[0-9]*" "$LOG" | head -1)
[ -n "$done_line" ] && ok "escape mod ran to the end: $done_line" || bad "escape mod did not finish"
[ -z "$fails" ] && ok "no escape check failed ($pass passed)" || { bad "escape checks failed:"; echo "$fails"; }
grep -q "SANDBOX exec reached the console" "$LOG" && ok "allowlisted xt.exec reached the console" || bad "xt.exec echo missing"
grep -q "SANDBOX command args one,two" "$LOG" && ok "mod console command got its arguments" || bad "mod console command did not run"

expect_error() { # mod, reason substring
	if grep -E "Lua mod $1 stopped: .*$2" "$LOG" > /dev/null; then ok "$1 stopped: $2"; else bad "$1 not stopped with '$2'"; grep "Lua mod $1" "$LOG"; fi
}
expect_error kill_loop "instruction limit|time limit"
expect_error kill_mem "memory limit"
expect_error kill_xpcall "instruction limit|time limit"
expect_error kill_coro "instruction limit|time limit"
expect_error kill_pattern "time limit|pattern too complex"
expect_error kill_close "instruction limit|time limit"
expect_error kill_gc "__gc metamethods are not allowed"
expect_error kill_frame "instruction limit|time limit"
expect_error kill_strrep "memory limit"
grep -q "SANDBOX kill_frame loaded" "$LOG" && ok "kill_frame loaded before its frame event" || bad "kill_frame never loaded"
grep -qE "^ *escape +off" "$LOG" && ok "lua_mod_disable turned escape off" || bad "escape not shown off after lua_mod_disable"
grep -qE "^ *notenabled +off" "$LOG" && ok "a folder not in enabled.txt stays off" || bad "notenabled state wrong"

if [ "$fail" != 0 ]; then
	echo "--- engine log (Lua lines) ---"
	grep -E "Lua|SANDBOX|\[" "$LOG" | tail -80
	exit 1
fi
echo "all Lua sandbox checks passed"
