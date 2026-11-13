#!/bin/bash
# Sandbox escape tests for the Lua mods runtime (engine/darkplaces/touch_lua*.c).
#
# Starts the real engine headless (SDL offscreen) with a fresh userdir holding the
# mods in tests/lua-sandbox/mods, all enabled, and checks:
#   - escape: every check prints SANDBOX PASS (no FAIL), and its command runs
#   - kill_*: each one ends in the error state with the expected reason, while
#     the engine keeps running (no hang, no crash) and quits cleanly; the
#     kill_t* ones loop inside the C table library, where the engine used to hang
#   - storage, mod folders and modules never go through a symlink, a hard link
#     or a FIFO; the manager's state stays out of every mod's reach
#   - legacy mods get the HUD set of permissions only; reserved names, the
#     <mod>_<name> namespace and the per-mod cvar cap
#   - the server realm's grants (a local match)
#   - text a server stuffs (a demo carrying svc_stufftext) cannot drive the
#     manager, the limit cvars or a mod's commands, not even after disconnect
#   - crash-loop protection: an engine killed while a mod loads does not load
#     that mod again at the next start
# Every engine run has a timeout (with SIGKILL after it: a hang inside a C
# library call does not answer SIGTERM), so a regression fails instead of
# hanging.
#
#   BASEDIR=<dir with data/*.pk3 and the Touch xonotic-data.pk3dir> scripts/test-lua-sandbox.sh
#
# ENGINE defaults to engine/darkplaces/darkplaces-sdl; KEEP=1 keeps the logs.
# See docs/MODDING.md.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
BASEDIR="${BASEDIR:?set BASEDIR to a Xonotic basedir (data/*.pk3)}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-lua-sandbox.XXXXXX")"
trap '[ -n "${KEEP:-}" ] && echo "logs kept in $WORK" || rm -rf "$WORK"' EXIT
UD="$WORK/home"
mkdir -p "$UD/mods" "$UD/data"
cp -r "$ROOT/tests/lua-sandbox/mods/." "$UD/mods/"

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

run_engine() { # log, extra args...
	local log="$1"; shift
	SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout -k 10 120 "$ENGINE" -xonotic -nosound \
		-basedir "$BASEDIR" +vid_width 640 +vid_height 480 +vid_fullscreen 0 "$@" > "$log" 2>&1
}

# --- fixtures the repository cannot hold --------------------------------------
# a FIFO as init.lua: the folder scan used to block on it forever
mkdir -p "$UD/mods/fifo" && mkfifo "$UD/mods/fifo/init.lua"
# a mod folder that is a symlink to code outside the mods folder
mkdir -p "$WORK/outside" && echo 'print("SYMLINK FAIL a symlinked mod folder ran")' > "$WORK/outside/init.lua"
ln -s "$WORK/outside" "$UD/mods/linkedmod"
# storage through symlinks: data -> the old grant folder, data -> the manager's grants
mkdir -p "$UD/mods/.permissions" && printf 'v1 3\n' > "$UD/mods/.permissions/scoped.txt"
ln -s ../.permissions "$UD/mods/storelink_old/data"
ln -s ../../luastate/mods/grants "$UD/mods/storelink_new/data"
# a real data folder with a symlinked file, a hard link, and a symlinked module
mkdir -p "$UD/mods/storefile/data"
(cd "$UD/mods" && ls -d */ | tr -d / > enabled.txt)
ln -s ../../enabled.txt "$UD/mods/storefile/data/link.txt"
ln "$UD/mods/enabled.txt" "$UD/mods/storefile/data/hard.txt"
ln -s "$WORK/outside/init.lua" "$UD/mods/storefile/evil.lua"
ENABLED_BEFORE="$(cat "$UD/mods/enabled.txt")"

