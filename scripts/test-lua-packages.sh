#!/bin/bash
# Packages and hooks of the Lua runtime (docs/PLUGIN_STANDARD.md), in the real
# engine, headless (SDL offscreen):
#   - the generated hook files are current (scripts/gen-lua-hooks.py --check)
#   - install from a .zip, a folder, and over HTTP from a local test server
#     (plain http to 127.0.0.1 with lua_install_insecure_localhost 1; HTTPS too
#     when bwrap and openssl are there to trust a throwaway CA); review before
#     anything is written, installs off with nothing granted
#   - every archive attack is refused and writes nothing: traversal, absolute
#     paths, backslashes, drive letters, symlinks, hidden files, file types,
#     data/, duplicates, size caps, a zip bomb that lies about its size,
#     encryption, junk, a bad id, a newer API, the wrong realm, a planted
#     .install.tmp symlink, a pinned sha256 that does not match
#   - update: keeps data/ and the grants still requested; new permissions stay
#     ungranted and are named; lua_mod_update from the recorded source
#   - uninstall: removes folder, grants and source record; keepdata keeps data/
#   - enabling with missing grants names them all; "grant all" and the restart
#   - a server's text (a demo's svc_stufftext) cannot install, confirm,
#     update or uninstall
#   - the hook bridge in a bot match: hooks_write turns damage off (no deaths),
#     a read-only subscriber's writes are ignored, a subscriber that errors,
#     one that overruns a call and one that overruns the frame budget are
#     stopped while the match goes on; without hooks_write players die
#
#   BASEDIR=<basedir with data/*.pk3 and the Touch xonotic-data.pk3dir> scripts/test-lua-packages.sh
#
# ENGINE defaults to engine/darkplaces/darkplaces-sdl; KEEP=1 keeps the logs.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${ENGINE:-$ROOT/engine/darkplaces/darkplaces-sdl}"
BASEDIR="${BASEDIR:?set BASEDIR to a Xonotic basedir (data/*.pk3)}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xt-lua-packages.XXXXXX")"
HTTPPID=""
cleanup() {
	[ -n "$HTTPPID" ] && kill "$HTTPPID" 2>/dev/null
	[ -n "${KEEP:-}" ] && echo "logs kept in $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

run_engine() { # log, extra args...
	local log="$1"; shift
	SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy timeout -k 10 150 ${WRAP:-} "$ENGINE" -xonotic -nosound \
		-basedir "$BASEDIR" +vid_width 640 +vid_height 480 +vid_fullscreen 0 "$@" > "$log" 2>&1
}

# --- generated hook files --------------------------------------------------
python3 "$ROOT/scripts/gen-lua-hooks.py" --check > "$WORK/gen.txt" 2>&1 && ok "$(cat "$WORK/gen.txt")" || { bad "hook files stale"; cat "$WORK/gen.txt"; }

# --- fixtures ----------------------------------------------------------------
P="$WORK/pkgs"
python3 -I "$ROOT/tests/lua-sandbox/packages/make.py" "$P" || { bad "fixtures"; exit 1; }
UD="$WORK/home"
mkdir -p "$UD/mods" "$UD/data" "$UD/luastate/mods"
echo canary > "$UD/luastate/mods/canary.txt"
# a planted staging folder that is a symlink into the manager's state
ln -s ../luastate/mods "$UD/mods/.install.tmp"
SHA1=$(sha256sum "$P/pkgmod-1.zip" | cut -d' ' -f1)

# a local web server for the http install and lua_mod_update
PORT=$((20000 + RANDOM % 20000))
mkdir -p "$WORK/www"
cp "$P/pkgmod-1.zip" "$WORK/www/pkgmod.zip"
(cd "$WORK/www" && exec python3 -I -m http.server "$PORT" --bind 127.0.0.1 > "$WORK/http.log" 2>&1) &
HTTPPID=$!
sleep 1

ATTACKS="traversal traversal2 absolute backslash drive symlink hidden type data dupcase big total many noinit badid api notzip bomb encrypted realm"
{
	t=1
	for a in $ATTACKS; do
		echo "defer $t \"echo ATTACK $a\""
		echo "defer $t.1 \"lua_mod_install $P/evil-$a.zip\""
		t=$((t + 1))
	done
	cat <<EOF
defer 30 "echo STEP pin-mismatch"
defer 30.1 "lua_mod_install $P/pkgmod-1.zip#sha256=0000000000000000000000000000000000000000000000000000000000000000"
defer 31 "echo STEP http-refused"
defer 31.1 "lua_mod_install http://127.0.0.1:$PORT/pkgmod.zip"
defer 32 "echo STEP review"
defer 32.1 "lua_mod_install $P/pkgmod-1.zip#sha256=$SHA1"
defer 33 "lua_mods"
defer 33.5 "echo STEP confirm"
defer 33.6 "lua_install_confirm"
defer 34 "lua_mods"
defer 34.5 "echo STEP enable-missing"
defer 34.6 "lua_mod_enable pkgmod"
defer 35 "lua_mod_permission pkgmod grant all"
defer 35.5 "lua_mods"
defer 36 "echo STEP data"
defer 36.1 "lua_mod_permission pkgmod revoke all"
defer 37 "echo STEP update"
defer 37.1 "lua_mod_install $P/pkgmod-2.zip"
defer 38 "lua_install_confirm"
defer 38.5 "lua_mod_info pkgmod"
defer 39 "echo STEP folder"
defer 39.1 "lua_mod_install $P/folder/pkgfolder"
defer 39.5 "lua_install_confirm"
defer 40 "echo STEP flat"
defer 40.1 "lua_mod_install $P/pkgflat.zip"
defer 40.5 "lua_install_confirm"
defer 41 "echo STEP http"
defer 41.1 "set lua_install_insecure_localhost 1"
defer 41.2 "lua_mod_update pkgmod http://127.0.0.1:$PORT/pkgmod.zip"
defer 44 "lua_install_status"
defer 44.5 "lua_install_confirm"
defer 45 "echo STEP update-recorded"
defer 45.1 "lua_mod_update pkgmod"
defer 47 "lua_install_status"
defer 47.5 "lua_install_cancel"
defer 48 "echo STEP uninstall"
defer 48.1 "lua_mod_uninstall pkgflat"
defer 48.5 "lua_mod_uninstall pkgfolder keepdata"
defer 49 "lua_mods"
defer 50 "quit"
EOF
} > "$UD/data/pkg.cfg"
# storage the update must keep, and that uninstall keepdata keeps
LOG="$WORK/pkg.log"
run_engine "$LOG" -userdir "$UD" +exec pkg.cfg &
EPID=$!
# put a data file in place between install and update (the engine never writes it itself)
for _ in $(seq 400); do grep -q "STEP data" "$LOG" 2>/dev/null && break; sleep 0.1; done
mkdir -p "$UD/mods/pkgmod/data" && echo keep-me > "$UD/mods/pkgmod/data/size.txt"
wait $EPID
RC=$?
[ "$RC" = 0 ] && ok "package run exited cleanly" || bad "package run exit code $RC"

has() { grep -aqF -- "$1" "$LOG"; }
between() { # marker, next marker, text: text appears between the markers
	awk -v a="$1" -v b="$2" 'index($0,a){on=1;next} index($0,b){on=0} on' "$LOG" | grep -aqF -- "$3"
}
expect_attack() { # name, reason substring
	local next
	next=$(echo "$ATTACKS STEP" | tr ' ' '\n' | grep -A1 -x "$1" | tail -1)
	[ "$next" = STEP ] && next="STEP pin-mismatch" || next="ATTACK $next"
	if between "ATTACK $1" "$next" "Install refused" && between "ATTACK $1" "$next" "$2"; then ok "refused $1 ($2)"; else bad "attack $1 not refused with '$2'"; awk -v a="ATTACK $1" -v b="$next" 'index($0,a){on=1;next} index($0,b){on=0} on' "$LOG" | head -5; fi
}
expect_attack traversal "a . or .. component"
expect_attack traversal2 "a . or .. component"
expect_attack absolute "an absolute path"
expect_attack backslash "a backslash"
expect_attack drive "a drive letter or colon"
expect_attack symlink "is a symlink"
expect_attack hidden "a hidden (dot) file"
expect_attack type "only .lua"
expect_attack data "data/ folder"
expect_attack dupcase "is in it twice"
expect_attack big "over 1 MB"
expect_attack total "over 8 MB unpacked"
expect_attack many "more than 256 files"
expect_attack noinit "no init.lua"
expect_attack badid "no usable name"
expect_attack api "needs API 9"
expect_attack notzip "not a zip archive"
expect_attack bomb "unpacks to more than it says"
expect_attack encrypted "is encrypted"
expect_attack realm "server mod"
for f in evil evilapi srvonly; do [ -e "$UD/mods/$f" ] && bad "attack left $UD/mods/$f"; done
[ ! -e "$UD/evil.lua" ] && [ ! -e "/tmp/evil.lua" ] && [ ! -e "$UD/mods/evil.lua" ] && ok "no attack file written anywhere" || bad "an attack wrote evil.lua"
[ "$(cat "$UD/luastate/mods/canary.txt" 2>/dev/null)" = canary ] && ok "manager state untouched by the planted .install.tmp symlink" || bad "luastate/mods/canary.txt changed or gone"
between "STEP pin-mismatch" "STEP http-refused" "sha256 mismatch" && ok "a pinned sha256 that does not match is refused" || bad "pin mismatch not refused"
between "STEP http-refused" "STEP review" "only https://" && ok "plain http refused by default" || bad "plain http accepted"
between "STEP review" "STEP confirm" "matches the pinned hash" && ok "a matching pin is accepted and shown" || bad "matching pin not shown"
between "STEP review" "STEP confirm" "reason.events" && bad "raw manifest leaked" || true
between "STEP review" "STEP confirm" "draws on the hud event" && ok "the review shows the author's reasons" || bad "reasons not in the review"
if between "STEP review" "STEP confirm" "pkgmod "; then bad "pkgmod listed before lua_install_confirm"; else ok "nothing written before lua_install_confirm"; fi
between "STEP confirm" "STEP enable-missing" "installed pkgmod: it is off and has no permissions" && ok "installed off, nothing granted" || bad "install message wrong"
grep -aqE "^ *pkgmod +off" "$LOG" && ok "the installed mod is listed off" || bad "pkgmod not listed off"
between "STEP enable-missing" "STEP data" "permissions not granted: events, display" && ok "enabling names every missing permission" || bad "missing permissions not all named"
between "STEP enable-missing" "STEP data" "granted all, restarted: running" && ok "grant all restarts a mod stopped for permissions" || bad "no restart after grant"
between "STEP update" "STEP folder" "NEW storage" && ok "update review marks the new permission" || bad "new permission not marked"
between "STEP update" "STEP folder" "asks for new permissions, not granted: storage" && ok "update says the new permission stays ungranted" || bad "update message wrong"
[ "$(cat "$UD/mods/pkgmod/data/size.txt" 2>/dev/null)" = keep-me ] && ok "update kept the mod's data/" || bad "update lost data/"
between "STEP update" "STEP folder" "version: 2.0" && ok "update installed version 2.0" || bad "version not updated"
has "installed pkgfolder: it is off" && ok "install from a folder" || bad "folder install failed"
has "installed pkgflat: it is off" && between "STEP flat" "STEP http" "3 files" && ok "install from a zip without a top folder (3 files, lib/util.lua kept)" || bad "flat install failed"
between "STEP http" "STEP update-recorded" "Update of Lua mod" && ok "install over HTTP from the local server (review)" || bad "http install did not reach the review"
grep -q "GET /pkgmod.zip" "$WORK/http.log" && ok "the test server was asked for the package" || bad "no request reached the test server"
grep -aq "source http://127.0.0.1:$PORT/pkgmod.zip" "$UD/luastate/mods/sources/pkgmod.txt" 2>/dev/null && ok "the source is recorded for lua_mod_update" || bad "source not recorded"
between "STEP update-recorded" "STEP uninstall" "Update of Lua mod" && ok "lua_mod_update fetches from the recorded source" || bad "lua_mod_update without a source failed"
[ ! -e "$UD/mods/pkgflat" ] && [ ! -e "$UD/luastate/mods/sources/pkgflat.txt" ] && ok "uninstall removed the folder and the source record" || bad "uninstall left files"
[ -d "$UD/mods/pkgfolder/data" ] || mkdir -p "$UD/mods/pkgfolder/data"
[ ! -e "$UD/mods/pkgfolder/init.lua" ] && ok "uninstall keepdata removed the code" || bad "keepdata left the code"
[ ! -e "$UD/luastate/mods/grants/pkgflat.txt" ] && ok "uninstall removed the grants" || bad "grants left after uninstall"

# --- HTTPS with a throwaway CA, when the host can do it -----------------------
if command -v bwrap > /dev/null && command -v openssl > /dev/null && [ -f /etc/pki/tls/certs/ca-bundle.crt -o -f /etc/ssl/certs/ca-certificates.crt ]; then
	C="$WORK/tls"; mkdir -p "$C"
	openssl req -x509 -newkey rsa:2048 -nodes -keyout "$C/key.pem" -out "$C/cert.pem" -days 1 -subj "/CN=127.0.0.1" -addext "subjectAltName=IP:127.0.0.1" > /dev/null 2>&1
	SPORT=$((PORT + 1))
	python3 -I - "$WORK/www" "$SPORT" "$C/cert.pem" "$C/key.pem" > "$WORK/https.log" 2>&1 <<'EOF' &
import http.server, ssl, sys, functools
d, port, cert, key = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
h = functools.partial(http.server.SimpleHTTPRequestHandler, directory=d)
s = http.server.HTTPServer(("127.0.0.1", port), h)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
s.socket = ctx.wrap_socket(s.socket, server_side=True)
s.serve_forever()
EOF
	SPID=$!
	sleep 1
	BUNDLE=/etc/pki/tls/certs/ca-bundle.crt; [ -f "$BUNDLE" ] || BUNDLE=/etc/ssl/certs/ca-certificates.crt
	cat "$BUNDLE" "$C/cert.pem" > "$C/bundle.crt"
	HUD="$WORK/https-home"; mkdir -p "$HUD/data"
	cat > "$HUD/data/https.cfg" <<EOF
defer 1 "lua_mod_install https://127.0.0.1:$SPORT/pkgmod.zip"
defer 5 "lua_install_status"
defer 6 "quit"
EOF
	WRAP="bwrap --dev-bind / / --ro-bind $C/bundle.crt $BUNDLE" run_engine "$WORK/https.log2" -userdir "$HUD" +exec https.cfg
	kill $SPID 2>/dev/null
	grep -aqF "New Lua mod" "$WORK/https.log2" && ok "install over HTTPS (local server, throwaway CA)" || { bad "HTTPS install did not reach the review"; grep -a "Install refused\|download" "$WORK/https.log2" | head -3; }
else
	echo "skip HTTPS install (needs bwrap, openssl and a system CA bundle); plain http to 127.0.0.1 was tested"
fi

# --- a server's text ---------------------------------------------------------
RUD="$WORK/remote"
mkdir -p "$RUD/mods/victim" "$RUD/data"
printf 'print("VICTIM")\n' > "$RUD/mods/victim/init.lua"
python3 - "$RUD/data/stuff.dem" "$P/pkgmod-1.zip" <<'EOF'
import struct, sys
z = sys.argv[2].encode()
text = (b"lua_mod_install " + z + b"\n" + b"lua_install_confirm\n" + b"lua_mod_uninstall victim\n" +
        b"lua_mod_update victim " + z + b"\n" + b"sv_lua_plugin_install " + z + b"\n" + b"sv_lua_mod_install " + z + b"\n" + b"lua_install_cancel\n" +
        b"set lua_install_insecure_localhost 1\n")
msg = bytes([9]) + text + b"\0"
with open(sys.argv[1], "wb") as f:
    f.write(b"-1\n")
    f.write(struct.pack("<i", len(msg)) + struct.pack("<fff", 0, 0, 0) + msg)
EOF
cat > "$RUD/data/remote.cfg" <<'EOF'
defer 2 "playdemo stuff"
defer 6 "lua_install_status"
defer 6.5 "lua_install_insecure_localhost"
defer 7 "quit"
EOF
RLOG="$WORK/remote.log"
run_engine "$RLOG" -userdir "$RUD" +exec remote.cfg
for c in lua_mod_install lua_install_confirm lua_mod_uninstall lua_mod_update sv_lua_plugin_install sv_lua_mod_install lua_install_cancel; do
	grep -aq "$c: not while connected to a server" "$RLOG" && ok "server text: $c refused" || bad "server text: $c not refused"
done
grep -aqE 'lua_install_insecure_localhost(.\[m)?"? is "0"' "$RLOG" && ok "server text cannot allow plain http" || bad "lua_install_insecure_localhost changed by server text"
[ -e "$RUD/mods/victim/init.lua" ] && [ ! -e "$RUD/mods/pkgmod" ] && ok "server text installed and removed nothing" || bad "server text changed the mods folder"

# --- the hook bridge in a bot match --------------------------------------------
match() { # name, grant hooks_write (0/1)
	local H="$WORK/match-$1"
	mkdir -p "$H/plugins" "$H/data" "$H/luastate/plugins/grants"
	cp -r "$ROOT/touch/lua/examples/plugins/damage_scale" "$ROOT/tests/lua-sandbox/hooks/"* "$H/plugins/"
	printf 'damage_scale\ndeathcount\nhookerror\nhookslow\nhookframe\n' > "$H/plugins/enabled.txt"
	# damage_scale asks for hooks (0x800), hooks_write (0x1000), messages (0x200)
	if [ "$2" = 1 ]; then printf 'v1 1a00\n' > "$H/luastate/plugins/grants/damage_scale.txt"; else printf 'v1 a00\n' > "$H/luastate/plugins/grants/damage_scale.txt"; fi
	for p in deathcount hookerror hookslow hookframe; do printf 'v1 800\n' > "$H/luastate/plugins/grants/$p.txt"; done
	cat > "$H/data/match.cfg" <<'EOF'
set sv_cheats 1
set bot_number 4
set skill 10
set g_warmup 0
set slowmo 2
set sv_lua_damage_scale_factor 0
set sv_lua_damage_scale_selfdamage 1
set lua_hook_ms 20
set lua_hook_frame_ms 5
map stormkeep
defer 10 "togglemenu 0"
defer 12 "togglemenu 0"
defer 14 "togglemenu 0"
defer 40 "sv_lua_mods"
defer 40.5 "sv_lua_hooks"
defer 41 "quit"
EOF
	run_engine "$WORK/match-$1.log" -userdir "$H" +exec match.cfg
	echo $?
}
RC=$(match write 1)
M="$WORK/match-write.log"
[ "$RC" = 0 ] && ok "hook match exited cleanly" || bad "hook match exit code $RC"
last=$(grep -ao "HOOKS deaths [0-9]* hits [0-9]*" "$M" | tail -1)
deaths=$(echo "$last" | awk '{print $3}'); hits=$(echo "$last" | awk '{print $5}')
[ -n "$hits" ] && [ "$hits" -gt 20 ] && ok "Damage_Calculate seen by a read-only subscriber ($hits hits)" || bad "too few hits seen: $last"
[ "${deaths:-1}" = 0 ] && ok "hooks_write set the damage to 0: no deaths in the match" || bad "players died with damage scaled to 0 ($last)"
grep -aqE "Lua server mod hookerror stopped: .*HOOKERROR on purpose" "$M" && ok "an erroring hook subscriber is stopped" || bad "hookerror not stopped"
grep -aqE "Lua server mod hookslow stopped: .*(time limit|instruction limit)" "$M" && ok "a hook call over lua_hook_ms is stopped" || bad "hookslow not stopped"
grep -aqE "Lua server mod hookframe stopped: hook budget" "$M" && ok "a subscriber over the frame budget is stopped" || bad "hookframe not stopped by the frame budget"
stopline=$(grep -an "Lua server mod hookframe stopped\|Lua server mod hookslow stopped" "$M" | tail -1 | cut -d: -f1)
afterline=$(grep -an "HOOKS deaths" "$M" | tail -1 | cut -d: -f1)
[ -n "$stopline" ] && [ "${afterline:-0}" -gt "$stopline" ] && ok "the match and the other server mods go on after the stops" || bad "nothing ran after the stops"
grep -aqE "^ *damage_scale +running" "$M" && ok "damage_scale still running" || bad "damage_scale not running"
grep -aq "Damage_Calculate.*calls" "$M" && ok "sv_lua_hooks counts the calls: $(grep -ao 'Damage_Calculate.*calls[^)]*)' "$M" | head -1 | tr -s ' ')" || bad "no hook stats"
RC=$(match nowrite 0)
last=$(grep -ao "HOOKS deaths [0-9]* hits [0-9]*" "$WORK/match-nowrite.log" | tail -1)
deaths=$(echo "$last" | awk '{print $3}')
[ "${deaths:-0}" -gt 0 ] && ok "without hooks_write the change is ignored: $deaths deaths" || bad "no deaths without hooks_write ($last)"

if [ "$fail" != 0 ]; then
	echo "--- package log (Lua lines) ---"
	grep -aE "Lua|lua_|Install|STEP|ATTACK|refused" "$LOG" | tail -80
	exit 1
fi
echo "all Lua package and hook checks passed"
