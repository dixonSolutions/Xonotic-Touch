#!/usr/bin/env bash
# Run a multi-finger touch script against the game on a test device and pull
# back in-engine screenshots.
#
# Screenshots are taken by the engine itself (`key F12`) rather than by the
# compositor: while the game holds a fullscreen direct-scanout surface, GNOME's
# screencast portal returns solid black.
#
# Usage:
#   scripts/dev-test.sh <script.touch> [--name PREFIX] [--host ID]
#   scripts/dev-test.sh - --name probe   # read script from stdin
#
# Script syntax is documented in scripts/touch-inject.py.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GDR_DEVICE="${GDR_DEVICE:-marinesurface}"
SCRIPT_ARG="${1:?usage: dev-test.sh <script.touch|-> [--name PREFIX]}"
shift || true
NAME="probe"

while [ $# -gt 0 ]; do
    case "$1" in
        --name) NAME="${2:?--name needs a value}"; shift ;;
        --host) GDR_DEVICE="${2:?--host needs a device id}"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# Read the touch script before anything else: ssh without -n consumes stdin, so
# a later remote call would otherwise eat the script.
if [ "$SCRIPT_ARG" = "-" ]; then
    TOUCH_SCRIPT="$(cat)"
else
    TOUCH_SCRIPT="$(cat "$SCRIPT_ARG")"
fi

gdr_field() {
    python3 - "$GDR_DEVICE" "$1" <<'PY'
import json, os, sys
cfg = json.load(open(os.path.expanduser("~/.config/gdr/config.json")))
print(cfg["hosts"][sys.argv[1]].get(sys.argv[2]) or "")
PY
}

SSH_TARGET="$(gdr_field ssh)"
SSH_PASS="$(gdr_field sudo_password)"

run_ssh() {
    SSHPASS="$SSH_PASS" sshpass -e ssh -n -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 "$SSH_TARGET" "$@"
}
# Same, but forwards stdin — used to stream the touch script to the injector.
run_ssh_stdin() {
    SSHPASS="$SSH_PASS" sshpass -e ssh -o StrictHostKeyChecking=no \
        -o ConnectTimeout=10 "$SSH_TARGET" "$@"
}
copy_to() {
    SSHPASS="$SSH_PASS" sshpass -e scp -q -o StrictHostKeyChecking=no "$1" "$SSH_TARGET:$2"
}
copy_from() {
    SSHPASS="$SSH_PASS" sshpass -e scp -q -o StrictHostKeyChecking=no "$SSH_TARGET:$1" "$2"
}

log() { printf '\033[36m==>\033[0m %s\n' "$*"; }

REMOTE_HOME="$(run_ssh 'echo $HOME')"
# The engine's userdir, where `screenshot` lands. The exact filename depends on
# whatever F12 happens to be bound to, so the harness picks the newest image
# written after the script ran rather than assuming a name.
SHOT_DIR="$REMOTE_HOME/.xonotic/data"
OUTDIR="$ROOT/docs/test-runs/current"
mkdir -p "$OUTDIR"

log "syncing injector"
copy_to "$ROOT/scripts/touch-inject.py" /tmp/touch-inject.py

# Panel geometry: injector works in physical pixels.
PANEL_W="${XT_PANEL_W:-2880}"
PANEL_H="${XT_PANEL_H:-1920}"

MARKER="/tmp/xt-run-marker"
run_ssh "touch $MARKER"

log "running touch script ($NAME)"
printf '%s\n' "$TOUCH_SCRIPT" | run_ssh_stdin \
    "python3 /tmp/touch-inject.py - -v --width $PANEL_W --height $PANEL_H" \
    2>&1 | sed 's/^/    /'

sleep 1
NEWEST="$(run_ssh "find $SHOT_DIR -maxdepth 1 \\( -name '*.jpg' -o -name '*.tga' -o -name '*.png' \\) \
                   -newer $MARKER -printf '%T@ %p\\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-")"

if [ -n "$NEWEST" ]; then
    OUT="$OUTDIR/$NAME.${NEWEST##*.}"
    copy_from "$NEWEST" "$OUT"
    log "captured $OUT"
else
    log "no screenshot produced (add a 'key F12' line to the script)"
fi
