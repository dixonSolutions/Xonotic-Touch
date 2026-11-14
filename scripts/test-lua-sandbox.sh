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
defer 1 "lua_mod_permission scoped grant all"
defer 2 "lua_scoped_probe"
defer 3 "lua_mod_permission scoped grant engine_full"
defer 4 "lua_scoped_probe"
defer 5 "lua_mod_permission scoped revoke engine_full"
defer 6 "lua_scoped_probe"
defer 7 "lua_mod_permission scoped grant gamerules"
defer 8 "_cl_lua_permissions"
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
# API 3 tiers: the old scopes are standard (no grant), engine_full is a switch
for p in exec storage cvar_write; do has "TIERS $p allowed" && ok "standard tier: $p without a grant" || bad "standard tier: $p denied"; done
has "TIERS exec ran" && ok "standard tier: the allowlisted command ran" || bad "allowlisted command did not run"
has "TIERS engine denied" && ok "engine_full not granted at start" || bad "engine_full granted without a grant"
[ "$(grep -c 'TIERS probe engine denied' "$LOG")" = 2 ] && ok "\"grant all\" does not grant engine_full; revoke takes it away" || bad "engine_full after grant all / revoke: $(grep -c 'TIERS probe engine denied' "$LOG") denials"
[ "$(grep -c 'TIERS probe engine allowed' "$LOG")" = 1 ] && ok "engine_full granted (a local mod: one step)" || bad "engine_full grant did not take effect"
grep -q 'TIERS private nil' "$LOG" && ok "engine_full: private cvars stay hidden" || bad "engine_full read a private cvar"
[ "$(grep -c 'TIERS allowlist PASS' "$LOG")" = 3 ] && ok "the standard tier keeps the command allowlist" || bad "allowlist check failed"
grep -q 'does not declare the gamerules tier' "$LOG" && ok "a tier the manifest does not declare cannot be granted" || bad "an unrequested tier was granted"
grep -q $'scoped\tv1\tstandard,engine_full\tstandard' "$LOG" && ok "machine-readable status lists the tiers" || bad "permission status wrong"
[ "$(cat "$G/scoped.txt")" = 'v1 0' ] && ok "revocation persisted" || bad "revocation was not persisted ($(cat "$G/scoped.txt"))"
grep -q 'PERMISSIONS PASS malformed grant file' "$LOG" && ok "a malformed grant file leaves the standard tier only" || bad "malformed grant file broke the mod"
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
# 2: legacy mods (no mod.txt) run at the standard tier
for p in exec storage cvar_write cvar_read state display events; do has "LEGACY $p allowed" && ok "legacy mod: $p (standard tier)" || bad "legacy mod: $p not allowed"; done
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
has "FLOOD created 64" && ok "own cvars capped at 64 per mod" || bad "own cvar cap: $(grep -o 'FLOOD created [0-9]*' "$LOG")"
CFG="$UD/data/config.cfg"
if [ -f "$CFG" ] && grep -q lua_escape_test "$CFG"; then
	[ "$(grep -c lua_flood_ "$CFG")" -le 64 ] && ok "own cvars archived, at most 64 ($(grep -c lua_flood_ "$CFG"))" || bad "more than 64 own cvars archived"
else
	bad "config.cfg missing or without the storage-granted mod's cvar"
fi
# 7: unload at quit
has "SANDBOX atquit unload ran" && ok "unload event runs at quit" || bad "unload did not run at quit"

