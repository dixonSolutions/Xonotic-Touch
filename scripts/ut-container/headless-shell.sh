#!/bin/bash
# An isolated, headless gnome-shell for driving the game by touch: its own
# session bus, a virtual monitor, nothing shared with the desktop you sit at.
#
#   headless-shell.sh start [wayland-N] [WxH]   prints the bus address to use
#   headless-shell.sh stop  [wayland-N]
#
# drive.py talks to it through org.gnome.Mutter.RemoteDesktop (touch, keys) and
# org.gnome.Shell.Screencast (recording), with DBUS_SESSION_BUS_ADDRESS set to
# the address in $STATE/bus-<wayland-N>. Name the socket wayland-<digit>: a
# click profile only lets the app connect to /run/user/*/wayland-[0-9]*. Pick
# one your desktop is not using.
set -euo pipefail

cmd="${1:?usage: headless-shell.sh start|stop [wayland-N] [WxH]}"
sock="${2:-wayland-7}"
size="${3:-2340x1080}"
STATE="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/ut-headless"
mkdir -p "$STATE"

case "$cmd" in
start)
    if [ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]; then
        echo "headless-shell: $sock already exists; pick another wayland-N" >&2
        exit 1
    fi
    rm -f "$STATE/bus-$sock"
    setsid dbus-run-session -- bash -c "echo \$DBUS_SESSION_BUS_ADDRESS > '$STATE/bus-$sock'; \
        echo \$\$ > '$STATE/pid-$sock'; \
        exec gnome-shell --headless --wayland --no-x11 --virtual-monitor $size --wayland-display $sock" \
        > "$STATE/shell-$sock.log" 2>&1 < /dev/null &
    for _ in $(seq 60); do
        [ -s "$STATE/bus-$sock" ] && [ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ] && break
        sleep 0.5
    done
    echo "DBUS_SESSION_BUS_ADDRESS=$(cat "$STATE/bus-$sock")"
    ;;
stop)
    if [ -f "$STATE/pid-$sock" ]; then
        kill "$(cat "$STATE/pid-$sock")" 2>/dev/null || true
        rm -f "$STATE/pid-$sock" "$STATE/bus-$sock"
        echo "stopped $sock"
    fi
    ;;
*)
    echo "headless-shell: unknown command $cmd" >&2
    exit 2
    ;;
esac
