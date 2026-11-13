#!/bin/bash
# Per-call cost of the Lua hook bridge on the hottest hooks (trivial handlers,
# tests/lua-mods/bench), in a headless bot match: prints sv_lua_hooks and
# lua_hooks (microseconds per call, marshalling included) and lua_mod_top.
#
#   BASEDIR=<basedir> scripts/bench-lua-hooks.sh [seconds]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
SECS="${1:-40}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-bench.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
UD="$WORK/home"
mkdir -p "$UD/mods" "$UD/plugins" "$UD/data"
cp -r "$ROOT/tests/lua-mods/bench/bench_sv" "$UD/plugins/"
cp -r "$ROOT/tests/lua-mods/bench/bench_cl" "$UD/mods/"
echo bench_sv > "$UD/plugins/enabled.txt"
echo bench_cl > "$UD/mods/enabled.txt"
cat > "$UD/data/bench.cfg" <<CFG
set bot_number 7
set skill 8
set g_warmup 0
set slowmo 2
map stormkeep
defer 10 "togglemenu 0"
defer 12 "cmd join"
defer 13 "togglemenu 0"
defer 15 "sv_lua_hooks_resetstats; lua_hooks_resetstats"
defer $((15 + SECS)) "sv_lua_hooks; lua_hooks; lua_mod_top"
defer $((16 + SECS)) "quit"
CFG
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout -k 10 $((60 + SECS)) "$ENGINE" -xonotic -nosound \
	-basedir "$BASEDIR" -userdir "$UD" +vid_width 1280 +vid_height 720 +vid_fullscreen 0 +exec bench.cfg > "$WORK/log" 2>&1
grep -aE "us each|hooks \(docs|bench_|cpu ms" "$WORK/log" | sed 's/\x1b\[[0-9;]*m//g'
