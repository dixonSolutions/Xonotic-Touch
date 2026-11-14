#!/bin/bash
# Leaving the game and coming back (docs/NETWORK.md, "Staying connected"):
# a dedicated server in one network namespace, a client in another, joined by
# a veth cable (unshare -rn). Android freezes or suspends an app it does not
# keep alive; here the client process is stopped (SIGSTOP) and continued, the
# worst case of that. Cases:
#   freeze5, freeze60     stopped 5 s / 60 s in a match: still connected, no reconnect
#   freeze-signon         stopped 5 s during the signon: the join completes
#   timeout               stopped past the server's net_messagetimeout (40 s
#                         here): the server drops it; on return the client
#                         notices within seconds and joins again by itself
#   newaddress            the client's address changes in a match (Wi-Fi to
#                         mobile data): it notices and joins again by itself
#   away                  touch_away 1 (Android's background): spectator after
#                         net_away_spectate (5 s here), back into the game on return
#   loss-freeze60         freeze60 with 2% loss
#   download              an HTTP pack download (2 MB/s cap) stopped for 10 s
#                         midway: it completes and the file is intact
#
#   BASEDIR=<Godnotic basedir> scripts/test-net-away.sh [CASE...]
# ENGINE, SERVER (default build/bin/xonotic, engine/darkplaces/darkplaces-dedicated), LIBS, OUT.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/build/bin/xonotic}"
SERVER="${SERVER:-$ROOT/engine/darkplaces/darkplaces-dedicated}"
BASEDIR="${BASEDIR:?set BASEDIR}"
MAP="${MAP:-stormkeep}"

if [ -z "${NET_TEST_INSIDE:-}" ]; then
	unshare -rn true 2> /dev/null || { echo "SKIP: no unprivileged network namespaces"; exit 77; }
	exec unshare -rn env NET_TEST_INSIDE=1 bash "$0" "$@"
fi
export LD_LIBRARY_PATH="${LIBS:+$LIBS:}${LD_LIBRARY_PATH:-}"
export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
WORK="$(mktemp -d "${TMPDIR:-/tmp}/godnotic-away.XXXXXX")"
cleanup() { jobs -p | xargs -r kill -CONT 2> /dev/null; jobs -p | xargs -r kill 2> /dev/null; [ -n "${OUT:-}" ] && mkdir -p "$OUT" && cp -r "$WORK"/. "$OUT/" 2> /dev/null; rm -rf "$WORK"; }
trap cleanup EXIT
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

ip link set lo up
unshare -n sleep 100000 & NSPID=$!
sleep 0.3
ip link add cl0 type veth peer name sv0
ip link set sv0 netns "$NSPID"
ip addr add 10.88.0.1/24 dev cl0
ip link set cl0 up
nsenter -t "$NSPID" -n sh -c 'ip link set lo up; ip addr add 10.88.0.2/24 dev sv0; ip link set sv0 up'
insv() { nsenter -t "$NSPID" -n "$@"; }

