#!/bin/bash
# LAN games end to end, headless, on one machine: hosting, discovery through
# the LAN search (never a typed address), the join password and the port-in-use
# handling. docs/LAN.md says what this does and does not prove about Wi-Fi.
#
# Two network namespaces joined by a virtual Ethernet cable (veth), made in an
# unprivileged user namespace (unshare -rn, no root needed):
#   "lan host" 10.77.0.2/24  -- the hosting game (a listen server, offscreen)
#                               and a second, dedicated server started on the
#                               same port, which must move to the next one
#   "lan peer" 10.77.0.1/24  -- the joining clients
# Neither has a default route, so the stock 255.255.255.255 query fails there
# (ENETUNREACH) exactly as on a phone on a Wi-Fi without internet; the games
# must be found through the subnet-directed broadcast (10.77.0.255).
#
#   BASEDIR=<basedir with data/*.pk3 and the Touch xonotic-data.pk3dir> scripts/test-lan.sh
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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-lan.XXXXXX")"
cleanup() {
	[ -n "${HOSTPID:-}" ] && kill "$HOSTPID" 2> /dev/null
	[ -n "${SECONDPID:-}" ] && kill "$SECONDPID" 2> /dev/null
	[ -n "${NSPID:-}" ] && kill "$NSPID" 2> /dev/null
	if [ -n "${OUT:-}" ]; then mkdir -p "$OUT" && cp "$WORK"/*.log "$OUT/" 2> /dev/null; fi
	rm -rf "$WORK"
}
trap cleanup EXIT

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

# --- the network: this namespace is the peer, a child one the host
ip link set lo up
unshare -n sleep 600 & NSPID=$!
sleep 0.3
ip link add lanpeer type veth peer name lanhost
ip link set lanhost netns "$NSPID"
ip addr add 10.77.0.1/24 dev lanpeer
ip link set lanpeer up
nsenter -t "$NSPID" -n sh -c 'ip link set lo up; ip addr add 10.77.0.2/24 dev lanhost; ip link set lanhost up'
sleep 1
inhost() { nsenter -t "$NSPID" -n "$@"; }

SIZE=(+vid_width 640 +vid_height 400)
run_engine() { # name ns(host|peer) seconds extra-args...
	local name=$1 where=$2 secs=$3
	shift 3
	local ud="$WORK/$name"
	mkdir -p "$ud/data"
	local cmd=(env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout "$secs" "$ENGINE" -xonotic -nosound
		-basedir "$BASEDIR" -userdir "$ud" +sys_colortranslation 2 "${SIZE[@]}" +vid_fullscreen 0 "$@")
	if [ "$where" = host ]; then inhost "${cmd[@]}"; else "${cmd[@]}"; fi
}

# --- the hosting game: a listen server, as the Host screen starts it
cat > "$WORK/hostcfg.cfg" <<EOF
set sv_touch_lan_password "$PASSWORD"
set sv_public 0
set sv_status_privacy 1
set hostname "LAN test game"
set bot_number 1
set g_warmup 0
maxplayers 4
port 26000
map $MAP
alias lanloop "touch_lan_status; exec hostpoll.cfg; defer 5 lanloop"
defer 15 lanloop
EOF
mkdir -p "$WORK/host/data"
cp "$WORK/hostcfg.cfg" "$WORK/host/data/hostcfg.cfg"
: > "$WORK/host/data/hostpoll.cfg"
[ -n "${SHOTS:-}" ] && SIZE=(+vid_width 2400 +vid_height 1080 +vid_touchscreen 1)
run_engine host host 300 +exec hostcfg.cfg > "$WORK/host.log" 2>&1 &
HOSTPID=$!
SIZE=(+vid_width 640 +vid_height 400)

# Wait for the listen server's socket.
for _ in $(seq 1 90); do
	grep -aq "Server listening on address 0.0.0.0:26000" "$WORK/host.log" && break
	sleep 1
done
grep -aq "Server listening on address 0.0.0.0:26000" "$WORK/host.log" \
	&& ok "host listens on 26000" || { bad "host did not start"; tail -30 "$WORK/host.log"; exit 1; }
sleep 22
grep -aq "Join at: 10.77.0.2:26000" "$WORK/host.log" \
	&& ok "host shows the address to join: $(grep -ao 'Join at: .*' "$WORK/host.log" | head -1)" \
	|| bad "touch_lan_status did not list 10.77.0.2:26000"

# --- port in use: the Host screen's check, then a second server on the same port
cat > "$WORK/portcheck.cfg" <<EOF
set _touch_lan_portcheck 26000
echo PORTCHECK free=\$_touch_lan_port_free suggest=\$_touch_lan_port_suggest
set _touch_lan_portcheck 26007
echo PORTCHECK2 free=\$_touch_lan_port_free suggest=\$_touch_lan_port_suggest
quit
EOF
mkdir -p "$WORK/portcheck/data"
cp "$WORK/portcheck.cfg" "$WORK/portcheck/data/"
run_engine portcheck host 60 -dedicated +exec portcheck.cfg > "$WORK/portcheck.log" 2>&1
grep -aq "PORTCHECK free=0 suggest=26001" "$WORK/portcheck.log" \
	&& ok "port 26000 in use is reported, 26001 offered" \
	|| bad "port check: $(grep -a PORTCHECK "$WORK/portcheck.log" | head -1)"
grep -aq "PORTCHECK2 free=1 suggest=26007" "$WORK/portcheck.log" \
	&& ok "a free port is reported free" || bad "free port check: $(grep -a PORTCHECK2 "$WORK/portcheck.log")"

# A second game asked for 26000 anyway: the engine moves it to the next port,
# and the LAN search (26000-26004) still finds it.
run_engine second host 200 -dedicated +set hostname "Second game" +maxplayers 2 +port 26000 +map "$MAP" > "$WORK/second.log" 2>&1 &
SECONDPID=$!
for _ in $(seq 1 60); do
	grep -aq "Server listening on address 0.0.0.0:2600" "$WORK/second.log" && break
	sleep 1
done
grep -aq "Server listening on address 0.0.0.0:26001" "$WORK/second.log" \
	&& ok "second server on the taken port moved to 26001" \
	|| bad "second server: $(grep -a 'listening\|failed to open' "$WORK/second.log" | head -3)"
sleep 8

# --- discovery: the LAN search only, no address typed, no master server
cat > "$WORK/find.cfg" <<EOF
set net_slist_lan_only 1
defer 8 "net_slist"
defer 14 "quit"
EOF
mkdir -p "$WORK/find/data"
cp "$WORK/find.cfg" "$WORK/find/data/"
run_engine find peer 60 +exec find.cfg > "$WORK/find.log" 2>&1
ADDR=$(grep -aoE '10\.77\.0\.2:26000' "$WORK/find.log" | head -1)
[ -n "$ADDR" ] && ok "LAN search found the hosted game at $ADDR" || bad "LAN search did not find the hosted game"
grep -aq "LAN test game" "$WORK/find.log" && ok "its name came with it" || bad "no server name in the list"
grep -aqE '10\.77\.0\.2:26001' "$WORK/find.log" && ok "the second game on port 26001 was found too" || bad "game on 26001 not found"
grep -aq "Querying DP master" "$WORK/find.log" && bad "LAN-only list asked a master server" || ok "no master server asked"
if grep -aq "Network is unreachable\|NetConn_Write" "$WORK/find.log"; then
	ok "stock 255.255.255.255 query failed here as expected (no default route); found through 10.77.0.255"
fi
ADDR=${ADDR:-10.77.0.2:26000}

# --- the lock the server list draws: the ":L1" key in the game's qcstatus,
# read from a getinfo reply (the list's own query gets the same string).
if command -v python3 > /dev/null; then
	python3 - "$WORK/status.txt" << 'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)
s.sendto(b"\xff\xff\xff\xffgetinfo lock", ("10.77.0.2", 26000))
try:
    open(sys.argv[1], "wb").write(s.recvfrom(4096)[0])
except Exception as e:
    open(sys.argv[1], "w").write("no reply: %s" % e)
PY
	grep -aq 'qcstatus\\[^\\]*:L1' "$WORK/status.txt" \
		&& ok "the locked game says so in its status: $(grep -ao 'qcstatus\\[^\\]*' "$WORK/status.txt")" \
		|| bad "no lock key in getinfo: $(tr -c '[:print:]' ' ' < "$WORK/status.txt" | head -c 300)"
fi

join() { # name password-or-empty
	local name=$1 pw=$2
	mkdir -p "$WORK/$name/data"
	{
		if [ -n "$pw" ]; then
			echo "set _cl_touch_lan_password \"$pw\""
			echo "set _cl_touch_lan_password_server \"$ADDR\""
		fi
		echo "defer 6 \"connect $ADDR\""
		echo "defer 30 \"quit\""
	} > "$WORK/$name/data/join.cfg"
	run_engine "$name" peer 60 +exec join.cfg > "$WORK/$name.log" 2>&1
}

join nopw ""
grep -aq "\[pw-required\] This game needs a password" "$WORK/nopw.log" \
	&& ok "no password: refused with 'needs a password'" || bad "no password: $(grep -a 'rejected\|Connect' "$WORK/nopw.log" | tail -2)"

join wrongpw "not it"
grep -aq "\[pw-wrong\] Wrong password" "$WORK/wrongpw.log" \
	&& ok "wrong password: refused with 'Wrong password'" || bad "wrong password: $(grep -a 'rejected' "$WORK/wrongpw.log" | tail -2)"
grep -aq "Refused 10.77.0.1:[0-9]*: wrong join password" "$WORK/host.log" \
	&& ok "host logged the refusal" || bad "host did not log the refusal"

if [ -n "${SHOTS:-}" ]; then
	# While the client is in: the host's own Host screen, addresses and players.
	( sleep 16
	  printf 'menu_cmd directmenu TouchLanHost\ndefer 3 "screenshot lan_hosting_2400x1080.png"\n' > "$WORK/host/data/hostpoll.cfg"
	  sleep 6
	  : > "$WORK/host/data/hostpoll.cfg" ) &
fi
join rightpw "$PASSWORD"
if grep -aq "rejected" "$WORK/rightpw.log"; then
	bad "right password: $(grep -a rejected "$WORK/rightpw.log" | tail -1)"
elif grep -aq "Connection established to $ADDR" "$WORK/rightpw.log"; then
	ok "right password: joined ($(grep -ao "Connection established to [0-9.:]*" "$WORK/rightpw.log" | head -1))"
else
	bad "right password: no sign of a connection"
fi

# The password never reaches a log.
if grep -aF "$PASSWORD" "$WORK"/*.log | grep -v "hostcfg\|join.cfg" > /dev/null; then
	bad "the password appears in a log"
else
	ok "the password is in no log"
fi

# --- optional: screenshots of the Host screen, the LAN list with the locked
# game and the password prompt, at a phone's and a tablet's size.
if [ -n "${SHOTS:-}" ]; then
	mkdir -p "$SHOTS"
	for size in 2400x1080 1920x1200; do
		w=${size%x*}
		h=${size#*x}
		name="shots$w"
		mkdir -p "$WORK/$name/data"
		{
			echo 'set vid_touchscreen 1'
			echo 'set net_slist_lan_only 1'
			echo 'set menu_slist_showempty 1'
			echo 'defer 8 "menu_cmd directmenu TouchLanHost"'
			echo "defer 11 \"screenshot lan_host_$size.png\""
			echo 'defer 13 "menu_cmd directmenu servers"'
			echo "defer 20 \"screenshot lan_list_$size.png\""
			echo "defer 22 \"menu_cmd directmenu TouchLanPassword wrong $ADDR\""
			echo "defer 26 \"screenshot lan_password_$size.png\""
			echo 'defer 30 "quit"'
		} > "$WORK/$name/data/shots.cfg"
		env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout 90 "$ENGINE" -xonotic -nosound \
			-basedir "$BASEDIR" -userdir "$WORK/$name" +sys_colortranslation 2 +vid_width "$w" +vid_height "$h" \
			+vid_fullscreen 0 +exec shots.cfg > "$WORK/$name.log" 2>&1
		find "$WORK/$name" -name 'lan_*' -exec cp {} "$SHOTS/" \;
	done
	find "$WORK/host" -name 'lan_*' -exec cp {} "$SHOTS/" \;
	ls "$SHOTS"
fi

sleep 1
grep -aqE "Players: .*\(you\), .*\([0-9]+ ms\)" "$WORK/host.log" \
	&& ok "host player list: $(grep -aoE 'Players: .*\([0-9]+ ms\).*' "$WORK/host.log" | tail -1)" || bad "no host player list"

[ "$fail" = 0 ] && echo "LAN test passed" || echo "LAN test FAILED"
exit $fail
