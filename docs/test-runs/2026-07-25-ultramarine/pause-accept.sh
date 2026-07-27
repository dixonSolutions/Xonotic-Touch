#!/usr/bin/env bash
# Pause-escape acceptance: PAUSE pill → sheet → RESUME / MENU round-trip.
set -euo pipefail

export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/1000/bus}"

SHOT_DIR="${SHOT_DIR:-$HOME/xonotic-touch-test/screenshots/pause}"
LOG="$HOME/xonotic-touch-test/logs/pause-accept.log"
mkdir -p "$SHOT_DIR" "$(dirname "$LOG")"
: >"$LOG"

log() { echo "[$(date -Iseconds)] $*" | tee -a "$LOG"; }

LOGIC_W="${LOGIC_W:-1440}"
LOGIC_H="${LOGIC_H:-960}"

to_logic() {
  local nx="$1" ny="$2"
  echo "$(awk -v n="$nx" -v w="$LOGIC_W" 'BEGIN{printf "%d", n*w+0.5}')" \
       "$(awk -v n="$ny" -v h="$LOGIC_H" 'BEGIN{printf "%d", n*h+0.5}')"
}

tap() {
  local nx="$1" ny="$2"
  read -r x y <<<"$(to_logic "$nx" "$ny")"
  log "tap $nx,$ny -> $x $y"
  ydotool mousemove --absolute -x "$x" -y "$y" 2>>"$LOG" || true
  sleep 0.05
  ydotool click 0xC0 2>>"$LOG" || ydotool click 1 2>>"$LOG" || true
  sleep 0.25
}

engine_screenshot() {
  local tag="$1"
  # Prefer console command via CON pill
  tap 0.500 0.050
  sleep 0.4
  ydotool type "screenshot" 2>>"$LOG" || true
  ydotool key 28:1 28:0 2>>"$LOG" || true
  sleep 0.9
  tap 0.500 0.050
  sleep 0.3
  # Collect newest screenshots
  local newest
  newest="$(ls -t "$HOME"/.xonotic/data/*.tga "$HOME"/.xonotic/data/data/*.tga 2>/dev/null | head -1 || true)"
  if [ -n "$newest" ]; then
    cp -f "$newest" "$SHOT_DIR/${tag}.tga"
    log "saved $SHOT_DIR/${tag}.tga from $newest"
  else
    log "WARN: no screenshot file found for $tag"
  fi
}

log "=== pause accept start ==="
pgrep -a xonotic | tee -a "$LOG" || { log "ERROR: xonotic not running"; exit 1; }
sleep 1

# Ensure we are in-game: try quick menu dismiss / click center
tap 0.50 0.55
sleep 0.5

log "P2: tap PAUSE pill"
tap 0.300 0.050
sleep 0.8
engine_screenshot "p2-sheet-open"

log "P5: tap RESUME"
tap 0.500 0.313
sleep 0.8
engine_screenshot "p5-resumed"

log "P2 again then P6 MENU"
tap 0.300 0.050
sleep 0.6
tap 0.500 0.505
sleep 1.2
engine_screenshot "p6-menu-open"

log "P6b: tap menu RESUME bar (top-left)"
tap 0.10 0.05
sleep 0.8
engine_screenshot "p6-menu-resumed"

log "P12: open sheet and tap FIRE — should not shoot forever"
tap 0.300 0.050
sleep 0.5
tap 0.860 0.640
sleep 0.3
tap 0.500 0.313
sleep 0.5
engine_screenshot "p12-modal"

log "=== pause accept done ==="
ls -la "$SHOT_DIR" | tee -a "$LOG"
