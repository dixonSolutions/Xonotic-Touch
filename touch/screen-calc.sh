#!/bin/sh
# Screen calculation layer — landscape dimensions and touch DPI for Xonotic Touch.
# Sourced by packaging/start.sh (not executed directly).
#
# Override for testing:
#   XONOTIC_SCREEN_WIDTH=2400 XONOTIC_SCREEN_HEIGHT=1080
#   XONOTIC_PHYS_MM_W=97 XONOTIC_PHYS_MM_H=214
#   XONOTIC_GNOME_SCALE=2
#   XONOTIC_RENDER_MAX_EDGE=1600   # cap framebuffer for laggy GPUs

xonotic_screen_log() {
    echo "xonotic-screen: $*" >&2
}

xonotic_align_even() {
    local v="$1"
    if [ "$((v % 2))" -ne 0 ]; then
        v=$((v - 1))
    fi
    if [ "$v" -lt 2 ]; then
        v=2
    fi
    echo "$v"
}

xonotic_parse_pair() {
    local text="$1"
    local w h pair
    # Prefer the first WxH token. Do NOT use greedy '.*([0-9]+)x' — that turns
    # 2880x1920 into w=0 (sed keeps only the last digit before x).
    pair=$(echo "$text" | grep -oE '[0-9]{3,5}x[0-9]{3,5}' | head -1)
    w=${pair%%x*}
    h=${pair##*x}
    if [ -n "$w" ] && [ -n "$h" ] && [ "$w" -gt 0 ] && [ "$h" -gt 0 ]; then
        XONOTIC_RAW_WIDTH="$w"
        XONOTIC_RAW_HEIGHT="$h"
        return 0
    fi
    return 1
}

xonotic_parse_phys_mm() {
    local text="$1"
    local wmm hmm
    wmm=$(echo "$text" | sed -n 's/.*\([0-9][0-9]*\)mm x \([0-9][0-9]*\)mm.*/\1/p' | head -1)
    hmm=$(echo "$text" | sed -n 's/.*\([0-9][0-9]*\)mm x \([0-9][0-9]*\)mm.*/\2/p' | head -1)
    if [ -n "$wmm" ] && [ -n "$hmm" ] && [ "$wmm" -gt 0 ] && [ "$hmm" -gt 0 ]; then
        XONOTIC_PHYS_MM_W="$wmm"
        XONOTIC_PHYS_MM_H="$hmm"
        return 0
    fi
    return 1
}

xonotic_detect_raw_dimensions() {
    XONOTIC_RAW_WIDTH=""
    XONOTIC_RAW_HEIGHT=""
    XONOTIC_PHYS_MM_W="${XONOTIC_PHYS_MM_W:-}"
    XONOTIC_PHYS_MM_H="${XONOTIC_PHYS_MM_H:-}"
    XONOTIC_GNOME_SCALE="${XONOTIC_GNOME_SCALE:-}"

    if [ -n "${XONOTIC_SCREEN_WIDTH:-}" ] && [ -n "${XONOTIC_SCREEN_HEIGHT:-}" ]; then
        XONOTIC_RAW_WIDTH="$XONOTIC_SCREEN_WIDTH"
        XONOTIC_RAW_HEIGHT="$XONOTIC_SCREEN_HEIGHT"
        xonotic_screen_log "using env XONOTIC_SCREEN_WIDTH/HEIGHT ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT}"
        return 0
    fi

    # GNOME Wayland (Ultramarine / Surface): gdctl knows mode + scale.
    # Example tree:
    #   └──2880x1920@120.000
    #   ├──Scale: 2.0
    if command -v gdctl >/dev/null 2>&1; then
        local out mode scale
        out=$(gdctl show 2>/dev/null || true)
        if [ -n "$out" ]; then
            mode=$(echo "$out" | grep -oE '[0-9]{3,5}x[0-9]{3,5}(@[0-9.]+)?' | head -1 | cut -d@ -f1)
            scale=$(echo "$out" | sed -n 's/.*[Ss]cale: *\([0-9][0-9]*\.[0-9]*\).*/\1/p' | head -1)
            if [ -z "$scale" ]; then
                scale=$(echo "$out" | sed -n 's/.*[Ss]cale: *\([0-9][0-9]*\).*/\1/p' | head -1)
            fi
            if [ -n "$mode" ] && xonotic_parse_pair "$mode"; then
                XONOTIC_GNOME_SCALE="${scale:-1}"
                xonotic_screen_log "gdctl ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT} scale ${XONOTIC_GNOME_SCALE}"
                return 0
            fi
        fi
    fi

    # Mutter DisplayConfig (GNOME Wayland without gdctl).
    if command -v gdbus >/dev/null 2>&1 && [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
        local mutter mode
        mutter=$(gdbus call --session \
            --dest org.gnome.Mutter.DisplayConfig \
            --object-path /org/gnome/Mutter/DisplayConfig \
            --method org.gnome.Mutter.DisplayConfig.GetCurrentState 2>/dev/null || true)
        if [ -n "$mutter" ]; then
            mode=$(echo "$mutter" | grep -oE '[0-9]{3,5}, [0-9]{3,5}' | head -1 | tr -d ' ' | tr ',' 'x')
            if [ -n "$mode" ] && xonotic_parse_pair "$mode"; then
                xonotic_screen_log "mutter ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT}"
                return 0
            fi
        fi
    fi

    if command -v mirout >/dev/null 2>&1; then
        local line
        line=$(mirout 2>/dev/null | grep -E 'connected.*[0-9]+x[0-9]+' | head -1)
        if [ -n "$line" ] && xonotic_parse_pair "$line"; then
            xonotic_parse_phys_mm "$line" || true
            xonotic_screen_log "mirout ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT}"
            return 0
        fi
    fi

    if [ -r /sys/class/graphics/fb0/virtual_size ]; then
        local fb
        fb=$(tr ',' ' ' < /sys/class/graphics/fb0/virtual_size)
        XONOTIC_RAW_WIDTH=$(echo "$fb" | awk '{print $1}')
        XONOTIC_RAW_HEIGHT=$(echo "$fb" | awk '{print $2}')
        if [ -n "$XONOTIC_RAW_WIDTH" ] && [ -n "$XONOTIC_RAW_HEIGHT" ] && [ "$XONOTIC_RAW_WIDTH" -gt 0 ]; then
            xonotic_screen_log "fb0 ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT}"
            return 0
        fi
    fi

    if command -v xdpyinfo >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
        local dims
        dims=$(xdpyinfo 2>/dev/null | awk '/dimensions:/{print $2}')
        if [ -n "$dims" ]; then
            XONOTIC_RAW_WIDTH="${dims%%x*}"
            XONOTIC_RAW_HEIGHT="${dims#*x}"
            xonotic_screen_log "xdpyinfo ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT}"
            return 0
        fi
    fi

    if command -v wlr-randr >/dev/null 2>&1; then
        local mode
        mode=$(wlr-randr 2>/dev/null | awk '/current/ {print $1; exit}')
        if [ -n "$mode" ] && xonotic_parse_pair "$mode"; then
            xonotic_screen_log "wlr-randr ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT}"
            return 0
        fi
    fi

    XONOTIC_RAW_WIDTH="${XONOTIC_DEFAULT_WIDTH:-1920}"
    XONOTIC_RAW_HEIGHT="${XONOTIC_DEFAULT_HEIGHT:-1080}"
    xonotic_screen_log "fallback ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT}"
}

xonotic_landscape_dims() {
    local w="$1"
    local h="$2"
    # Touch layout is two-thumb landscape. Always present landscape framebuffer.
    if [ "$w" -ge "$h" ]; then
        XONOTIC_VID_WIDTH="$w"
        XONOTIC_VID_HEIGHT="$h"
        XONOTIC_ORIENTATION="landscape"
    else
        XONOTIC_VID_WIDTH="$h"
        XONOTIC_VID_HEIGHT="$w"
        XONOTIC_ORIENTATION="landscape-rotated"
    fi
    XONOTIC_VID_WIDTH=$(xonotic_align_even "$XONOTIC_VID_WIDTH")
    XONOTIC_VID_HEIGHT=$(xonotic_align_even "$XONOTIC_VID_HEIGHT")
}

xonotic_apply_render_budget() {
    # HiDPI tablets (e.g. Surface 2880×1920 @ scale 2) choke Iris Xe at native.
    # Prefer logical size (physical / scale), then cap longest edge.
    local scale="${XONOTIC_GNOME_SCALE:-1}"
    # Fanless tablets (Surface Pro 4 class): default 960px long edge.
    local max_edge="${XONOTIC_RENDER_MAX_EDGE:-960}"
    local w="$XONOTIC_VID_WIDTH"
    local h="$XONOTIC_VID_HEIGHT"

    if awk "BEGIN { exit !($scale > 1.01) }"; then
        w=$(awk "BEGIN { printf \"%d\", $w / $scale }")
        h=$(awk "BEGIN { printf \"%d\", $h / $scale }")
        xonotic_screen_log "logical via scale ${scale}: ${w}x${h}"
    fi

    local long short
    if [ "$w" -ge "$h" ]; then
        long=$w
        short=$h
    else
        long=$h
        short=$w
    fi
    if [ "$long" -gt "$max_edge" ]; then
        short=$(awk "BEGIN { printf \"%d\", $short * $max_edge / $long }")
        long=$max_edge
        if [ "$w" -ge "$h" ]; then
            w=$long
            h=$short
        else
            w=$short
            h=$long
        fi
        xonotic_screen_log "render budget max_edge=${max_edge}: ${w}x${h}"
    fi

    XONOTIC_VID_WIDTH=$(xonotic_align_even "$w")
    XONOTIC_VID_HEIGHT=$(xonotic_align_even "$h")
}

xonotic_calc_dpi() {
    local w="$1"
    local h="$2"
    local wmm="${XONOTIC_PHYS_MM_W:-}"
    local hmm="${XONOTIC_PHYS_MM_H:-}"

    if [ -n "$wmm" ] && [ -n "$hmm" ]; then
        XONOTIC_TOUCH_XDPI=$(awk "BEGIN { printf \"%.0f\", $w / ($wmm / 25.4) }")
        XONOTIC_TOUCH_YDPI=$(awk "BEGIN { printf \"%.0f\", $h / ($hmm / 25.4) }")
    else
        # Density is no longer multiplied into widget size; keep a mild default
        # so engine touch paths that still read it stay sane.
        XONOTIC_TOUCH_XDPI="${XONOTIC_TOUCH_XDPI:-160}"
        XONOTIC_TOUCH_YDPI="${XONOTIC_TOUCH_YDPI:-160}"
    fi

    XONOTIC_TOUCH_DENSITY=$(awk "BEGIN { d=($XONOTIC_TOUCH_XDPI + $XONOTIC_TOUCH_YDPI) / 320; if (d < 1) d=1; if (d > 1.5) d=1.5; printf \"%.2f\", d }")
}

xonotic_write_layout_file() {
    local path="$1"
    if [ -z "$path" ]; then
        return 0
    fi
    cat > "$path" <<EOF
// Generated by touch/screen-calc.sh — touch HUD layout
// raw ${XONOTIC_RAW_WIDTH}x${XONOTIC_RAW_HEIGHT} -> vid ${XONOTIC_VID_WIDTH}x${XONOTIC_VID_HEIGHT} (${XONOTIC_ORIENTATION})
//
// vid_conwidthauto is deliberately ON. Detection here is best-effort — inside
// the Flatpak sandbox gdctl/fb0 are not reachable and we fall back to a guess —
// and a Wayland compositor hands us whatever fullscreen surface it likes
// regardless of vid_width/vid_height. Pinning vid_conwidth to a guessed width
// is what produced a 16:9 console on a 3:2 panel, which stretched every touch
// control and moved every hit box. Letting the engine derive conwidth from the
// real surface keeps 2D space square on any display.
vid_width ${XONOTIC_VID_WIDTH}
vid_height ${XONOTIC_VID_HEIGHT}
vid_conwidthauto 1
vid_conheight ${XONOTIC_VID_HEIGHT}
vid_touchscreen_xdpi ${XONOTIC_TOUCH_XDPI}
vid_touchscreen_ydpi ${XONOTIC_TOUCH_YDPI}
vid_touchscreen_density ${XONOTIC_TOUCH_DENSITY}
EOF
}

# Main entry — sets globals and optional layout cfg path in $1.
xonotic_screen_calc() {
    local layout_file="${1:-}"

    xonotic_detect_raw_dimensions
    xonotic_landscape_dims "$XONOTIC_RAW_WIDTH" "$XONOTIC_RAW_HEIGHT"
    xonotic_apply_render_budget
    xonotic_calc_dpi "$XONOTIC_VID_WIDTH" "$XONOTIC_VID_HEIGHT"
    xonotic_write_layout_file "$layout_file"

    xonotic_screen_log "vid ${XONOTIC_VID_WIDTH}x${XONOTIC_VID_HEIGHT} dpi ${XONOTIC_TOUCH_XDPI}x${XONOTIC_TOUCH_YDPI} density ${XONOTIC_TOUCH_DENSITY} orient ${XONOTIC_ORIENTATION}"
}
