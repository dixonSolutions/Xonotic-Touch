#!/bin/bash
# A game on this device is local unless the player hosts it on the LAN, and a
# hosted game ends for everyone when its host leaves or quits (docs/LAN.md).
#
# Two network namespaces joined by a veth pair, as scripts/test-lan.sh:
#   host 10.77.0.2/24  -- the game under test (a listen server, offscreen)
#   peer 10.77.0.1/24  -- packet probes and joining clients
#
# 1. LAN off (maxplayers 4, a bot, a map; nothing armed): the host process has
#    no UDP socket on its game port, every probe (getinfo, getstatus,
#    getchallenge, connect) to every UDP port it does hold goes unanswered, the
#    LAN search of a second game finds nothing and a typed address times out.
# 2. LAN on (touch_lan_arm, a password): the port listens, getinfo answers with
#    the lock, a join without the password is refused, one with it gets in.
# 3. The host leaves the match: the client is told "The host ended the game",
#    is back at its menu, does not rejoin, and the port is free again.
# 4. LAN on without a password, the host quits the game: the same for the
#    client; the host process is gone and the port is free.
# 5. A host that goes through the menu (Create, LAN ticked): its address popup
#    opens by itself, closes when a second game joins, and the game then runs.
#
#   BASEDIR=<basedir with data/*.pk3 and the Touch xonotic-data.pk3dir> scripts/test-lan-private.sh
# Optional: ENGINE (default engine/darkplaces/darkplaces-sdl), OUT (keep logs),
# LD_LIBRARY_PATH for a libjpeg.so.62 the engine dlopens.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
BASEDIR="${BASEDIR:?set BASEDIR to a Xonotic basedir (data/*.pk3 plus the Touch pk3dir)}"
MAP="${MAP:-stormkeep}"
PASSWORD="hunter2 with spaces"

