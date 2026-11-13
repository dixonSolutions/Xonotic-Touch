#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a GUI command on a private Xvfb display, never on the user's session.
#
#   loader/scripts/run-headless.sh [--size WxH] -- <command> [args...]
#
# Starts `Xvfb :<free display>`, strips WAYLAND_DISPLAY, DISPLAY, XAUTHORITY,
# DBUS_SESSION_BUS_ADDRESS and the portal/notification variables, sets
# DISPLAY to the Xvfb and GDK_BACKEND=x11, checks the child's environment, runs
# the command with a parent-death signal (setpriv/prctl via `timeout --foreground`
# fallback), and kills the Xvfb and anything left on it afterwards.
# Commands starting with `flatpak run` get --nosocket=wayland --socket=x11 and
# the same environment, so a sandboxed run cannot reach the user's display either.
set -euo pipefail
SIZE=1920x1200
while [ $# -gt 0 ]; do
    case "$1" in
        --size) SIZE="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done
[ $# -gt 0 ] || { echo "usage: $0 [--size WxH] -- command..." >&2; exit 2; }

# A free display number from 90 up.
N=90
while [ -e "/tmp/.X11-unix/X$N" ] || [ -e "/tmp/.X$N-lock" ]; do N=$((N + 1)); done
Xvfb ":$N" -screen 0 "${SIZE}x24" -nolisten tcp >/dev/null 2>&1 &
XVFB=$!
cleanup() {
    # Anything still connected to our display goes, then the server.
    pkill -f -- "--env=DISPLAY=:$N" 2>/dev/null || true
    kill "$XVFB" 2>/dev/null || true
    wait "$XVFB" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
for _ in $(seq 50); do [ -e "/tmp/.X11-unix/X$N" ] && break; sleep 0.1; done

CLEAN=(env -u WAYLAND_DISPLAY -u DISPLAY -u XAUTHORITY -u DBUS_SESSION_BUS_ADDRESS -u XDG_SESSION_TYPE
       -u WAYLAND_SOCKET -u SWAYSOCK -u HYPRLAND_INSTANCE_SIGNATURE
       DISPLAY=":$N" GDK_BACKEND=x11 NO_AT_BRIDGE=1 GTK_A11Y=none GSK_RENDERER="${GSK_RENDERER:-cairo}")

if [ "$1" = flatpak ] && [ "${2:-}" = run ]; then
    shift 2
    set -- flatpak run --nosocket=wayland --nosocket=fallback-x11 --socket=x11 --nosocket=session-bus \
        --unset-env=WAYLAND_DISPLAY --env=DISPLAY=":$N" --env=GDK_BACKEND=x11 --env=NO_AT_BRIDGE=1 \
        --env=GTK_A11Y=none --env=GSK_RENDERER="${GSK_RENDERER:-cairo}" "$@"
fi

# Refuse to run if the environment would still reach the user's display.
"${CLEAN[@]}" sh -c '[ -z "${WAYLAND_DISPLAY:-}" ] && [ "$DISPLAY" = ":'"$N"'" ]' \
    || { echo "run-headless: environment still points at the user's display; refusing" >&2; exit 3; }
echo "run-headless: DISPLAY=:$N (Xvfb $SIZE), no WAYLAND_DISPLAY" >&2
set +e
"${CLEAN[@]}" setpriv --pdeathsig KILL -- "$@"
rc=$?
set -e
exit $rc