# Grants are the user's, in <userdir>/luastate/, out of every mod's reach.
# Only event and command registration are initially granted to the v1 fixture.
G="$UD/luastate/mods/grants"
mkdir -p "$G"
printf 'v1 3\n' > "$G/scoped.txt"
printf 'bad grant\n' > "$G/invalid_grants.txt"
for m in escape ns_a ns_a_b storelink_old storefile; do printf 'legacy 7ff\n' > "$G/$m.txt"; done
printf 'legacy 7df\n' > "$G/flood.txt"          # everything but storage
printf 'legacy 20\n' > "$G/storelink_new.txt"   # storage only
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
defer 12 "lua_ns_a_b_c"
defer 21 "lua_mods"
defer 22 "lua_mod_disable escape"
defer 23 "lua_mods"
defer 24 "quit"
EOF

LOG="$WORK/out.log"
START=$(date +%s)
run_engine "$LOG" -userdir "$UD" +cl_lua_enable 1 +exec sandbox.cfg
RC=$?
ELAPSED=$(( $(date +%s) - START ))

[ "$RC" = 0 ] && ok "engine exited cleanly after ${ELAPSED}s" || bad "engine exit code $RC after ${ELAPSED}s (124/137 = hung)"

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
has() { grep -qF -- "$1" "$LOG"; }
grep -q 'permission players is not requested' "$LOG" && ok "unrequested permission refused" || bad "unrequested permission accepted"
grep -q 'PERMISSIONS PASS malformed grant file denied' "$LOG" && ok "malformed legacy grants fail closed" || bad "malformed legacy grants allowed"
[ "$(grep -c 'PERMISSIONS PASS denied' "$LOG")" = 3 ] && ok "ungranted APIs denied" || bad "ungranted API denial failed"
[ "$(grep -c 'PERMISSIONS exec allowed' "$LOG")" = 1 ] && ok "grant and revocation take effect" || bad "exec grant/revocation failed"
[ "$(grep -c 'PERMISSIONS allowlist PASS' "$LOG")" = 4 ] && ok "grants do not bypass allowlists" || bad "allowlist check failed"
[ "$(grep -c 'PERMISSIONS frames ' "$LOG")" = 4 ] && ok "revoked command not dispatched" || bad "command revocation failed"
frames_after=$(grep 'PERMISSIONS frames ' "$LOG" | tail -2 | sed 's/.*PERMISSIONS frames //')
[ "$(printf '%s\n' "$frames_after" | sort -u | wc -l)" = 1 ] && ok "event revocation takes effect" || bad "revoked events still dispatched"
grep -q $'scoped\tv1\tevents,commands,cvar_write,exec,storage\tcommands' "$LOG" && ok "machine-readable status reflects revocation" || bad "permission status wrong"
[ "$(cat "$G/scoped.txt")" = 'v1 0' ] && ok "revocation persisted" || bad "revocation was not persisted"
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

