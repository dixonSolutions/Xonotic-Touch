#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# What the engine prints over play after a join: player chat, and none of the
# game's own lines ("connected", "is now playing", frags), which stay in the
# console (TouchUIB_ChatboxMask in engine/darkplaces/touch_uib.c, the
# touch_chat_system_lines cvar). Real offscreen clients; the feed is read back
# with uib_chatfeed, which lists the console lines the engine would draw.
#
#   local       a map started in the client with bots joining
#   stock       the same with touch_chat_system_lines 1: the game's lines are back
#               (and the check can see them at all)
#   connect     a dedicated server with bots, the client connects
#   download    the same server offers a pack over HTTP (sv_curl_serverpackages):
#               the client downloads it, joins, and the feed is the same
#
#   BASEDIR=<Godnotic basedir> scripts/test-notify-lines.sh
# Environment: ENGINE (default build/bin/xonotic; also the server, -dedicated),
# LIBS (extra LD_LIBRARY_PATH), OUT (keep the logs).
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
if [ ! -x "$ENGINE" ] || [ ! -d "${BASEDIR:-/nonexistent}/data" ]; then
	echo "SKIP: set ENGINE and BASEDIR"
	exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-notify.XXXXXX")"
cleanup() {
	jobs -p | xargs -r kill 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
unset DISPLAY WAYLAND_DISPLAY
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

server() { # dir port extra...
	local d=$1 port=$2 n; shift 2
	mkdir -p "$d/data"
	"$ENGINE" -dedicated -xonotic -basedir "$BASEDIR" -userdir "$d" +set port "$port" +set sv_public 0 +set g_maplist stormkeep \
		+set bot_number 2 +set minplayers 0 "$@" +map stormkeep > "$d.log" 2>&1 &
	for n in $(seq 1 900); do grep -q "Server spawned" "$d.log" 2> /dev/null && break; sleep 0.1; done
	sleep 1
}
# client dir "first command" extra...: starts an offscreen client that runs the
# first command, then polls data/step.cfg every half second (a step ends the
# polling by redefining xt_poll)
client() {
	local d=$1 first=$2; shift 2
	mkdir -p "$d/data"
	printf 'seta touch_profile_done 1\nseta _cl_name Tester\nseta cl_allow_uid2name 0\n' > "$d/data/autoexec.cfg"
	: > "$d/data/step.cfg"
	printf 'alias xt_poll "exec step.cfg; defer 0.5 xt_poll"\ndefer 2 "%s"\ndefer 3 xt_poll\ndefer 170 quit\n' "$first" > "$d/data/run.cfg"
	timeout 200 "$ENGINE" -xonotic -nosound -basedir "$BASEDIR" -userdir "$d" +vid_fullscreen 0 +vid_width 640 +vid_height 360 \
		+set _touch_assets_ready 1 +set touch_joinfail_dialog 0 "$@" +exec run.cfg > "$d.log" 2>&1 &
}
waitfor() { # log pattern seconds
	local n
	for n in $(seq 1 $(($3 * 10))); do grep -aq "$2" "$1" 2> /dev/null && return 0; sleep 0.1; done
	return 1
}
# feed dir label: after the join, say something, then read the feed back
feed() {
	local d=$1 label=$2
	if ! waitfor "$d.log" "Tester.*connected" 120; then bad "$label: the client never joined (log: $d.log)"; return 1; fi
	# the bots joined before the client: their lines are already printed
	sleep 3
	# say something once (the polling moves on to step2.cfg), wait for the
	# server to echo it, then read the feed back
	: > "$d/data/step2.cfg"
	printf 'alias xt_poll "exec step2.cfg; defer 0.5 xt_poll"\nsay notify-test-chat-%s\n' "$label" > "$d/data/step.cfg"
	if ! waitfor "$d.log" "notify-test-chat-$label" 60; then bad "$label: the chat never came back from the server (log: $d.log)"; return 1; fi
	sleep 1
	printf 'alias xt_poll ""\nuib_chatfeed\ndefer 1 quit\n' > "$d/data/step2.cfg"
	if ! waitfor "$d.log" "CHATFEED-END" 60; then bad "$label: no uib_chatfeed answer (log: $d.log)"; return 1; fi
	sed 's/\x1b\[[0-9;]*m//g' "$d.log" | grep -a "^CHATFEED " > "$d.feed"
	return 0
}
port=$((26900 + RANDOM % 400))

# --- local: a map started in the client, bots joining
d="$WORK/local"; mkdir -p "$d"
client "$d/c" "map stormkeep" +set bot_number 2 +set minplayers 0
if feed "$d/c" local; then
	if grep -aq "notify-test-chat-local" "$d/c.feed"; then ok "local: the player's chat is in the feed"; else bad "local: the player's chat is missing from the feed"; fi
	if grep -aqi "connected\|is now playing" "$d/c.feed"; then bad "local: the game's own lines are in the feed: $(grep -ai 'connected\|is now playing' "$d/c.feed" | head -2 | tr '\n' '|')"
	else ok "local: no \"connected\" or \"is now playing\" lines over play ($(wc -l < "$d/c.feed") feed line(s))"; fi
	if sed 's/\x1b\[[0-9;]*m//g' "$d/c.log" | grep -aq "^Tester connected"; then ok "local: the console still has the \"connected\" lines"; else bad "local: the \"connected\" lines are gone from the console too"; fi
fi
wait

# --- stock: the switch brings the game's lines back
d="$WORK/stock"; mkdir -p "$d"
client "$d/c" "map stormkeep" +set bot_number 2 +set minplayers 0 +set touch_chat_system_lines 1
if feed "$d/c" stock; then
	if grep -aq "connected" "$d/c.feed" && grep -aq "notify-test-chat-stock" "$d/c.feed"; then ok "stock: with touch_chat_system_lines 1 the feed has the game's lines and the chat"
	else bad "stock: touch_chat_system_lines 1 did not bring the game's lines back (feed: $d/c.feed)"; fi
fi
wait

# --- connect: a dedicated server with bots
d="$WORK/connect"; mkdir -p "$d"
server "$d/sv" $port
client "$d/c" "connect 127.0.0.1:$port"
if feed "$d/c" connect; then
	if grep -aq "notify-test-chat-connect" "$d/c.feed"; then ok "connect: the player's chat is in the feed"; else bad "connect: the player's chat is missing from the feed"; fi
	if grep -aqi "connected\|is now playing" "$d/c.feed"; then bad "connect: the game's own lines are in the feed: $(grep -ai 'connected\|is now playing' "$d/c.feed" | head -2 | tr '\n' '|')"
	else ok "connect: no \"connected\" or \"is now playing\" lines over play"; fi
fi
wait $(jobs -p | tail -n +2) 2> /dev/null
kill %1 2> /dev/null; wait 2> /dev/null

# --- download: the server offers a pack over HTTP
port=$((port + 1)); hport=$((port + 1000)); d="$WORK/download"; mkdir -p "$d/web" "$d/sv/data"
python3 -I - "$d/web/zzz-notifytest.pk3" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    z.writestr("notifytest.txt", "a pack the client does not have\n")
PY
cp "$d/web/zzz-notifytest.pk3" "$d/sv/data/"
(cd "$d/web" && exec python3 -m http.server --bind 127.0.0.1 "$hport" > "$d/http.log" 2>&1) &
server "$d/sv" $port +set sv_curl_defaulturl "http://127.0.0.1:$hport/" +set sv_curl_serverpackages notifytest.txt
client "$d/c" "connect 127.0.0.1:$port" +set cl_curl_enabled 1
if feed "$d/c" download; then
	if find "$d/c" -name 'zzz-notifytest.pk3' | grep -q .; then ok "download: the client downloaded the server's pack"; else bad "download: the pack was not downloaded (log: $d/c.log)"; fi
	if grep -aq "notify-test-chat-download" "$d/c.feed"; then ok "download: the player's chat is in the feed"; else bad "download: the player's chat is missing from the feed"; fi
	if grep -av "notify-test-chat-download" "$d/c.feed" | grep -aqi "connected\|is now playing\|download"; then bad "download: system lines in the feed: $(head -3 "$d/c.feed" | tr '\n' '|')"
	else ok "download: no system or download lines over play"; fi
fi

[ $fail = 0 ] && echo "notify-lines: all checks passed"
exit $fail