# start NAME "client cfg lines" "server args" -> CLPID, SVPID, logs in $WORK/NAME
start() {
	local name=$1 ccfg=$2 sargs=$3
	mkdir -p "$WORK/$name/cl/data" "$WORK/$name/sv/data"
	# poll.cfg runs every half second: the test writes commands into it
	printf '%b\nalias _ap "exec poll.cfg; defer 0.5 _ap"\n_ap\n' "$ccfg" > "$WORK/$name/cl/data/away.cfg"
	: > "$WORK/$name/cl/data/poll.cfg"
	printf 'alias _sp "exec next.cfg; defer 0.5 _sp"\n_sp\n' > "$WORK/$name/sv/data/svpoll.cfg"
	: > "$WORK/$name/sv/data/next.cfg"
	printf 'seta touch_profile_done 1\nseta _cl_name "Bench"\n' > "$WORK/$name/cl/data/autoexec.cfg"
	# shellcheck disable=SC2086
	insv "$SERVER" -dedicated -xonotic -basedir "$BASEDIR" -userdir "$WORK/$name/sv" +set port 26000 +set sv_public 0 \
		+set bot_number 2 +set minplayers 0 $sargs +map "$MAP" +exec svpoll.cfg > "$WORK/$name/server.log" 2>&1 & SVPID=$!
	for _ in $(seq 1 300); do grep -q "Server spawned" "$WORK/$name/server.log" 2> /dev/null && break; sleep 0.1; done
	"$ENGINE" -xonotic -basedir "$BASEDIR" -userdir "$WORK/$name/cl" +vid_fullscreen 0 +vid_width 480 +vid_height 300 \
		+set _touch_assets_ready 1 +exec away.cfg > "$WORK/$name/client.log" 2>&1 & CLPID=$!
}
finish() { # the client quits by itself; the server goes with it
	wait "$CLPID" 2> /dev/null
	pkill -f -- "-userdir $WORK/[a-z0-9-]*/sv" 2> /dev/null
	wait "$SVPID" 2> /dev/null
	sleep 0.5
}
wait_for() { # file pattern seconds
	local i
	for i in $(seq 1 $(($3 * 10))); do grep -q "$2" "$1" 2> /dev/null && return 0; sleep 0.1; done
	return 1
}
count() { grep -c "$2" "$1" 2> /dev/null || true; }
# a command for the running client (idempotent ones: it runs every 0.5 s until cleared)
say() { echo "$2" > "$WORK/$1/cl/data/poll.cfg"; sleep 1.2; : > "$WORK/$1/cl/data/poll.cfg"; }
svpid() { pgrep -f -- "-userdir $WORK/$1/sv" | head -1; }
# the server log after the client's last (re)join
since_join() { awk '/^Bench connected/{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}' "$WORK/$1/server.log"; }
wait_since() { # name pattern seconds: in the server log after the last join
	local i
	for i in $(seq 1 $(($3 * 2))); do since_join "$1" | grep -q "$2" && return 0; sleep 0.5; done
	return 1
}
# stop the client (or the server) past the server's timeout, then wait until
# the client has joined again by itself and is in the game
freeze_and_rejoin() { # name client|server seconds
	local name=$1 who=$2 secs=$3 pid
	pid=$CLPID; [ "$who" = server ] && pid=$(svpid "$name")
	kill -STOP "$pid"; sleep "$secs"; kill -CONT "$pid"
	for _ in $(seq 1 120); do [ "$(count "$WORK/$name/server.log" "^Bench connected")" -ge 2 ] && break; sleep 0.5; done
	# back in the slot it had (a server keeps a player who comes back from
	# the same address in the game, so there may be no "is now playing")
	[ "$(count "$WORK/$name/server.log" "^Bench connected")" -ge 2 ] || return 1
	say "$name" "togglemenu 0; cmd join"
	sleep 4
}
# away after the rejoin: spectator, then the clean disconnect that frees the slot
check_away() { # name label
	local name=$1 label=$2
	if wait_since "$name" "Bench.*is now.*spectating" 25; then ok "$name: $label: spectator after net_away_spectate"; else bad "$name: $label: not moved to the spectators"; fi
	if wait_for "$WORK/$name/server.log" 'Client "Bench" dropped (Disconnect by user)' 60 && grep -q "Left .* after .* in the background" "$WORK/$name/client.log"; then
		ok "$name: $label: left at net_away_limit, the server freed the slot, the client says why ($(grep -o 'Left .* background' "$WORK/$name/client.log" | head -1))"
	else
		bad "$name: $label: no clean disconnect at net_away_limit ($(grep 'Bench" dropped' "$WORK/$name/server.log" | tr '\n' ' '))"
	fi
}
AWAYCFG='set net_away_spectate 5\nset net_away_limit 30'
playing() { wait_for "$WORK/$1/server.log" "Bench.*is now playing" 60 || { bad "$1: the client never got into the game"; return 1; }; }
# join the game a few times over: on a loaded machine the signon may not be
# through by the first one (a join while playing does nothing)
JOIN='connect 10.88.0.2:26000\ndefer 10 "togglemenu 0; cmd join"\ndefer 20 "togglemenu 0; cmd join"\ndefer 28 "cmd join"'