# 1a: loops inside C library functions end like Lua loops (they used to hang the engine)
expect_error kill_tmove "instruction limit|time limit"
expect_error kill_tinsert "instruction limit|time limit"
expect_error kill_tremove "instruction limit|time limit"
expect_error kill_tsort "instruction limit|time limit"
has "BOUNDS rep returned 0 0 0 ab-ab-ab" && ok "string.rep with an empty result returns at once" || bad "string.rep bound"
# 2: legacy mods get the HUD set until granted more
for p in exec storage cvar_write; do has "LEGACY $p denied" && ok "legacy mod: $p denied by default" || bad "legacy mod: $p not denied"; done
for p in cvar_read state display events; do has "LEGACY $p allowed" && ok "legacy mod: $p allowed by default" || bad "legacy mod: $p not allowed"; done
has "LEGACY exec ran" && bad "legacy mod ran a console command without a grant"
# 2: storage and code only through real files below the mod's own folder
has "STORE old PASS" && ok "save through data -> ../.permissions refused" || bad "save through a symlinked data folder (old grants)"
has "STORE new PASS" && ok "save through data -> luastate/mods/grants refused" || bad "save through a symlinked data folder (grants)"
[ "$(cat "$UD/mods/.permissions/scoped.txt")" = 'v1 3' ] && ok "old grant file untouched" || bad "a mod rewrote mods/.permissions/scoped.txt"
[ "$(cat "$G/storelink_new.txt")" = 'legacy 20' ] && ok "a mod cannot rewrite its own grant file" || bad "a mod rewrote its grant file"
[ "$(grep -c 'STORE .* PASS$' "$LOG")" = 6 ] && ok "symlinked/hard-linked files and modules refused, plain storage works" || { bad "storefile checks:"; grep "STORE " "$LOG"; }
[ "$(cat "$UD/mods/storefile/data/hard.txt")" = "$ENABLED_BEFORE" ] && ok "hard-linked enabled.txt untouched" || bad "a mod wrote through a hard link"
has "SYMLINK FAIL" && bad "a symlinked mod folder ran" || ok "a symlinked mod folder does not run"
grep -qE "^ *linkedmod " "$LOG" && bad "a symlinked mod folder is listed" || ok "a symlinked mod folder is not listed"
grep -qE "^ *fifo " "$LOG" && bad "a FIFO init.lua is listed" || ok "a FIFO init.lua does not block the scan and is not listed"
# 5: names
expect_error memory "the name is reserved"
has "RESERVED FAIL" && bad "a mod named memory ran"
has "NS PASS ns_a refused b_c" && ok "a command name with _ refused (no hijack of ns_a_b's commands)" || bad "ns_a took ns_a_b's command name"
has "NS PASS ns_a refused ns_a_b's cvar" && ok "ns_a cannot set ns_a_b's cvar" || bad "ns_a set ns_a_b's cvar"
has "NS PASS ns_a_b set its cvar" && ok "ns_a_b sets its own cvar" || bad "ns_a_b could not set its cvar"
has "NS ns_a_b command ran" && ok "lua_ns_a_b_c runs ns_a_b's command" || bad "lua_ns_a_b_c did not run ns_a_b's command"
has "NS FAIL" && bad "namespace fixture failed"
has "FLOOD created 32" && ok "own cvars capped at 32 per mod" || bad "own cvar cap: $(grep -o 'FLOOD created [0-9]*' "$LOG")"
CFG="$UD/data/config.cfg"
if [ -f "$CFG" ] && grep -q lua_escape_test "$CFG"; then
	grep -q lua_flood_ "$CFG" && bad "own cvars of a mod without storage were archived ($(grep -c lua_flood_ "$CFG"))" || ok "own cvars archived only with storage"
else
	bad "config.cfg missing or without the storage-granted mod's cvar"
fi
# 7: unload at quit
has "SANDBOX atquit unload ran" && ok "unload event runs at quit" || bad "unload did not run at quit"

# Exercise the server realm in a local match too, with an independent grant store.
mkdir -p "$UD/plugins" "$UD/luastate/plugins/grants"
cp -r "$ROOT/tests/lua-sandbox/plugins/scoped_server" "$UD/plugins/"
printf 'scoped_server\n' > "$UD/plugins/enabled.txt"
printf 'v1 2\n' > "$UD/luastate/plugins/grants/scoped_server.txt"
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
run_engine "$SERVER_LOG" -userdir "$UD" +exec server-permissions.cfg
[ "$?" = 0 ] && ok "server realm exited cleanly" || bad "server realm did not exit cleanly"
[ "$(grep -c 'SERVER PERMISSIONS denied' "$SERVER_LOG")" = 2 ] && ok "server message permission denied before grant and after revoke" || bad "server message revocation failed"
[ "$(grep -c 'SERVER PERMISSIONS allowed' "$SERVER_LOG")" = 1 ] && ok "server message grant worked" || bad "server message grant failed"
grep -q 'SERVER PERMISSIONS FAIL' "$SERVER_LOG" && bad "server permission fixture failed"
[ "$(cat "$UD/luastate/plugins/grants/scoped_server.txt")" = 'v1 2' ] && ok "server grant stored separately" || bad "server grant storage wrong"

