#!/usr/bin/env bash
# Fast iteration loop for touch UI work: compile QuakeC locally, push the .dat
# files (and touch gfx/config) straight into the device's data dir, restart the
# game, and pull a screenshot back.
#
# The engine binary is NOT rebuilt here — only QuakeC and data. Use
# scripts/install-flatpak.sh when engine C code changes.
#
# Usage:
#   scripts/dev-deploy.sh [--no-build] [--no-restart] [--shot NAME] [--host ID]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QCSRC="$ROOT/engine/data/xonotic-data.pk3dir/qcsrc"
PK3DIR="$ROOT/engine/data/xonotic-data.pk3dir"
QCC="$ROOT/engine/gmqcc/gmqcc"

GDR_DEVICE="${GDR_DEVICE:-marinesurface}"
DO_BUILD=1
DO_RESTART=1
SHOT_NAME=""

while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) DO_BUILD=0 ;;
        --no-restart) DO_RESTART=0 ;;
        --shot) SHOT_NAME="${2:?--shot needs a name}"; shift ;;
        --host) GDR_DEVICE="${2:?--host needs a device id}"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# Device coordinates come from the gdr profile so credentials live in one place.
gdr_field() {
    python3 - "$GDR_DEVICE" "$1" <<'PY'
import json, os, sys
cfg = json.load(open(os.path.expanduser("~/.config/gdr/config.json")))
host = cfg["hosts"][sys.argv[1]]
print(host.get(sys.argv[2]) or "")
PY
}

SSH_TARGET="$(gdr_field ssh)"
SSH_PASS="$(gdr_field sudo_password)"
[ -n "$SSH_TARGET" ] || { echo "no ssh target for device '$GDR_DEVICE'" >&2; exit 1; }

run_ssh() {
    SSHPASS="$SSH_PASS" sshpass -e ssh -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 "$SSH_TARGET" "$@"
}

# scp needs absolute remote paths; resolve $HOME once instead of quoting it
# through every call site.
REMOTE_HOME="$(run_ssh 'echo $HOME')"
REMOTE_DATA="$REMOTE_HOME/.local/share/xonotic-touch/data"
REMOTE_OVERLAY="$REMOTE_DATA/zzzz-touch-dev.pk3dir"
# Engine userdir: where config.cfg, screenshots, and the winning autoexec live.
REMOTE_USERDIR="$REMOTE_HOME/.xonotic/data"

copy_to() {
    local src="$1" dest="$2"
    SSHPASS="$SSH_PASS" sshpass -e scp -q -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 -r "$src" "$SSH_TARGET:$dest"
}

log() { printf '\033[36m==>\033[0m %s\n' "$*"; }

if [ "$DO_BUILD" = 1 ]; then
    log "compiling QuakeC"
    make -C "$QCSRC" QCC="$QCC" ../csprogs.dat ../menu.dat >/tmp/xt-qc-build.log 2>&1 || {
        echo "QuakeC build FAILED — tail of /tmp/xt-qc-build.log:" >&2
        tail -40 /tmp/xt-qc-build.log >&2
        exit 1
    }
    grep -E '^\s+(size|crc):' /tmp/xt-qc-build.log | tail -4 || true
fi

log "staging overlay on $GDR_DEVICE"
# Stale zzz*-touch-fix trees from older manual test runs shadow the new build
# (docs/test-runs/.../FINDINGS-pause.md), so clear them every deploy.
run_ssh "rm -rf $REMOTE_DATA/zzz-touch-fix.pk3dir $REMOTE_DATA/zzzz-touch-fix.pk3dir \
         && mkdir -p $REMOTE_OVERLAY/gfx"

TMPSTAGE="$(mktemp -d)"
trap 'rm -rf "$TMPSTAGE"' EXIT
cp "$PK3DIR/csprogs.dat" "$PK3DIR/menu.dat" "$TMPSTAGE/"
[ -d "$ROOT/touch/gfx" ] && cp -a "$ROOT/touch/gfx/." "$TMPSTAGE/gfx/" 2>/dev/null || true

