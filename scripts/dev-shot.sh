#!/usr/bin/env bash
# Drive the device into a live match and pull an engine-side screenshot.
#
# Two things make this necessary rather than a one-liner:
#   * The compositor capture (`gdr screenshot`) keeps returning the loading
#     plaque during a match, because the game renders directly and GNOME's
#     screencast copy is never refreshed. Only the engine's own `screenshot`
#     command sees the real frame.
#   * Booting lands in the menu, and joining needs two separate commands
#     (start the match, then pick a team), each with its own settle time.
#
# Usage:
#   scripts/dev-shot.sh NAME [--join] [--host ID]
#     --join   start a bot match and spawn in first (needed after a restart)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GDR_DEVICE="${GDR_DEVICE:-marinesurface}"
SHOT_NAME=""
DO_JOIN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --join) DO_JOIN=1 ;;
        --host) GDR_DEVICE="${2:?--host needs a device id}"; shift ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) SHOT_NAME="$1" ;;
    esac
    shift
done
[ -n "$SHOT_NAME" ] || { echo "usage: dev-shot.sh NAME [--join]" >&2; exit 2; }

gdr_field() {
    python3 - "$GDR_DEVICE" "$1" <<'PY'
import json, os, sys
cfg = json.load(open(os.path.expanduser("~/.config/gdr/config.json")))
print(cfg["hosts"][sys.argv[1]].get(sys.argv[2]) or "")
PY
}

SSH_TARGET="$(gdr_field ssh)"
SSH_PASS="$(gdr_field sudo_password)"
[ -n "$SSH_TARGET" ] || { echo "no ssh target for device '$GDR_DEVICE'" >&2; exit 1; }

run_ssh() {
    SSHPASS="$SSH_PASS" sshpass -e ssh -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 "$SSH_TARGET" "$@"
}

# gdr's CLI takes evdev keycodes, not names. These match the binds written by
# scripts/dev-deploy.sh.
KEY_F7=65    # cmd join
KEY_F9=67    # exec xt-match.cfg
KEY_F12=88   # screenshot xt-shot.jpg
KEY_SPACE=57 # respawn

tap() { gdr --dev="$GDR_DEVICE" key "$1" >/dev/null; }
log() { printf '\033[36m==>\033[0m %s\n' "$*"; }

REMOTE_USERDIR="$(run_ssh 'echo $HOME')/.xonotic/data"

if [ "$DO_JOIN" = 1 ]; then
    log "starting bot match"
    tap "$KEY_F9"
    # Map load on this device is slow enough that polling beats a fixed sleep.
    for _ in $(seq 1 40); do
        run_ssh "grep -q 'Server spawned' /tmp/xonotic-dev.log" && break
        sleep 2
    done
    sleep 8
    log "joining"
    tap "$KEY_F7"
    sleep 3
    tap "$KEY_SPACE"
    sleep 2
fi

OUT="$ROOT/docs/test-runs/current/$SHOT_NAME.jpg"
mkdir -p "$(dirname "$OUT")"
run_ssh "rm -f '$REMOTE_USERDIR/xt-shot.jpg'"
tap "$KEY_F12"
sleep 2
SSHPASS="$SSH_PASS" sshpass -e scp -q -o StrictHostKeyChecking=no \
    "$SSH_TARGET:$REMOTE_USERDIR/xt-shot.jpg" "$OUT" || {
    echo "no engine screenshot — game may still be in the menu" >&2
    exit 1
}
log "$OUT"
