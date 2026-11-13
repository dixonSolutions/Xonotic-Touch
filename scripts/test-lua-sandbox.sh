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
# Only event and command registration are initially granted to the v1 fixture.
mkdir -p "$UD/mods/.permissions"
printf 'v1 3\n' > "$UD/mods/.permissions/scoped.txt"
printf 'bad grant\n' > "$UD/mods/.permissions/invalid_grants.txt"
# a folder that is not enabled must stay off
mkdir -p "$UD/mods/notenabled" && echo 'print("SANDBOX FAIL notenabled ran")' > "$UD/mods/notenabled/init.lua"

cat > "$UD/data/sandbox.cfg" <<'EOF'
defer 20 "lua_escape_selftest one two"
defer 1 "lua_mod_permission scoped grant players"
defer 2 "lua_scoped_probe"
defer 3 "lua_mod_permission scoped grant exec"
defer 4 "lua_scoped_probe"
defer 5 "lua_mod_permission scoped revoke exec"
defer 6 "lua_mod_permission scoped revoke events"
defer 7 "lua_scoped_probe"
defer 8 "_cl_lua_permissions"
defer 9 "lua_scoped_probe"
defer 10 "lua_mod_permission scoped revoke commands"
defer 11 "lua_scoped_probe"
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
grep -q 'permission players is not requested' "$LOG" && ok "unrequested permission refused" || bad "unrequested permission accepted"
grep -q 'PERMISSIONS PASS malformed grant file denied' "$LOG" && ok "malformed legacy grants fail closed" || bad "malformed legacy grants allowed"
[ "$(grep -c 'PERMISSIONS PASS denied' "$LOG")" = 3 ] && ok "ungranted APIs denied" || bad "ungranted API denial failed"
[ "$(grep -c 'PERMISSIONS exec allowed' "$LOG")" = 1 ] && ok "grant and revocation take effect" || bad "exec grant/revocation failed"
[ "$(grep -c 'PERMISSIONS allowlist PASS' "$LOG")" = 4 ] && ok "grants do not bypass allowlists" || bad "allowlist check failed"
[ "$(grep -c 'PERMISSIONS frames ' "$LOG")" = 4 ] && ok "revoked command not dispatched" || bad "command revocation failed"
frames_after=$(grep 'PERMISSIONS frames ' "$LOG" | tail -2 | sed 's/.*PERMISSIONS frames //')
[ "$(printf '%s\n' "$frames_after" | sort -u | wc -l)" = 1 ] && ok "event revocation takes effect" || bad "revoked events still dispatched"
grep -q $'scoped\tv1\tevents,commands,cvar_write,exec,storage\tcommands' "$LOG" && ok "machine-readable status reflects revocation" || bad "permission status wrong"
[ "$(cat "$UD/mods/.permissions/scoped.txt")" = 'v1 0' ] && ok "revocation persisted" || bad "revocation was not persisted"
grep -q 'PERMISSIONS FAIL' "$LOG" && bad "permission fixture failed"
expect_error invalid_api "invalid or unsupported Lua API manifest"
expect_error invalid_scope "invalid or unsupported Lua API manifest"
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

# Exercise the server realm in a local match too, with an independent grant store.
mkdir -p "$UD/plugins/.permissions"
cp -r "$ROOT/tests/lua-sandbox/plugins/scoped_server" "$UD/plugins/"
printf 'scoped_server\n' > "$UD/plugins/enabled.txt"
printf 'v1 2\n' > "$UD/plugins/.permissions/scoped_server.txt"
cat > "$UD/data/server-permissions.cfg" <<'EOF'
map stormkeep
defer 3 "sv_lua_scoped_server_probe"
defer 4 "sv_lua_plugin_permission scoped_server grant messages"
defer 5 "sv_lua_scoped_server_probe"
defer 6 "sv_lua_plugin_permission scoped_server revoke messages"
defer 7 "sv_lua_scoped_server_probe"
defer 9 "quit"
EOF
SERVER_LOG="$WORK/server.log"
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout 120 "$ENGINE" -xonotic -nosound \
    -basedir "$BASEDIR" -userdir "$UD" +vid_width 640 +vid_height 480 +vid_fullscreen 0 \
    +exec server-permissions.cfg > "$SERVER_LOG" 2>&1
[ "$?" = 0 ] && ok "server realm exited cleanly" || bad "server realm did not exit cleanly"
[ "$(grep -c 'SERVER PERMISSIONS denied' "$SERVER_LOG")" = 2 ] && ok "server message permission denied before grant and after revoke" || bad "server message revocation failed"
[ "$(grep -c 'SERVER PERMISSIONS allowed' "$SERVER_LOG")" = 1 ] && ok "server message grant worked" || bad "server message grant failed"
grep -q 'SERVER PERMISSIONS FAIL' "$SERVER_LOG" && bad "server permission fixture failed"
[ "$(cat "$UD/plugins/.permissions/scoped_server.txt")" = 'v1 2' ] && ok "server grant stored separately" || bad "server grant storage wrong"

if [ "$fail" != 0 ]; then
	echo "--- engine log (Lua lines) ---"
	grep -E "Lua|SANDBOX|PERMISSIONS|\[" "$LOG" | tail -80
	grep -E "Lua|PERMISSIONS" "$SERVER_LOG" | tail -80
	exit 1
fi
echo "all Lua sandbox checks passed"
