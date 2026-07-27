#!/bin/sh
set -e
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/1000}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/1000/bus}"
export XONOTIC_TOUCH_PERF_PROFILE="${XONOTIC_TOUCH_PERF_PROFILE:-thermal}"
export XONOTIC_TOUCH_PROFILE="${XONOTIC_TOUCH_PROFILE:-standard}"
export XONOTIC_RENDER_MAX_EDGE="${XONOTIC_RENDER_MAX_EDGE:-960}"

powerprofilesctl set power-saver 2>/dev/null || true

USER_BASE="$HOME/.local/share/xonotic-touch"
USER_DATA="$USER_BASE/data"
LAYOUT_CFG="$USER_DATA/screen.layout.cfg"
mkdir -p "$USER_DATA/touch/profiles" "$HOME/.xonotic/data"

. "$HOME/xonotic-touch-test/screen-calc.sh"
xonotic_screen_calc "$LAYOUT_CFG"

{
  echo "developer 0"
  echo "prvm_traceqc 0"
  echo "prvm_statementprofiling 0"
  echo "prvm_timeprofiling 0"
  echo "exec touch/profiles/${XONOTIC_TOUCH_PROFILE}.cfg"
  echo "exec touch/profiles/${XONOTIC_TOUCH_PERF_PROFILE}.cfg"
  if [ -f "$HOME/.xonotic/data/touch.layout.cfg" ] || [ -f "$USER_DATA/touch.layout.cfg" ]; then
    echo "exec touch.layout.cfg"
  fi
  echo "touch_debug 1"
  echo "touch_glass_quality 1"
} > "$USER_DATA/touch/startup.cfg"

mkdir -p "$USER_DATA/zzz-touch-fix.pk3dir" "$HOME/.xonotic/data/zzz-touch-fix.pk3dir"
if [ -f "$USER_DATA/zzz-touch-fix.pk3dir/csprogs.dat" ]; then
  cp -f "$USER_DATA/zzz-touch-fix.pk3dir/csprogs.dat" "$HOME/.xonotic/data/zzz-touch-fix.pk3dir/csprogs.dat"
fi

echo "xonotic-glass: ${XONOTIC_VID_WIDTH}x${XONOTIC_VID_HEIGHT} fps_max=30 glass_quality=1" >&2

cd "$USER_BASE"

HOST_BIN="${XONOTIC_TOUCH_HOST_BIN:-$HOME/xonotic-touch-test/xonotic-host}"
if [ -x "$HOST_BIN" ]; then
  echo "xonotic-glass: using host engine $HOST_BIN" >&2
  set -- "$HOST_BIN"
else
  echo "xonotic-glass: using flatpak engine" >&2
  set -- flatpak run --command=/app/bin/xonotic io.github.dixonSolutions.XonoticTouch
fi

exec "$@" \
  -xonotic \
  -customgamename "Xonotic Touch" \
  +exec xonotic.cfg \
  +exec screen.layout.cfg \
  +exec config.cfg \
  +exec autoexec.cfg \
  +exec touch/startup.cfg \
  +set _touch_asset_fetch_active 0 \
  +set _touch_assets_ready 1 \
  +vid_fullscreen 1 \
  +vid_touchscreen 1 \
  +vid_conwidthauto 0 \
  +vid_width "${XONOTIC_VID_WIDTH}" \
  +vid_height "${XONOTIC_VID_HEIGHT}" \
  +vid_conwidth "${XONOTIC_VID_WIDTH}" \
  +vid_conheight "${XONOTIC_VID_HEIGHT}" \
  +vid_touchscreen_xdpi "${XONOTIC_TOUCH_XDPI}" \
  +vid_touchscreen_ydpi "${XONOTIC_TOUCH_YDPI}" \
  +vid_touchscreen_density "${XONOTIC_TOUCH_DENSITY}" \
  +cl_movement 1 \
  +con_closeontoggle 1 \
  +scr_screenshot_jpeg 0 \
  +fps_max 30 \
  +vid_vsync 1 \
  +developer 0 \
  +prvm_traceqc 0 \
  +prvm_statementprofiling 0 \
  +prvm_timeprofiling 0
