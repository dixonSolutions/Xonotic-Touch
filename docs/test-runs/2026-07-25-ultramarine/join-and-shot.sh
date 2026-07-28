#!/bin/sh
set -e
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export YDOTOOL_SOCKET="${YDOTOOL_SOCKET:-/run/user/1000/.ydotool_socket}"

sleep 2
ydotool key 1:1 1:0
sleep 0.3
ydotool key 41:1 41:0
sleep 0.8
ydotool type "maxplayers 2"
ydotool key 28:1 28:0
sleep 0.3
ydotool type "map courtfun"
ydotool key 28:1 28:0
sleep 14
ydotool key 41:1 41:0
sleep 0.8
ydotool type "join"
ydotool key 28:1 28:0
sleep 2
ydotool key 41:1 41:0
sleep 0.3
for y in 1050 1150 1250 1350; do
  ydotool mousemove --absolute -x 1440 -y "$y"
  sleep 0.05
  ydotool click 0xC0 || true
  sleep 0.25
done
sleep 1
ydotool key 1:1 1:0
sleep 0.4
ydotool key 41:1 41:0
sleep 0.8
ydotool type "touch_debug 1"
ydotool key 28:1 28:0
sleep 0.3
ydotool type "screenshot"
ydotool key 28:1 28:0
sleep 1
ydotool key 41:1 41:0
sleep 0.8
ydotool key 41:1 41:0
sleep 0.8
ydotool type "screenshot"
ydotool key 28:1 28:0
sleep 1
ydotool key 41:1 41:0
ls -lt "$HOME/.xonotic/data/screenshots" | head -6
grep -E "program loaded \(crc" "$HOME/xonotic-touch-test/logs/glass-launch3.log" | tail -5