# Declared x granted: what a mod declares in mod.txt bounds what it can be granted
# and what it can call, whatever the grant file says. Fixtures written here.
DUD="$WORK/declared"
mkdir -p "$DUD/mods" "$DUD/data" "$DUD/luastate/mods/grants"
mkmod() { # name, mod.txt text ("" = no mod.txt)
	mkdir -p "$DUD/mods/$1"
	[ -n "$2" ] && printf '%s\n' "$2" > "$DUD/mods/$1/mod.txt"
	cat > "$DUD/mods/$1/init.lua" <<LUA
xt.command("probe", function()
  local ok, err = pcall(xt.engine.cvar, "hostname")
  print("DECL $1 " .. (ok and "allowed" or ("denied: " .. tostring(err))))
end, "tries an engine_full call")
LUA
}
mkmod decl_std $'api: 3\nrealm: client\npermissions: standard'
mkmod nomani ""
mkmod decl_eng $'api: 3\nrealm: client\npermissions: standard, engine_full\nreason.engine_full: tests the switch'
mkmod decl_new $'api: 3\nrealm: client\npermissions: standard, internet, engine_full'
mkmod decl_keep $'api: 3\nrealm: client\npermissions: standard, internet, engine_full'
printf 'decl_std\nnomani\ndecl_new\n' > "$DUD/mods/enabled.txt"
# grant bits written by hand for tiers the mod does not declare
printf 'v1 4001\n' > "$DUD/luastate/mods/grants/decl_std.txt"
printf 'v1 7001\n' > "$DUD/luastate/mods/grants/nomani.txt"
# an update that added engine_full: internet was granted before, engine_full is new
printf 'v1 2000\n' > "$DUD/luastate/mods/grants/decl_new.txt"
# a turn-on screen whose switches already granted internet: "with keep" settles the rest as off
printf 'v1 2000\n' > "$DUD/luastate/mods/grants/decl_keep.txt"
cat > "$DUD/data/declared.cfg" <<'CFG'
defer 2 "lua_decl_std_probe"
defer 2.5 "lua_nomani_probe"
defer 3 "lua_mod_permission decl_std grant engine_full"
defer 3.5 "lua_mod_permission nomani grant engine_full"
defer 4 "lua_mod_permission nomani"
defer 4.5 "_cl_lua_permissions"
defer 5 "lua_mod_enable decl_eng with engine_full"
defer 6 "lua_decl_eng_probe"
defer 6.5 "lua_mod_permission decl_eng revoke engine_full"
defer 7 "lua_decl_eng_probe"
defer 7.5 "lua_mod_permission decl_new decline engine_full"
defer 7.7 "lua_mod_enable decl_keep with keep"
defer 8 "_cl_lua_permissions"
defer 8.5 "lua_mod_info decl_new"
defer 9 "quit"
CFG
DLOG="$WORK/declared.log"
run_engine "$DLOG" -userdir "$DUD" +cl_lua_enable 1 +exec declared.cfg
[ "$?" = 0 ] && ok "declared-permissions run exited cleanly" || bad "declared-permissions run did not exit cleanly"
dhas() { grep -qaE -- "$1" "$DLOG"; }
dhas 'DECL decl_std denied: .*needs the engine_full tier' && ok "an undeclared call is denied though the grant file has the bit" || bad "undeclared call with a hand-written grant: $(grep -a 'DECL decl_std' "$DLOG")"
dhas 'DECL nomani denied: .*no mod.txt' && ok "a folder without mod.txt cannot call beyond standard, whatever its grant file says" || bad "manifest-less call: $(grep -a 'DECL nomani' "$DLOG")"
dhas 'decl_std does not declare the engine_full tier' && ok "a grant of an undeclared tier is refused with a message" || bad "undeclared grant not refused"
dhas 'nomani declares nothing \(no mod.txt\)' && ok "a manifest-less folder cannot be raised" || bad "manifest-less grant not refused"
dhas 'nomani, API legacy; declares nothing \(no mod.txt\): standard only' && ok "a manifest-less folder is listed as declaring nothing" || bad "manifest-less listing: $(grep -a 'Lua mod nomani' "$DLOG")"
[ "$(cat "$DUD/luastate/mods/grants/decl_std.txt")" = 'v1 4001' ] && ok "a refused grant writes nothing" || bad "refused grant rewrote the file"
grep -qaP '^nomani\tnone\tstandard\t' "$DLOG" && ok "machine-readable status: mode none for a manifest-less folder" || bad "nomani status: $(grep -aP '^nomani\t' "$DLOG")"
dhas 'DECL decl_eng allowed' && ok "turning a mod on with a declared tier grants it (lua_mod_enable ... with)" || bad "enable with engine_full: $(grep -a 'DECL decl_eng' "$DLOG")"
dhas 'DECL decl_eng denied: .*not turned on' && ok "a revoke takes effect on the next call, no restart" || bad "revoke not live: $(grep -a 'DECL decl_eng' "$DLOG")"
grep -qaP '^decl_new\tv1\tstandard,internet,engine_full\tstandard,internet\tengine_full$' "$DLOG" && ok "an update that declares more keeps the old grant and asks about the new tier only" || bad "update status: $(grep -aP '^decl_new\t' "$DLOG" | head -1)"
grep -qaP '^decl_new\tv1\tstandard,internet,engine_full\tstandard,internet\t?$' "$DLOG" && ok "decline settles the new tier, off, and keeps the old grant" || bad "decline status: $(grep -aP '^decl_new\t' "$DLOG" | tail -1)"
grep -qaP '^decl_keep\tv1\tstandard,internet,engine_full\tstandard,internet\t?$' "$DLOG" && [ "$(cat "$DUD/luastate/mods/grants/decl_keep.txt")" = 'v1 2000' ] && ok "lua_mod_enable ... with keep turns on with the grants as they are and settles the rest" || bad "with keep: $(grep -aP '^decl_keep\t' "$DLOG" | tail -1) / $(cat "$DUD/luastate/mods/grants/decl_keep.txt")"
[ "$(cat "$DUD/luastate/mods/grants/decl_new.txt")" = 'v1 2000' ] && ok "decline keeps the grant file as it was" || bad "decline changed grants ($(cat "$DUD/luastate/mods/grants/decl_new.txt"))"

