#!/bin/bash
# The example Lua mod and plugins in a real local match, headless (SDL offscreen).
#
# Enables the hud_clock client mod and the welcome + underdog server plugins from
# touch/lua/examples in a fresh userdir, starts a bot match, then checks that the
# plugins saw the game's events (connect greeting, deaths, damage scaling, the
# chat filter, "cmd stats") and that the client mod drew its panel. Leaves a
# screenshot in $OUT (default: a temp dir that is removed) when OUT is set.
#
#   BASEDIR=<dir with data/*.pk3 and the Touch xonotic-data.pk3dir with progs.dat> scripts/test-lua-match.sh
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
BASEDIR="${BASEDIR:?set BASEDIR to a Xonotic basedir (data/*.pk3)}"
MAP="${MAP:-stormkeep}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-lua-match.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
UD="$WORK/home"
mkdir -p "$UD/mods" "$UD/plugins" "$UD/data"
cp -r "$ROOT/touch/lua/examples/mods/hud_clock" "$ROOT/touch/lua/examples/mods/settings_panel" "$UD/mods/"
cp -r "$ROOT/touch/lua/examples/plugins/welcome" "$ROOT/touch/lua/examples/plugins/underdog" "$UD/plugins/"
printf 'hud_clock\nsettings_panel\n' > "$UD/mods/enabled.txt"
printf 'underdog\nwelcome\n' > "$UD/plugins/enabled.txt"
# The examples declare API 1 permissions (mod.txt), which start ungranted; grant
# what each one asks for, as lua_mod_permission / sv_lua_plugin_permission
# would (<userdir>/luastate/<realm>/grants/<name>.txt: "v1 <hex mask>").
mkdir -p "$UD/luastate/mods/grants" "$UD/luastate/plugins/grants"
printf 'v1 56f\n' > "$UD/luastate/mods/grants/hud_clock.txt"         # events display commands cvar_read cvar_write storage players state
printf 'v1 ff\n' > "$UD/luastate/mods/grants/settings_panel.txt"     # events display input commands cvar_read cvar_write exec storage
printf 'v1 10d\n' > "$UD/luastate/plugins/grants/underdog.txt"       # events players cvar_read cvar_write
printf 'v1 701\n' > "$UD/luastate/plugins/grants/welcome.txt"        # events players state messages

cat > "$UD/data/match.cfg" <<EOF
set sv_cheats 1
set bot_number 3
set skill 8
set g_warmup 0
set slowmo 2
map $MAP
defer 10 "togglemenu 0"
defer 12 "togglemenu 0"
defer 13 "cmd join"
defer 14 "togglemenu 0"
defer 16 "togglemenu 0"
defer 40 "cmd stats"
defer 41 "say this has badword in it"
defer 42 "say hello everyone"
defer 42.5 "lua_settings_panel_open"
defer 43 "screenshot lua_match.png"
defer 44 "sv_lua_plugins"
defer 44.5 "lua_mods"
defer 45 "sv_lua_plugin_info welcome"
defer 46 "quit"
EOF

LOG="$WORK/out.log"
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout -k 10 180 "$ENGINE" -xonotic -nosound \
	-basedir "$BASEDIR" -userdir "$UD" +vid_width 1280 +vid_height 720 +vid_fullscreen 0 \
	+exec match.cfg > "$LOG" 2>&1
RC=$?
[ -n "${OUT:-}" ] && mkdir -p "$OUT" && cp "$LOG" "$OUT/match.log"

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
has() { grep -aE "$1" "$LOG" > /dev/null; }

[ "$RC" = 0 ] && ok "engine exited cleanly" || bad "engine exit code $RC"
has "Lua plugin welcome loaded" && ok "welcome plugin loaded" || bad "welcome plugin not loaded"
has "Lua plugin underdog loaded" && ok "underdog plugin loaded" || bad "underdog plugin not loaded"
has "Lua mod hud_clock loaded" && ok "hud_clock mod loaded" || bad "hud_clock mod not loaded"
has "Lua mod settings_panel loaded" && ok "settings_panel mod (xt.ui) loaded" || bad "settings_panel mod not loaded"
has "welcome: new map $MAP" && ok "map event" || bad "no map event"
has "Welcome to $MAP" && ok "connect event: greeting reached the player" || bad "no greeting"
has "welcome: .* killed .* \(" && ok "death events: $(grep -ac 'welcome: .* killed' "$LOG") kills seen" || bad "no death events"
has "underdog: .* took [0-9]+ instead of [0-9]+" && ok "damage event scaled damage: $(grep -ao 'underdog: .*' "$LOG" | head -1)" || bad "no damage scaling"
has "You have [0-9]+ kills this map" && ok "clientcmd: cmd stats answered by the plugin" || bad "cmd stats not answered"
has "That word is not allowed here" && ok "chat event: filtered message held back" || bad "chat filter did not fire"
has "hello everyone" && ok "chat event: normal message went through" || bad "normal chat missing"
has "this has badword in it" && grep -a "badword in it" "$LOG" | grep -vq "say this" && bad "filtered message was still shown" || ok "filtered message not shown"
if has "stopped:"; then bad "a mod or plugin stopped:"; grep -a "stopped:" "$LOG"; fi

SHOT=$(find "$UD" -name 'lua_match*' | head -1)
if [ -n "$SHOT" ]; then
	ok "screenshot written"
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp "$SHOT" "$OUT/" && echo "     copied to $OUT/$(basename "$SHOT")"; fi
else
	bad "no screenshot"
fi

if [ "$fail" != 0 ]; then
	echo "--- engine log (tail) ---"
	grep -avE "Mod_LoadQ3Shaders" "$LOG" | tail -60
	exit 1
fi
echo "all Lua match checks passed"