# --- 3: a server's text -----------------------------------------------------
# A demo is what a server sends: here one svc_stufftext that tries to switch a
# mod on, raise the limits, turn mods off, run a mod's commands, grant
# permissions, and do it again after a disconnect and from a defer.
RUD="$WORK/remote"
mkdir -p "$RUD/mods" "$RUD/data"
cp -r "$ROOT/tests/lua-sandbox/remote/." "$RUD/mods/"
printf 'remote_cmd\n' > "$RUD/mods/enabled.txt"
python3 - "$RUD/data/stuff.dem" <<'EOF'
import struct, sys
text = (b"lua_mod_enable victim\n"
        b"lua_memory_mb 64\n"
        b"lua_time_ms 250\n"
        b"cl_lua_enable 0\n"
        b"lua_remote_cmd_plain\n"
        b"lua_remote_cmd_open\n"
        b"lua_mod_permission remote_cmd grant exec\n"
        b"sv_lua_plugin_permission anything grant exec\n"
        b"defer 2 \"lua_mod_enable victim\"\n"
        b"disconnect\n"
        b"lua_mod_enable victim\n")
msg = bytes([9]) + text + b"\0"  # svc_stufftext
with open(sys.argv[1], "wb") as f:
    f.write(b"-1\n")
    f.write(struct.pack("<i", len(msg)) + struct.pack("<fff", 0, 0, 0) + msg)
EOF
cat > "$RUD/data/remote.cfg" <<'EOF'
defer 3 "playdemo stuff"
defer 7 "lua_memory_mb"
defer 7.2 "lua_time_ms"
defer 7.4 "cl_lua_enable"
defer 7.6 "lua_mods"
defer 8 "echo REMOTE local enable next"
defer 8.2 "lua_mod_enable victim"
defer 8.5 "lua_remote_cmd_local"
defer 9 "lua_mods"
defer 10 "quit"
EOF
REMOTE_LOG="$WORK/remote.log"
run_engine "$REMOTE_LOG" -userdir "$RUD" +exec remote.cfg
[ "$?" = 0 ] && ok "remote run exited cleanly" || bad "remote run did not exit cleanly"
rhas() { grep -qF -- "$1" "$REMOTE_LOG"; }
rhas 'lua_remote_cmd_plain: refused' && ok "the stuffed text reached the console (and was refused)" || bad "the demo's text never ran (is the demo test valid?)"
grep -qE 'lua_memory_mb(.\[m)?"? is "16"' "$REMOTE_LOG" && ok "server text cannot raise lua_memory_mb" || bad "lua_memory_mb changed by server text: $(grep -o 'lua_memory_mb.* is "[0-9]*"' "$REMOTE_LOG")"
grep -qE 'lua_time_ms(.\[m)?"? is "40"' "$REMOTE_LOG" && ok "server text cannot raise lua_time_ms" || bad "lua_time_ms changed by server text"
grep -qE 'cl_lua_enable(.\[m)?"? is "1"' "$REMOTE_LOG" && ok "server text cannot turn mods off" || bad "cl_lua_enable changed by server text"
first_victim=$(grep -n "VICTIM ran" "$REMOTE_LOG" | head -1 | cut -d: -f1)
marker=$(grep -n "REMOTE local enable next" "$REMOTE_LOG" | grep -v echo | head -1 | cut -d: -f1)
[ "$(grep -c 'VICTIM ran' "$REMOTE_LOG")" = 1 ] && [ -n "$marker" ] && [ "${first_victim:-0}" -gt "$marker" ] \
	&& ok "server text cannot enable a mod (also after disconnect and from defer); the user still can" \
	|| bad "victim ran $(grep -c 'VICTIM ran' "$REMOTE_LOG") time(s), first at line ${first_victim:-none}, local enable at ${marker:-none}"
rhas "REMOTE FAIL plain ran" && bad "server text ran a mod command" || ok "server text cannot run a mod command"
rhas "REMOTE open ran" && ok "a command the mod opened to servers runs" || bad "fromserver command did not run"
rhas "REMOTE plain-local ran" && ok "mod commands run locally once the server's text is gone" || bad "local mod command refused"
[ ! -e "$RUD/luastate/mods/grants/remote_cmd.txt" ] && ok "server text cannot grant permissions" || bad "server text wrote a grant"