freeze_case() { # name seconds when(match|signon) netem
	local name=$1 secs=$2 when=$3
	start "$name" "$JOIN\ndefer $((secs + 45)) quit" ""
	[ -n "${4:-}" ] && tc qdisc replace dev cl0 root netem $4
	if [ "$when" = signon ]; then
		wait_for "$WORK/$name/client.log" "Connection established" 30
	elif ! wait_for "$WORK/$name/server.log" "Bench.*is now playing" 60; then
		bad "$name: the client never got into the game (an overloaded machine?)"; finish; return
	fi
	sleep 1
	kill -STOP "$CLPID"; sleep "$secs"; kill -CONT "$CLPID"
	finish
	tc qdisc del dev cl0 root 2> /dev/null
	local est again
	est=$(count "$WORK/$name/client.log" "Connection established")
	again=$(count "$WORK/$name/client.log" "joining .* again")
	if [ "$est" = 1 ] && [ "$again" = 0 ] && ! grep 'Client "Bench" dropped' "$WORK/$name/server.log" | grep -vq "Disconnect by user"; then
		ok "$name: stopped $secs s ($when), still connected, no reconnect"
	elif grep -q 'Bench" dropped (Timed out)' "$WORK/$name/server.log" && [ "$again" -ge 1 ] && [ "$(count "$WORK/$name/server.log" "^Bench connected")" -ge 2 ]; then
		# longer than the server's net_messagetimeout (Xonotic: 30 s)
		ok "$name: stopped $secs s, the server timed it out, it joined again by itself"
	else
		bad "$name: $est connections, $again reconnects; server: $(grep 'Bench" dropped' "$WORK/$name/server.log" | tr '\n' ' ')"
	fi
	[ "$when" = signon ] && { [ "$(count "$WORK/$name/server.log" "^Bench connected")" -ge 1 ] && ok "$name: the join completed" || bad "$name: the join did not complete"; }
}

run() {
	case $1 in
		freeze5) freeze_case freeze5 5 match ;;
		freeze60) freeze_case freeze60 60 match ;;
		loss-freeze60) freeze_case loss-freeze60 60 match "delay 20ms loss 2%" ;;
		freeze-signon) freeze_case freeze-signon 5 signon ;;
		timeout)
			start timeout "$JOIN\ndefer 130 quit" "+set net_messagetimeout 40"
			wait_for "$WORK/timeout/server.log" "Bench.*is now playing" 60 || bad "timeout: the client never got into the game"; sleep 1
			kill -STOP "$CLPID"; sleep 50; kill -CONT "$CLPID"
			wait_for "$WORK/timeout/client.log" "joining .* again" 20 && t1=$(date +%s)
			wait_for "$WORK/timeout/server.log" "Bench.*is now playing.*\|^Bench connected" 0
			finish
			grep -q 'Bench" dropped (Timed out)' "$WORK/timeout/server.log" && ok "timeout: the server dropped the frozen client after net_messagetimeout" || bad "timeout: no server-side timeout"
			[ "$(count "$WORK/timeout/client.log" "Connection established")" -ge 2 ] && [ "$(count "$WORK/timeout/server.log" "^Bench connected")" -ge 2 ] \
				&& ok "timeout: back, the client noticed ($(grep -o 'Lost connection[^:]*' "$WORK/timeout/client.log" | head -1)) and joined again by itself" \
				|| bad "timeout: no automatic reconnect ($(grep -a 'Lost\|again\|Reconnect' "$WORK/timeout/client.log" | head -3 | tr '\n' ' '))" ;;
		newaddress)
			start newaddress "$JOIN\ndefer 70 quit" ""
			wait_for "$WORK/newaddress/server.log" "Bench.*is now playing" 40; sleep 2
			ip addr add 10.88.0.9/24 dev cl0; ip addr del 10.88.0.1/24 dev cl0
			finish
			ip addr add 10.88.0.1/24 dev cl0 2> /dev/null; ip addr del 10.88.0.9/24 dev cl0 2> /dev/null
			grep -q "address changed (10.88.0.1 -> 10.88.0.9)" "$WORK/newaddress/client.log" && ok "newaddress: the client saw its address change" || bad "newaddress: change not seen"
			[ "$(count "$WORK/newaddress/server.log" "^Bench connected")" -ge 2 ] && ok "newaddress: joined again from 10.88.0.9 by itself" || bad "newaddress: no reconnect" ;;
		away)
			start away "$JOIN\nset net_away_spectate 5\ndefer 30 \"touch_away 1\"\ndefer 45 \"touch_away 0\"\ndefer 60 quit" ""
			finish
			grep -q "going to the spectators" "$WORK/away/client.log" && grep -q "Bench.*is now.*spectating" "$WORK/away/server.log" \
				&& ok "away: spectator after 5 s in the background" || bad "away: not moved to the spectators"
			grep -q "Back: joining the game again" "$WORK/away/client.log" && [ "$(count "$WORK/away/server.log" "Bench.*is now playing")" -ge 2 ] \
				&& ok "away: back into the game on return" || bad "away: did not rejoin the game" ;;
		download)
			mkdir -p "$WORK/www"
			python3 - "$WORK/www/zzawaypack.pk3" <<'EOF'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_STORED) as z:
    for i in range(6):
        z.writestr("away/blob%d.bin" % i, os.urandom(4 << 20))
