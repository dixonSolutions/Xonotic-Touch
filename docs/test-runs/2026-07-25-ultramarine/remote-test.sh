#!/usr/bin/env bash
# Run ON the Ultramarine host after SSH works.
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/dixonSolutions/Xonotic-Touch.git}"
REPO_DIR="${REPO_DIR:-$HOME/Projects/Xonotic-Touch}"
SHOT_DIR="${SHOT_DIR:-$HOME/xonotic-touch-test/screenshots}"
LOG_DIR="${LOG_DIR:-$HOME/xonotic-touch-test/logs}"
mkdir -p "$SHOT_DIR" "$LOG_DIR" "$(dirname "$REPO_DIR")"

stamp() { date -Iseconds; }
log() { echo "[$(stamp)] $*" | tee -a "$LOG_DIR/run.log"; }

export DISPLAY="${DISPLAY:-:0}"
# Prefer active graphical session runtime
if [[ -z "${XDG_RUNTIME_DIR:-}" ]]; then
  export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi
if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
  for cand in wayland-0 wayland-1; do
    if [[ -S "$XDG_RUNTIME_DIR/$cand" ]]; then
      export WAYLAND_DISPLAY="$cand"
      break
    fi
  done
fi

log "USER=$USER WAYLAND_DISPLAY=${WAYLAND_DISPLAY-} DISPLAY=$DISPLAY XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"

shot() {
  local name="$1"
  local out="$SHOT_DIR/${name}.png"
  if command -v grim >/dev/null 2>&1 && [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
    grim "$out" && log "shot grim -> $out"
  elif command -v gnome-screenshot >/dev/null 2>&1; then
    gnome-screenshot -f "$out" && log "shot gnome-screenshot -> $out"
  else
    log "WARN: no screenshot tool"
    return 1
  fi
}

ensure_tools() {
  if ! command -v grim >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1; then
    log "Installing grim/git/ydotool via dnf (may need sudo)"
    echo '1122' | sudo -S dnf install -y grim git ydotool wl-clipboard 2>&1 | tee -a "$LOG_DIR/dnf.log" || true
  fi
}

sync_repo() {
  if [[ -d "$REPO_DIR/.git" ]]; then
    log "Updating $REPO_DIR"
    git -C "$REPO_DIR" fetch --all --prune
    git -C "$REPO_DIR" pull --ff-only || git -C "$REPO_DIR" reset --hard origin/main
  else
    log "Cloning into $REPO_DIR"
    git clone --depth 1 "$REPO_URL" "$REPO_DIR"
  fi
  git -C "$REPO_DIR" log -1 --oneline | tee -a "$LOG_DIR/run.log"
}

launch_game() {
  cd "$REPO_DIR"
  # Prefer fast native path if binary exists; else Flatpak remote install
  if [[ -x ./scripts/run-local.sh ]] && [[ -x ./engine/darkplaces/darkplaces-sdl || -x ./xonotic-linux-sdl.sh ]]; then
    log "Launching run-local.sh"
    nohup ./scripts/run-local.sh >"$LOG_DIR/game.log" 2>&1 &
    echo $! >"$LOG_DIR/game.pid"
  else
    log "Ensuring Flatpak app"
    flatpak remote-add --user --if-not-exists xonotic-touch \
      https://dixonSolutions.github.io/Xonotic-Touch/flatpak || true
    flatpak install -y --user xonotic-touch io.github.dixonSolutions.XonoticTouch || true
    log "Launching flatpak"
    nohup flatpak run io.github.dixonSolutions.XonoticTouch >"$LOG_DIR/game.log" 2>&1 &
    echo $! >"$LOG_DIR/game.pid"
  fi
}

asset_status() {
  local data="$HOME/.local/share/xonotic-touch/data"
  [[ -f "$data/.assets-ready" ]] && echo ready || echo missing
  [[ -f "$data/.asset-fetch-progress" ]] && cat "$data/.asset-fetch-progress" || true
  ls "$data" 2>/dev/null | head -40 || true
}

main() {
  ensure_tools
  sync_repo
  shot "00-desktop-before"
  launch_game
  sleep 8
  shot "01-after-launch"
  log "asset status: $(asset_status | head -1)"
  asset_status | tee "$LOG_DIR/assets.txt"
  sleep 15
  shot "02-warmup"
  log "done bootstrap launch — continue interactive taps from controller host"
}

main "$@"