# Exercise the server realm in a local match too, with an independent grant store.
mkdir -p "$UD/plugins" "$UD/luastate/plugins/grants"
cp -r "$ROOT/tests/lua-sandbox/plugins/scoped_server" "$UD/plugins/"
printf 'scoped_server\n' > "$UD/plugins/enabled.txt"
printf 'v1 2\n' > "$UD/luastate/plugins/grants/scoped_server.txt"
# grant with sv_lua_mod_permission, revoke with its old name sv_lua_plugin_permission
cat > "$UD/data/server-permissions.cfg" <<'EOF'
map stormkeep
defer 3 "sv_lua_scoped_server_probe"
defer 4 "sv_lua_mod_permission scoped_server grant gamerules"
defer 5 "sv_lua_scoped_server_probe"
defer 6 "sv_lua_plugin_permission scoped_server revoke gamerules"
defer 7 "sv_lua_scoped_server_probe"
defer 9 "quit"
EOF
SERVER_LOG="$WORK/server.log"
run_engine "$SERVER_LOG" -userdir "$UD" +exec server-permissions.cfg
[ "$?" = 0 ] && ok "server realm exited cleanly" || bad "server realm did not exit cleanly"
[ "$(grep -c 'SERVER PERMISSIONS denied' "$SERVER_LOG")" = 2 ] && ok "server gamerules tier denied before grant and after revoke" || bad "server gamerules revocation failed"
[ "$(grep -c 'SERVER PERMISSIONS allowed' "$SERVER_LOG")" = 1 ] && ok "server gamerules grant worked" || bad "server gamerules grant failed"
grep -q 'SERVER PERMISSIONS FAIL' "$SERVER_LOG" && bad "server permission fixture failed"
[ "$(cat "$UD/luastate/plugins/grants/scoped_server.txt")" = 'v1 0' ] && ok "server grant stored separately" || bad "server grant storage wrong ($(cat "$UD/luastate/plugins/grants/scoped_server.txt"))"

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
        b"lua_memory_mb 200\n"
        b"lua_time_ms 900\n"
        b"cl_lua_enable 0\n"
        b"lua_remote_cmd_plain\n"
        b"lua_remote_cmd_open\n"
        b"lua_mod_permission remote_cmd grant exec\n"
        b"lua_mod_permission remote_cmd grant engine_full\n"
        b"lua_mod_permission remote_cmd grant engine_full confirm\n"
        b"sv_lua_plugin_permission anything grant exec\n"
        b"sv_lua_mod_permission anything grant exec\n"
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
grep -qE 'lua_memory_mb(.\[m)?"? is "64"' "$REMOTE_LOG" && ok "server text cannot change lua_memory_mb" || bad "lua_memory_mb changed by server text: $(grep -o 'lua_memory_mb.* is "[0-9]*"' "$REMOTE_LOG")"
grep -qE 'lua_time_ms(.\[m)?"? is "100"' "$REMOTE_LOG" && ok "server text cannot change lua_time_ms" || bad "lua_time_ms changed by server text"
grep -qE 'cl_lua_enable(.\[m)?"? is "1"' "$REMOTE_LOG" && ok "server text cannot turn mods off" || bad "cl_lua_enable changed by server text"
first_victim=$(grep -n "VICTIM ran" "$REMOTE_LOG" | head -1 | cut -d: -f1)
marker=$(grep -n "REMOTE local enable next" "$REMOTE_LOG" | grep -v echo | head -1 | cut -d: -f1)
[ "$(grep -c 'VICTIM ran' "$REMOTE_LOG")" = 1 ] && [ -n "$marker" ] && [ "${first_victim:-0}" -gt "$marker" ] \
	&& ok "server text cannot enable a mod (also after disconnect and from defer); the user still can" \
	|| bad "victim ran $(grep -c 'VICTIM ran' "$REMOTE_LOG") time(s), first at line ${first_victim:-none}, local enable at ${marker:-none}"
rhas "REMOTE FAIL plain ran" && bad "server text ran a mod command" || ok "server text cannot run a mod command"
rhas "REMOTE open ran" && ok "a command the mod opened to servers runs" || bad "fromserver command did not run"
rhas "REMOTE plain-local ran" && ok "mod commands run locally once the server's text is gone" || bad "local mod command refused"
[ ! -e "$RUD/luastate/mods/grants/remote_cmd.txt" ] && ok "server text cannot grant tiers, engine_full included (confirm or not)" || bad "server text wrote a grant"

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