EOF
			insv python3 "$ROOT/tests/net/range_http.py" "$WORK/www" 8124 2048 2> "$WORK/http.log" & HP=$!
			sleep 0.5
			start download "curl --pak http://10.88.0.2:8124/zzawaypack.pk3\nset developer 1\ndefer 60 quit" ""
			sleep 5
			kill -STOP "$CLPID"; sleep 10; kill -CONT "$CLPID"
			finish; kill $HP 2> /dev/null
			cmp -s "$WORK/www/zzawaypack.pk3" "$WORK/download/cl/data/dlcache/zzawaypack.pk3" && grep -q "Download of .*: OK" "$WORK/download/client.log" \
				&& ok "download: stopped 10 s midway, completed, identical" || bad "download: $(grep -a 'Download of' "$WORK/download/client.log" | head -1)" ;;
		reconnect-away) # frozen past the server's timeout, joined again by itself, then away
			start reconnect-away "$JOIN\n$AWAYCFG\ndefer 240 quit" ""
			playing reconnect-away || { finish; return; }
			freeze_and_rejoin reconnect-away client 40 && ok "reconnect-away: rejoined by itself after a 40 s freeze" || bad "reconnect-away: did not get back into the game"
			say reconnect-away "touch_away 1"
			check_away reconnect-away "away after the reconnect"
			finish ;;
		svfreeze-away) # the server froze 36 s, the client timed out and joined again, then away
			start svfreeze-away "$JOIN\n$AWAYCFG\ndefer 240 quit" ""
			playing svfreeze-away || { finish; return; }
			freeze_and_rejoin svfreeze-away server 36 && ok "svfreeze-away: rejoined by itself after the server froze 36 s" || bad "svfreeze-away: did not get back into the game"
			say svfreeze-away "touch_away 1"
			check_away svfreeze-away "away after the reconnect"
			finish ;;
		away-freeze) # away first, then frozen past the timeout: joined again while away
			start away-freeze "$JOIN\nset net_away_spectate 5\nset net_away_limit 75\ndefer 240 quit" ""
			playing away-freeze || { finish; return; }
			say away-freeze "touch_away 1"
			wait_since away-freeze "Bench.*is now.*spectating" 25 && ok "away-freeze: spectator" || bad "away-freeze: not a spectator before the freeze"
			kill -STOP "$CLPID"; sleep 40; kill -CONT "$CLPID"
			for _ in $(seq 1 120); do [ "$(count "$WORK/away-freeze/server.log" "^Bench connected")" -ge 2 ] && break; sleep 0.5; done
			check_away away-freeze "reconnected while away"
			finish ;;
		mapchange-away) # away across a map change
			start mapchange-away "$JOIN\nset net_away_spectate 5\nset net_away_limit 45\ndefer 240 quit" ""
			playing mapchange-away || { finish; return; }
			say mapchange-away "touch_away 1"
			wait_since mapchange-away "Bench.*is now.*spectating" 25 && ok "mapchange-away: spectator" || bad "mapchange-away: not a spectator"
			echo "changelevel $MAP" > "$WORK/mapchange-away/sv/data/next.cfg"; sleep 0.8; : > "$WORK/mapchange-away/sv/data/next.cfg"
			for _ in $(seq 1 60); do [ "$(count "$WORK/mapchange-away/server.log" "^Bench connected")" -ge 2 ] && break; sleep 0.5; done
			check_away mapchange-away "after a map change"
			finish ;;
		manual-away) # away, then the player's own reconnect (connect again)
			start manual-away "$JOIN\nset net_away_spectate 5\nset net_away_limit 40\ndefer 240 quit" ""
			playing manual-away || { finish; return; }
			say manual-away "touch_away 1"
			wait_since manual-away "Bench.*is now.*spectating" 25 || bad "manual-away: not a spectator"
			echo "connect 10.88.0.2:26000" > "$WORK/manual-away/cl/data/poll.cfg"; sleep 0.8; : > "$WORK/manual-away/cl/data/poll.cfg"
			for _ in $(seq 1 60); do [ "$(count "$WORK/manual-away/server.log" "^Bench connected")" -ge 2 ] && break; sleep 0.5; done
			check_away manual-away "after a manual reconnect"
			finish ;;
		*) bad "unknown case $1" ;;
	esac
}

[ $# -gt 0 ] || set -- freeze5 freeze-signon away newaddress timeout download freeze60 loss-freeze60 reconnect-away svfreeze-away away-freeze mapchange-away manual-away
for c in "$@"; do run "$c"; done
[ $fail = 0 ] && echo "all away tests passed" || echo "some away tests FAILED"
exit $fail
