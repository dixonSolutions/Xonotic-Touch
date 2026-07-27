#!/usr/bin/env bash
# Acceptance smoke for glass layout on Ultramarine (ydotool + in-engine screenshot).
set -euo pipefail

export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/1000/bus}"

SHOT_DIR="${SHOT_DIR:-$HOME/xonotic-touch-test/screenshots/glass}"
LOG="$HOME/xonotic-touch-test/logs/glass-accept.log"
mkdir -p "$SHOT_DIR" "$(dirname "$LOG")"
: >"$LOG"

log() { echo "[$(date -Iseconds)] $*" | tee -a "$LOG"; }

# ydotool absolute mouse under Mutter/Wayland uses *logical* compositor coords.
# Panel 2880x1920 @ GNOME scale 2 → logical 1440x960. Mapping to physical
# (2880x1920) lands taps ~2× too far and misses widgets.
LOGIC_W="${LOGIC_W:-1440}"
LOGIC_H="${LOGIC_H:-960}"

# Engine may write under userdir/gamedir → ~/.xonotic/data/data/touch.layout.cfg
LAYOUT_CANDIDATES=(
  "$HOME/.xonotic/data/data/touch.layout.cfg"
  "$HOME/.xonotic/data/touch.layout.cfg"
  "$HOME/.local/share/xonotic-touch/data/data/touch.layout.cfg"
  "$HOME/.local/share/xonotic-touch/data/touch.layout.cfg"
)

to_logic() {
  # args: nx ny (0..1 of game viewport assumed fullscreen)
  local nx="$1" ny="$2"
  echo "$(awk -v n="$nx" -v w="$LOGIC_W" 'BEGIN{printf "%d", n*w+0.5}')" \
       "$(awk -v n="$ny" -v h="$LOGIC_H" 'BEGIN{printf "%d", n*h+0.5}')"
}

tap() {
  local nx="$1" ny="$2"
  read -r x y <<<"$(to_logic "$nx" "$ny")"
  log "tap $nx,$ny -> $x $y (logical ${LOGIC_W}x${LOGIC_H})"
  ydotool mousemove --absolute -x "$x" -y "$y" 2>>"$LOG" || true
  sleep 0.05
  ydotool click 0xC0 2>>"$LOG" || ydotool click 1 2>>"$LOG" || true
  sleep 0.15
}

hold() {
  local nx="$1" ny="$2" ms="$3"
  read -r x y <<<"$(to_logic "$nx" "$ny")"
  log "hold $nx,$ny ${ms}ms -> $x $y"
  ydotool mousemove --absolute -x "$x" -y "$y" 2>>"$LOG" || true
  sleep 0.05
  ydotool click 0x40 2>>"$LOG" || true  # down
  sleep "$(awk -v m="$ms" 'BEGIN{printf "%.3f", m/1000}')"
  ydotool click 0x80 2>>"$LOG" || true  # up
  sleep 0.2
}

drag() {
  local nx0="$1" ny0="$2" nx1="$3" ny1="$4" ms="${5:-400}"
  read -r x0 y0 <<<"$(to_logic "$nx0" "$ny0")"
  read -r x1 y1 <<<"$(to_logic "$nx1" "$ny1")"
  log "drag $nx0,$ny0 -> $nx1,$ny1"
  ydotool mousemove --absolute -x "$x0" -y "$y0" 2>>"$LOG" || true
  sleep 0.05
  ydotool click 0x40 2>>"$LOG" || true
  sleep "$(awk -v m="$ms" 'BEGIN{printf "%.3f", m/1000}')"
  ydotool mousemove --absolute -x "$x1" -y "$y1" 2>>"$LOG" || true
  sleep 0.1
  ydotool click 0x80 2>>"$LOG" || true
  sleep 0.2
}

log "=== glass accept start ==="
log "power=$(powerprofilesctl get 2>/dev/null || echo unknown)"
log "coord space: logical ${LOGIC_W}x${LOGIC_H}"
pgrep -a xonotic | tee -a "$LOG" || log "WARN: xonotic not running"

# Give game focus
sleep 2

# T1 layout — screenshot via console if possible
# Open console with backtick is hard; use CON handle tap at top-middle
tap 0.500 0.045
sleep 0.5
# type screenshot if console opened — use ydotool key
if command -v ydotool >/dev/null; then
  # type: screenshot
  ydotool type "screenshot" 2>>"$LOG" || true
  ydotool key 28:1 28:0 2>>"$LOG" || true  # enter
  sleep 0.8
  tap 0.500 0.045  # close console
fi

# FIRE tap cluster (T8 sample)
for i in 1 2 3 4 5; do
  tap 0.855 0.735
done

# SPACE hold
hold 0.640 0.880 400

# MOVE nudge
drag 0.150 0.760 0.150 0.650 300

# LOOK swipe
drag 0.700 0.450 0.850 0.450 350

# CON hold-drag (T13) — then explicit save so disk check can pass
hold 0.500 0.045 450
drag 0.500 0.045 0.800 0.200 500
tap 0.500 0.045
sleep 0.3
ydotool type "touch_save" 2>>"$LOG" || true
ydotool key 28:1 28:0 2>>"$LOG" || true
sleep 0.5
tap 0.500 0.045

# Final screenshot attempt
tap 0.500 0.045
sleep 0.3
ydotool type "screenshot" 2>>"$LOG" || true
ydotool key 28:1 28:0 2>>"$LOG" || true
sleep 1
tap 0.500 0.045

# Collect screenshots
ls -lt "$HOME/.xonotic/data/screenshots" 2>/dev/null | head -10 | tee -a "$LOG" || true
ls -lt "$HOME/.local/share/xonotic-touch/data/screenshots" 2>/dev/null | head -10 | tee -a "$LOG" || true

# Layout persistence (engine path is often data/data/)
log "layout file:"
found=0
for f in "${LAYOUT_CANDIDATES[@]}"; do
  if [[ -f "$f" ]]; then
    found=1
    ls -la "$f" | tee -a "$LOG"
    grep -E "touch_con_|touch_glass" "$f" | tee -a "$LOG" || true
  fi
done
if [[ "$found" -eq 0 ]]; then
  log "no layout yet (checked ${#LAYOUT_CANDIDATES[@]} candidates)"
fi

log "=== glass accept done ==="