if [ -z "${LAN_TEST_INSIDE:-}" ]; then
	command -v unshare > /dev/null || { echo "SKIP: no unshare"; exit 77; }
	command -v ip > /dev/null || { echo "SKIP: no ip (iproute2)"; exit 77; }
	if ! unshare -rn true 2> /dev/null; then
		echo "SKIP: unprivileged user/network namespaces are not allowed here"
		exit 77
	fi
	exec unshare -rn env LAN_TEST_INSIDE=1 bash "$0" "$@"
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-lanpriv.XXXXXX")"
PIDS=()
cleanup() {
	for p in "${PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2> /dev/null; done
	[ -n "${NSPID:-}" ] && kill "$NSPID" 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp "$WORK"/*.log "$WORK"/*.txt "$WORK"/*/data/*.json "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

ip link set lo up
unshare -n sleep 900 & NSPID=$!
sleep 0.3
ip link add lanpeer type veth peer name lanhost
ip link set lanhost netns "$NSPID"
ip addr add 10.77.0.1/24 dev lanpeer
ip link set lanpeer up
nsenter -t "$NSPID" -n sh -c 'ip link set lo up; ip addr add 10.77.0.2/24 dev lanhost; ip link set lanhost up'
sleep 1
inhost() { nsenter -t "$NSPID" -n "$@"; }

run_engine() { # name ns(host|peer) seconds extra-args...
	local name=$1 where=$2 secs=$3
	shift 3
	local ud="$WORK/$name"
	mkdir -p "$ud/data"
	local cmd=(env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout "$secs" "$ENGINE" -xonotic -nosound
		-basedir "$BASEDIR" -userdir "$ud" +sys_colortranslation 2 +vid_width 640 +vid_height 400 +vid_fullscreen 0 "$@")
	if [ "$where" = host ]; then inhost "${cmd[@]}"; else "${cmd[@]}"; fi
}

waitlog() { # file pattern seconds
	local i
	for i in $(seq 1 $(($3 * 5))); do
		grep -aq -- "$2" "$1" 2> /dev/null && return 0
		sleep 0.2
	done
	return 1
}

# the host takes commands from hostpoll.cfg, read every second
mkdir -p "$WORK/host/data"
: > "$WORK/host/data/hostpoll.cfg"
STEP=0
hostcmd() { # commands...; waits until the host ran them
	STEP=$((STEP + 1))
	printf '%s\necho HOSTSTEP-%d\n' "$1" "$STEP" > "$WORK/host/data/hostpoll.cfg.new"
	mv "$WORK/host/data/hostpoll.cfg.new" "$WORK/host/data/hostpoll.cfg"
	waitlog "$WORK/host.log" "HOSTSTEP-$STEP" 60 || bad "the host did not run step $STEP"
	: > "$WORK/host/data/hostpoll.cfg"
}

cat > "$WORK/host/data/hostcfg.cfg" <<EOF
set hostname "Private test game"
set bot_number 1
set g_warmup 0
set sv_status_privacy 1
alias pollloop "exec hostpoll.cfg; defer 1 pollloop"
defer 2 pollloop
maxplayers 4
port 26000
map $MAP
EOF
run_engine host host 800 +exec hostcfg.cfg > "$WORK/host.log" 2>&1 &
HOSTJOB=$!
PIDS+=("$HOSTJOB")

# udp ports the host namespace has bound, and a probe of one from the peer
udp_ports() { inhost ss -Hlun 2> /dev/null | awk '{print $4}' | sed 's/.*://' | sort -un; }
probe() { # port -> prints REPLY <first bytes> or NOREPLY for each query
	python3 - "$1" << 'PY'
import socket, sys
port = int(sys.argv[1])
for q in (b"getinfo x", b"getstatus x", b"getchallenge", b"connect\\protocol\\darkplaces 3\\challenge\\AAAAAAAAAAA"):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    try:
        s.sendto(b"\xff\xff\xff\xff" + q, ("10.77.0.2", port))
        d = s.recvfrom(4096)[0]
        print("REPLY", q.split()[0].decode(), repr(d[:40]))
    except Exception as e:
        print("NOREPLY", q.split(b"\\")[0].split()[0].decode(), type(e).__name__)
    s.close()
PY
}

# ---------------------------------------------------------------- 1. LAN off
hostcmd "touch_lan_status" # the map is up when the poll loop runs
sleep 4
hostcmd "touch_lan_status"
grep -aq "Not hosting a game others can join" "$WORK/host.log" \
	&& ok "LAN off: the game says it is not hosting" || bad "LAN off: touch_lan_status: $(grep -a 'Hosting\|Join at' "$WORK/host.log" | tail -2)"
grep -aq "Server listening on address" "$WORK/host.log" \
	&& bad "LAN off: the local game opened a network socket: $(grep -a 'Server listening' "$WORK/host.log" | head -2 | tr '\n' ' ')" \
	|| ok "LAN off: no 'Server listening on address' (loopback socket only)"
PORTS="$(udp_ports | tr '\n' ' ')"
echo "     UDP ports held in the host namespace: ${PORTS:-none}"
case " $PORTS " in *" 26000 "*) bad "LAN off: UDP 26000 is bound";; *) ok "LAN off: UDP 26000 is not bound";; esac
: > "$WORK/probe-off.txt"
for p in 26000 $PORTS; do echo "port $p" >> "$WORK/probe-off.txt"; probe "$p" >> "$WORK/probe-off.txt"; done
if grep -q "^REPLY" "$WORK/probe-off.txt"; then
	bad "LAN off: a probe was answered: $(grep '^REPLY' "$WORK/probe-off.txt" | head -3 | tr '\n' ' ')"
else
	ok "LAN off: getinfo, getstatus, getchallenge and connect unanswered on $(grep -c '^port' "$WORK/probe-off.txt") port(s) ($(grep -c NOREPLY "$WORK/probe-off.txt") probes)"
fi
# a second game: the LAN search and a typed address
mkdir -p "$WORK/findoff/data"
cat > "$WORK/findoff/data/find.cfg" <<EOF
set net_slist_lan_only 1
defer 3 "net_slist"
defer 7 "net_slist"
defer 9 "connect 10.77.0.2:26000"
defer 24 "quit"
EOF
run_engine findoff peer 60 +exec find.cfg > "$WORK/findoff.log" 2>&1
grep -aq "10\.77\.0\.2:2600" "$WORK/findoff.log" && grep -aq "Private test game" "$WORK/findoff.log" \
	&& bad "LAN off: the LAN search found the local game" || ok "LAN off: the LAN search of another game finds nothing"
grep -aq "Connection established\|Connection accepted" "$WORK/findoff.log" \
	&& bad "LAN off: another game connected by address" || ok "LAN off: connect by address gets nowhere"

# ---------------------------------------------------------------- 2. LAN on, password
hostcmd "disconnect"
sleep 1
hostcmd "set sv_touch_lan_password \"$PASSWORD\"; set sv_public 0; touch_lan_arm; maxplayers 4; port 26000; map $MAP"
waitlog "$WORK/host.log" "Server listening on address 0.0.0.0:26000" 90 \
	&& ok "LAN on: the game listens on 26000" || { bad "LAN on: no listening socket"; tail -20 "$WORK/host.log"; }
sleep 3
hostcmd "touch_lan_status"
grep -aq "Join at: 10.77.0.2:26000" "$WORK/host.log" && ok "LAN on: $(grep -ao 'Join at: .*' "$WORK/host.log" | tail -1)" || bad "LAN on: no join address"
grep -aq "Join link: godnotic://join/10.77.0.2:26000" "$WORK/host.log" && ok "LAN on: join link given" || bad "LAN on: no join link"
probe 26000 > "$WORK/probe-on.txt"
grep -q "^REPLY getinfo" "$WORK/probe-on.txt" && ok "LAN on: getinfo answered" || bad "LAN on: getinfo unanswered: $(cat "$WORK/probe-on.txt" | tr '\n' ' ')"
grep -q "^REPLY getchallenge" "$WORK/probe-on.txt" && ok "LAN on: getchallenge answered" || bad "LAN on: getchallenge unanswered"
python3 - > "$WORK/lock.txt" << 'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(2)
s.sendto(b"\xff\xff\xff\xffgetinfo lock", ("10.77.0.2", 26000))
try: print(s.recvfrom(4096)[0].decode("latin1"))
except Exception as e: print("no reply", e)
PY
grep -aq 'qcstatus\\[^\\]*:L1' "$WORK/lock.txt" && ok "LAN on: the password shows as a lock in the status" || bad "LAN on: no lock key"

join() { # name password-or-empty seconds extra cfg lines
	local name=$1 pw=$2 secs=$3
	mkdir -p "$WORK/$name/data"
	{
		echo "set menu_touch_uib 1"
		if [ -n "$pw" ]; then
			echo "set _cl_touch_lan_password \"$pw\""
			echo "set _cl_touch_lan_password_server \"10.77.0.2:26000\""
		fi
		echo "defer 5 \"connect 10.77.0.2:26000\""
		# the menu's description every 3 s (the toast lasts 8 s)
		echo "alias dumploop \"uib_dump; defer 3 dumploop\""
		echo "defer 8 dumploop"
		echo "defer $((secs - 6)) \"uib_dump end.json\""
		echo "defer $((secs - 5)) \"exec state.cfg\""
		echo "defer $((secs - 3)) \"quit\""
	} > "$WORK/$name/data/join.cfg"
	echo 'echo ENDSTATE reconnect=[$net_reconnect_state]' > "$WORK/$name/data/state.cfg"
	run_engine "$name" peer $((secs + 20)) +exec join.cfg > "$WORK/$name.log" 2>&1
}

join nopw "" 22
grep -aq "\[pw-required\]" "$WORK/nopw.log" && ok "LAN on: no password is refused" || bad "LAN on: no password: $(grep -a 'rejected\|established' "$WORK/nopw.log" | tail -2)"

# ---------------------------------------------------------------- 3. the host leaves the match
ended_checks() { # name what
	local name=$1 what=$2
	grep -aq "Connection established to 10.77.0.2:26000\|Connection accepted to 10.77.0.2:26000" "$WORK/$name.log" \
		&& ok "$what: the client joined" || bad "$what: the client never joined: $(grep -a 'rejected\|Connect' "$WORK/$name.log" | tail -2)"
	grep -aq "The host ended the game" "$WORK/$name.log" \
		&& ok "$what: the client is told 'The host ended the game'" || bad "$what: no 'host ended' on the client: $(grep -a 'isconnect' "$WORK/$name.log" | tail -2)"
	grep -aq "joining 10.77.0.2:26000 again" "$WORK/$name.log" \
		&& bad "$what: the client tried to join again" || ok "$what: the client does not try to join again"
	local dump="$WORK/$name/data/data/end.json"
	[ -f "$dump" ] || dump="$WORK/$name/data/end.json"
	if grep -q '"context":"menu"' "$dump" 2> /dev/null && grep -q '"stack":\["home"\]' "$dump"; then
		ok "$what: the client's menu is back at Home"
	else
		bad "$what: the client's menu after the end: $(grep -o '"stack":[^]]*]\|"context":"[a-z]*"' "$dump" 2> /dev/null | tr '\n' ' ')"
	fi
	grep -a '"type":"toast"' "$WORK/$name.log" | grep -aq 'The host ended the game' \
		&& ok "$what: the menu shows 'The host ended the game'" || bad "$what: the menu does not say why"
}
port_free() {
	inhost python3 -c 'import socket,sys
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
try: s.bind(("0.0.0.0",26000))
except OSError as e: sys.exit(1)'
}

join leave "$PASSWORD" 60 &
JOINJOB=$!
PIDS+=("$JOINJOB")
waitlog "$WORK/leave.log" "Connection established to 10.77.0.2:26000\|Connection accepted to 10.77.0.2:26000" 60
sleep 15
hostcmd "disconnect"
wait "$JOINJOB"
ended_checks leave "host leaves"
sleep 1
port_free && ok "host leaves: UDP 26000 is free again" || bad "host leaves: UDP 26000 is still bound"
kill -0 "$HOSTJOB" 2> /dev/null && ok "host leaves: the host's game is still running (at its menu)" || bad "host leaves: the host process died"

# ---------------------------------------------------------------- 4. the host quits
hostcmd "set sv_touch_lan_password \"\"; set sv_public 0; touch_lan_arm; maxplayers 4; port 26000; map $MAP"
waitlog "$WORK/host.log" "HOSTSTEP-$STEP" 5
sleep 12
join quit "" 60 &
JOINJOB=$!
PIDS+=("$JOINJOB")
waitlog "$WORK/quit.log" "Connection established to 10.77.0.2:26000\|Connection accepted to 10.77.0.2:26000" 60
sleep 15
printf 'quit\n' > "$WORK/host/data/hostpoll.cfg"
wait "$JOINJOB"
ended_checks quit "host quits"
for _ in $(seq 1 50); do kill -0 "$HOSTJOB" 2> /dev/null || break; sleep 0.2; done
if kill -0 "$HOSTJOB" 2> /dev/null; then bad "host quits: the host process is still there"; else ok "host quits: no host process left"; fi
port_free && ok "host quits: UDP 26000 is free" || bad "host quits: UDP 26000 is still bound"
# ---------------------------------------------------------------- 5. the host's popup closes when someone joins
# A host that goes through the menu (Multiplayer > Create, LAN ticked, Host): the
# popup with the addresses opens by itself, a second game joins, the popup
# leaves with "A player joined your game", and the game is running for both.
mkdir -p "$WORK/host2/data"
ev() { printf 'uib_event %s' "$1" | sed 's/"/\\"/g'; }
cat > "$WORK/host2/data/host2.cfg" <<HOST2
set menu_touch_uib 1
set touch_lan_bots 1
set touch_lan_host_port 26000
set touch_lan_popup_autoclose 1
defer 6 "$(ev '{"ev":"action","node":"home/tiles/mp","action":"mp"}')"
defer 7 "$(ev '{"ev":"action","node":"mp/join/create","action":"create"}')"
defer 8 "$(ev '{"ev":"value","node":"sp/match/form/lan","value":true}')"
defer 9 "$(ev '{"ev":"value","node":"sp/match/form/pwon","value":false}')"
defer 10 "$(ev '{"ev":"action","node":"sp","action":"play"}')"
alias dumploop "uib_dump; prvm_global server time; defer 2 dumploop"
defer 12 dumploop
HOST2
run_engine host2 host 200 +exec host2.cfg > "$WORK/host2.log" 2>&1 &
HOST2JOB=$!
PIDS+=("$HOST2JOB")
waitlog "$WORK/host2.log" "Server listening on address 0.0.0.0:26000" 90 \
	&& ok "popup: Create with LAN ticked hosts on 26000" || { bad "popup: the menu's Host did not listen"; tail -5 "$WORK/host2.log"; }
waitlog "$WORK/host2.log" '"modal":\["lan.host"\]' 40 \
	&& ok "popup: it opens by itself when the hosted game is up" || bad "popup: it did not open: $(grep -ao '"modal":[^]]*]' "$WORK/host2.log" | tail -1)"
grep -a '"lan.host/addresses/a0/text"' "$WORK/host2.log" | grep -aq '10.77.0.2:26000' \
	&& ok "popup: it shows 10.77.0.2:26000" || bad "popup: no address row"
LINES_BEFORE=$(wc -l < "$WORK/host2.log")
join popupjoin "" 40 &
JOINJOB=$!
PIDS+=("$JOINJOB")
waitlog "$WORK/popupjoin.log" "Connection established to 10.77.0.2:26000\|Connection accepted to 10.77.0.2:26000" 60 \
	&& ok "popup: a second game joined" || bad "popup: nobody joined"
sleep 12
tail -n +"$LINES_BEFORE" "$WORK/host2.log" > "$WORK/host2-after.txt"
grep -a '"type":"toast"' "$WORK/host2-after.txt" | grep -aq "A player joined your game" \
	&& ok "popup: the host is told 'A player joined your game'" || bad "popup: no toast about the join"
grep -ao '"modal":[^]]*]' "$WORK/host2-after.txt" | tail -1 | grep -aq '"modal":\[\]' \
	&& ok "popup: it closed by itself" || bad "popup: still up after the join: $(grep -ao '"modal":[^]]*]' "$WORK/host2-after.txt" | tail -1)"
# the game runs for the joiner: the server's clock moves between two readings
T=($(grep -a "time" "$WORK/host2-after.txt" | grep -av '"' | grep -ao '[0-9][0-9]*\.[0-9]*' | tail -2))
if [ "${#T[@]}" = 2 ] && awk "BEGIN{exit !(${T[1]} > ${T[0]} + 0.5)}"; then
	ok "popup: the game is not left paused (server time ${T[0]} -> ${T[1]})"
else
	bad "popup: the server clock did not move after the join: ${T[*]:-no readings}"
fi
wait "$JOINJOB"
kill "$HOST2JOB" 2> /dev/null
inhost pkill -f "$WORK/host2" 2> /dev/null

! grep -aq "$PASSWORD" "$WORK"/*.log && ok "the password is in no log" || bad "the password is in a log"

[ "$fail" = 0 ] && echo "PASS" || echo "FAILED"
exit "$fail"