log "uploading progs + gfx"
copy_to "$TMPSTAGE/." "$REMOTE_OVERLAY/"

log "uploading touch configs"
run_ssh "mkdir -p '$REMOTE_DATA/touch/profiles'"
copy_to "$ROOT/touch/profiles/." "$REMOTE_DATA/touch/profiles/"
copy_to "$ROOT/touch/xonotic.cfg" "$REMOTE_DATA/xonotic.cfg"

# Test binds let scripts/dev-test.sh drive the game with single keypresses
# instead of navigating the desktop-oriented menus by touch.
cat > "$TMPSTAGE/xt-devbinds.cfg" <<'BINDS'
// Written by scripts/dev-deploy.sh — development harness only.
bind F12 "screenshot xt-shot.jpg"
bind F9  "exec xt-match.cfg"
bind F10 "toggle touch_debug 0 1"
bind F7  "cmd join"
bind F8  "exec xt-probe.cfg"
BINDS
cat > "$TMPSTAGE/xt-match.cfg" <<'MATCH'
// One-key jump into a live bot deathmatch for touch testing.
// cl_welcome 0 skips the join/spectate dialog, which cannot be dismissed by
// touch: the engine only synthesizes clicks while keydest is a menu.
cl_welcome 0
g_dm 1
minplayers 2
bot_number 2
timelimit 30
map ${XT_TEST_MAP}
MATCH
sed -i "s|\${XT_TEST_MAP}|${XT_TEST_MAP:-solarium}|" "$TMPSTAGE/xt-match.cfg"
# These go in the engine *userdir*, not the gamedir: the userdir shadows the
# gamedir for `exec`, so an autoexec.cfg written to the gamedir is never seen.
copy_to "$TMPSTAGE/xt-devbinds.cfg" "$REMOTE_USERDIR/xt-devbinds.cfg"
copy_to "$TMPSTAGE/xt-match.cfg" "$REMOTE_USERDIR/xt-match.cfg"
# autoexec.cfg is on the launcher's +exec chain, so chain the binds from there.
run_ssh "touch '$REMOTE_USERDIR/autoexec.cfg'; \
         grep -q xt-devbinds '$REMOTE_USERDIR/autoexec.cfg' \
         || echo 'exec xt-devbinds.cfg' >> '$REMOTE_USERDIR/autoexec.cfg'"

if [ "$DO_RESTART" = 1 ]; then
    log "restarting game"
    # A plain ssh login has no session bus or Wayland socket, so flatpak run
    # would exit immediately; re-attach to the logged-in GNOME session.
    # setsid detaches from the ssh session: without it the game is killed as
    # soon as this connection closes. `pgrep -x` avoids matching our own
    # command line, which contains the pattern.
    run_ssh "export XDG_RUNTIME_DIR=/run/user/\$(id -u); \
             export DBUS_SESSION_BUS_ADDRESS=unix:path=\$XDG_RUNTIME_DIR/bus; \
             export WAYLAND_DISPLAY=wayland-0; \
             pkill -x xonotic 2>/dev/null; sleep 1; \
             setsid nohup flatpak run io.github.dixonSolutions.XonoticTouch \
             >/tmp/xonotic-dev.log 2>&1 </dev/null & \
             sleep ${XT_BOOT_WAIT:-20}; pgrep -x xonotic >/dev/null \
             && echo 'game running' || echo 'GAME FAILED TO START'"
fi

if [ -n "$SHOT_NAME" ]; then
    OUT="$ROOT/docs/test-runs/current/$SHOT_NAME.png"
    mkdir -p "$(dirname "$OUT")"
    log "screenshot -> $OUT"
    gdr --dev="$GDR_DEVICE" screenshot -o "$OUT" >/dev/null
fi

log "done"