# --- 1b: crash-loop protection ------------------------------------------------
# Kill the engine (SIGKILL) while a mod's init.lua runs, then start again.
CUD="$WORK/crash"
mkdir -p "$CUD/mods" "$CUD/data"
cp -r "$ROOT/tests/lua-sandbox/crashloop/." "$CUD/mods/"
printf 'hang_init\n' > "$CUD/mods/enabled.txt"
CRASH_LOG="$WORK/crash1.log"
# the engine itself in the background (not under timeout, whose death would
# leave it running), with a watchdog
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy "$ENGINE" -xonotic -nosound \
	-basedir "$BASEDIR" -userdir "$CUD" +vid_width 640 +vid_height 480 +vid_fullscreen 0 > "$CRASH_LOG" 2>&1 &
PID=$!
( sleep 90; kill -9 $PID 2>/dev/null ) &
WATCHDOG=$!
for _ in $(seq 1200); do grep -q "CRASH hang_init loading" "$CRASH_LOG" && break; kill -0 $PID 2>/dev/null || break; sleep 0.05; done
kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null
kill $WATCHDOG 2>/dev/null; wait $WATCHDOG 2>/dev/null
grep -q "Lua mod hang_init stopped" "$CRASH_LOG" && bad "hang_init finished loading before the kill (the test kills too late)"
grep -q "CRASH hang_init loading" "$CRASH_LOG" && ok "killed the engine while hang_init loaded" || bad "hang_init never started loading"
[ "$(cat "$CUD/luastate/mods/loading.txt" 2>/dev/null)" = hang_init ] && ok "the crash marker names hang_init" || bad "no crash marker after the kill"
cat > "$CUD/data/crash.cfg" <<'EOF'
defer 3 "lua_mods"
defer 4 "echo CRASH re-enable next"
defer 4.2 "lua_mod_enable hang_init"
defer 7 "lua_mods"
defer 8 "quit"
EOF
CRASH2_LOG="$WORK/crash2.log"
run_engine "$CRASH2_LOG" -userdir "$CUD" +exec crash.cfg
[ "$?" = 0 ] && ok "restart after the kill exited cleanly" || bad "restart after the kill did not exit cleanly"
grep -q "did not finish loading last time" "$CRASH2_LOG" && ok "the mod is shown off with the reason" || bad "no 'did not finish loading' reason"
first_load=$(grep -n "CRASH hang_init loading" "$CRASH2_LOG" | head -1 | cut -d: -f1)
reenable=$(grep -n "CRASH re-enable next" "$CRASH2_LOG" | grep -v echo | head -1 | cut -d: -f1)
[ -n "$reenable" ] && [ "${first_load:-0}" -gt "$reenable" ] && ok "not loaded at startup; loads again when the user turns it on" \
	|| bad "hang_init loaded at line ${first_load:-none}, re-enable at ${reenable:-none}"
grep -qE "Lua mod hang_init stopped: .*(time limit|instruction limit)" "$CRASH2_LOG" && ok "re-enabled mod stopped by its limit" || bad "re-enabled mod not stopped"
[ ! -e "$CUD/luastate/mods/loading.txt" ] && ok "crash marker cleared after the load ended" || bad "crash marker left behind"

if [ "$fail" != 0 ]; then
	echo "--- engine log (Lua lines) ---"
	grep -aE "Lua|SANDBOX|PERMISSIONS|STORE|LEGACY|NS |FLOOD|BOUNDS|\[" "$LOG" | tail -80
	grep -aE "Lua|PERMISSIONS" "$SERVER_LOG" | tail -40
	grep -aE "Lua|REMOTE|VICTIM|lua_|refused|is \"" "$REMOTE_LOG" | tail -40
	grep -aE "Lua|CRASH|did not" "$CRASH_LOG" "$CRASH2_LOG" | tail -20
	exit 1
fi
echo "all Lua sandbox checks passed"
