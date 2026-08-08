#!/bin/bash
# Shared helpers for Xonotic Touch scripts.
set -euo pipefail

xonotic_root() {
    if [ -n "${ROOT:-}" ]; then
        printf '%s\n' "$ROOT"
        return 0
    fi
    local here
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    ROOT="$here"
    printf '%s\n' "$ROOT"
}

xonotic_bin() {
    printf '%s/build/bin/xonotic\n' "$(xonotic_root)"
}

xonotic_data_dir() {
    printf '%s/engine/data\n' "$(xonotic_root)"
}

xonotic_usage() {
    printf '%s\n' "$1" >&2
    exit "${2:-1}"
}

xonotic_need_cmd() {
    local cmd="$1"
    local hint="${2:-}"
    if command -v "$cmd" >/dev/null 2>&1; then
        return 0
    fi
    printf 'Missing command: %s\n' "$cmd" >&2
    if [ -n "$hint" ]; then
        printf '%s\n' "$hint" >&2
    fi
    return 1
}

xonotic_maybe_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        printf 'Need root privileges to run: %s\n' "$*" >&2
        return 1
    fi
}

xonotic_apt_packages() {
    printf '%s\n' \
        build-essential \
        make \
        gcc \
        git \
        pkg-config \
        libsdl2-dev \
        libjpeg-dev \
        zlib1g-dev \
        libxmp-dev \
        libgmp-dev \
        autoconf \
        automake \
        libtool \
        zip
}

xonotic_compiler() {
    printf '%s' "${CC:-gcc}"
}

xonotic_compiler_triplet() {
    local triplet
    triplet="$("$(xonotic_compiler)" -print-multiarch 2>/dev/null || true)"
    if [ -n "$triplet" ]; then
        printf '%s' "$triplet"
        return 0
    fi
    if [ -n "${ARCH_TRIPLET:-}" ]; then
        printf '%s' "$ARCH_TRIPLET"
        return 0
    fi
    return 1
}

xonotic_apply_cross_compile_env() {
    local triplet="${ARCH_TRIPLET:-}"
    if [ -z "$triplet" ]; then
        case "${ARCH:-}" in
            arm64|aarch64) triplet=aarch64-linux-gnu ;;
            armhf|armv7l) triplet=arm-linux-gnueabihf ;;
        esac
    fi
    if [ -n "$triplet" ] && command -v "${triplet}-gcc" >/dev/null 2>&1; then
        export ARCH_TRIPLET="$triplet"
        export CC="${CC:-${triplet}-gcc}"
        export CXX="${CXX:-${triplet}-g++}"
        export AR="${AR:-${triplet}-ar}"
        export PKG_CONFIG="${PKG_CONFIG:-${triplet}-pkg-config}"
        export PKG_CONFIG_PATH="/usr/lib/${triplet}/pkgconfig:${PKG_CONFIG_PATH:-}"
        printf 'Cross-compiling for %s (CC=%s)\n' "$triplet" "$CC"
    fi
}

xonotic_has_gmp_headers() {
    local triplet
    triplet="$(xonotic_compiler_triplet || true)"
    if [ -n "$triplet" ] && [ -f "/usr/include/${triplet}/gmp.h" ]; then
        return 0
    fi
    test -f /usr/include/gmp.h
}

xonotic_has_native_build_deps() {
    xonotic_need_cmd gcc || return 1
    xonotic_need_cmd make || return 1
    xonotic_need_cmd git || return 1
    pkg-config --exists sdl2 2>/dev/null || return 1
    pkg-config --exists zlib 2>/dev/null || return 1
    pkg-config --exists libjpeg 2>/dev/null || return 1
    pkg-config --exists libxmp 2>/dev/null || return 1
    pkg-config --exists gmp 2>/dev/null || return 1
    xonotic_has_gmp_headers || return 1
}

xonotic_ensure_gmp_headers() {
    if xonotic_has_gmp_headers; then
        return 0
    fi

    printf 'gmp.h missing — installing libgmp-dev...\n' >&2
    if command -v apt-get >/dev/null 2>&1 && [ "$(id -u)" -eq 0 ]; then
        env DEBIAN_FRONTEND=noninteractive apt-get update -qq
        env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libgmp-dev
    elif command -v apt-get >/dev/null 2>&1; then
        if ! xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq \
            || ! xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libgmp-dev; then
            printf 'Could not install libgmp-dev (no sudo). Install manually:\n' >&2
            printf '  sudo apt install libgmp-dev\n' >&2
            printf 'For cross-builds: also install the target arch libgmp-dev package.\n' >&2
        fi
    fi

    if ! xonotic_has_gmp_headers; then
        printf 'Warning: gmp.h still missing for %s — d0_blind_id build may fail.\n' "$(xonotic_compiler_triplet 2>/dev/null || echo "$(xonotic_compiler)")" >&2
        printf 'Host: sudo apt install libgmp-dev\n' >&2
    fi
}

xonotic_gmp_include_flags() {
    local triplet
    triplet="$(xonotic_compiler_triplet || true)"
    if [ -n "$triplet" ] && [ -f "/usr/include/${triplet}/gmp.h" ]; then
        printf '%s' "-I/usr/include/${triplet}"
        return 0
    fi
    if [ -f /usr/include/gmp.h ]; then
        return 0
    fi
    return 1
}

xonotic_gmp_libs() {
    if [ -n "${PKG_CONFIG:-}" ] && "$PKG_CONFIG" --exists gmp 2>/dev/null; then
        "$PKG_CONFIG" --libs gmp
        return 0
    fi
    if pkg-config --exists gmp 2>/dev/null; then
        pkg-config --libs gmp
        return 0
    fi
    printf '%s' "-lgmp"
}

xonotic_install_native_deps() {
    if xonotic_has_native_build_deps; then
        printf 'Native build dependencies already present.\n'
        return 0
    fi

    if ! command -v apt-get >/dev/null 2>&1; then
        xonotic_usage 'Native dependency install supports Debian/Ubuntu (apt-get) only.' 1
    fi

    printf 'Installing native build dependencies...\n'
    # shellcheck disable=SC2046
    xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    # shellcheck disable=SC2046
    xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        $(xonotic_apt_packages)

    xonotic_has_native_build_deps || xonotic_usage 'Dependency install finished but checks still fail.' 1
}

xonotic_ensure_game_code() {
    local root
    root="$(xonotic_root)"
    if [ -f "$root/engine/darkplaces/makefile.inc" ] \
        && [ -f "$root/engine/data/xonotic-data.pk3dir/qcsrc/Makefile" ]; then
        return 0
    fi
    printf 'Game source incomplete — fetching code...\n'
    bash "$root/scripts/fetch-sources.sh" code
}

xonotic_ensure_game_assets() {
    local root pk3dir
    root="$(xonotic_root)"
    pk3dir="$root/engine/data/xonotic-data.pk3dir"

    # Full game requires the main asset directories AND the additional data packs.
    if [ -d "$pk3dir/gfx" ] && [ -d "$pk3dir/textures" ] && [ -d "$pk3dir/models" ] \
        && [ -d "$root/engine/data/xonotic-maps.pk3dir" ] \
        && [ -d "$root/engine/data/xonotic-music.pk3dir" ]; then
        return 0
    fi
    printf 'Game assets missing — fetching full game data (this only runs once)...\n'
    bash "$root/scripts/fetch-sources.sh" full
}

xonotic_maybe_make_clean() {
    if [ "${XONOTIC_PACKAGE_BUILD:-0}" = "1" ]; then
        return 0
    fi
    make clean >/dev/null 2>&1 || true
}

xonotic_compile_engine_only() {
    local root out_dir out_bin darkplaces
    root="$(xonotic_root)"
    darkplaces="$root/engine/darkplaces"
    out_dir="$root/build/bin"
    out_bin="$out_dir/xonotic"

    if [ ! -f "$darkplaces/makefile.inc" ]; then
        xonotic_usage 'Missing engine/darkplaces — run: ./scripts/fetch-sources.sh code' 1
    fi

    mkdir -p "$out_dir"
    printf 'Building DarkPlaces for %s...\n' "${ARCH:-host}"
    cd "$darkplaces"
    xonotic_maybe_make_clean
    PATH="/usr/bin:${PATH}" make sdl-release DP_SSE=0 "${MAKEFLAGS:--j$(nproc)}"
    install -m 755 darkplaces-sdl "$out_bin"
    printf 'Built %s (%s)\n' "$out_bin" "$(file -b "$out_bin")"
}

xonotic_compile() {
    local root out_dir out_bin gmqcc qcsrc
    root="$(xonotic_root)"
    out_dir="$root/build/bin"
    out_bin="$out_dir/xonotic"
    gmqcc="$root/engine/gmqcc/gmqcc"
    qcsrc="$root/engine/data/xonotic-data.pk3dir/qcsrc/Makefile"

    xonotic_ensure_game_code
    if [ "${XONOTIC_PACKAGE_BUILD:-0}" != "1" ]; then
        xonotic_ensure_game_assets
    fi
    mkdir -p "$out_dir"

    if [ ! -f "$qcsrc" ]; then
        xonotic_compile_engine_only
        return 0
    fi

    printf 'Compiling QuakeC + engine...\n'
    xonotic_apply_cross_compile_env
    xonotic_ensure_gmp_headers
    cd "$root/engine"
    export MAKEFLAGS="${MAKEFLAGS:--j$(nproc)}"
    export QCCFLAGS_WATERMARK="${QCCFLAGS_WATERMARK:-local-dev}"
    export XON_BUILDSYSTEM=1

    cd d0_blind_id
    if [ -n "${ARCH_TRIPLET:-}" ] && [ -f Makefile ]; then
        rm -f Makefile
    fi
    gmp_cflags="$(xonotic_gmp_include_flags || true)"
    gmp_libs="$(xonotic_gmp_libs)"
    d0_cflags="-g -O2"
    d0_cppflags=""
    d0_ldflags=""
    if [ -n "$gmp_cflags" ]; then
        d0_cflags="$d0_cflags $gmp_cflags"
        d0_cppflags="$gmp_cflags"
    fi
    if [ -n "$gmp_libs" ]; then
        d0_ldflags="$gmp_libs"
    fi
    if [ ! -f Makefile ]; then
        sh autogen.sh
        if [ -n "${ARCH_TRIPLET:-}" ]; then
            ./configure --host="${ARCH_TRIPLET}" CPPFLAGS="$d0_cppflags" CFLAGS="$d0_cflags" LDFLAGS="$d0_ldflags" LIBS="$gmp_libs"
        else
            ./configure CPPFLAGS="$d0_cppflags" CFLAGS="$d0_cflags" LDFLAGS="$d0_ldflags" LIBS="$gmp_libs"
        fi
    fi
    make $MAKEFLAGS CPPFLAGS="$d0_cppflags" CFLAGS="$d0_cflags" LDFLAGS="$d0_ldflags" LIBS="$gmp_libs"

    cd "$root/engine/gmqcc"
    # gmqcc must run on the build host to compile QuakeC (arch-neutral output).
    if [ -f gmqcc ] && ! ./gmqcc --version >/dev/null 2>&1; then
        printf 'gmqcc binary incompatible with current environment — rebuilding from source\n'
        xonotic_maybe_make_clean
    fi
    if [ -n "${ARCH_TRIPLET:-}" ]; then
        xonotic_maybe_make_clean
        CC="${HOST_CC:-gcc}" CXX="${HOST_CXX:-g++}" \
            make $MAKEFLAGS STRIP=: gmqcc
    else
        make $MAKEFLAGS STRIP=: gmqcc
    fi

    cd "$root/engine/data/xonotic-data.pk3dir"
    make QCC="$gmqcc" XON_BUILDSYSTEM=1 QCCFLAGS_WATERMARK="$QCCFLAGS_WATERMARK" $MAKEFLAGS qc
    # Ensure menu.dat is rebuilt when menu sources change (make may skip 'all').
    make QCC="$gmqcc" XON_BUILDSYSTEM=1 QCCFLAGS_WATERMARK="$QCCFLAGS_WATERMARK" -C qcsrc ../menu.dat

    cd "$root/engine/darkplaces"
    xonotic_maybe_make_clean
    # Some SDK images ship a broken sdl2-config — ensure /usr/bin/sdl2-config is found first.
    PATH="/usr/bin:${PATH}" make sdl-release DP_SSE=0 $MAKEFLAGS STRIP=:
    install -m 755 darkplaces-sdl "$out_bin"

    printf 'Built %s (%s)\n' "$out_bin" "$(file -b "$out_bin")"
}

xonotic_stage_touch_runtime() {
    local root data_dir startup_cfg
    root="$(xonotic_root)"
    data_dir="$(xonotic_data_dir)"
    startup_cfg="$data_dir/touch/startup.cfg"

    local touch_profile="${XONOTIC_TOUCH_PROFILE:-standard}"
    local touch_perf="${XONOTIC_TOUCH_PERF_PROFILE:-thermal}"
    local user_layout="${XONOTIC_TOUCH_LAYOUT:-${HOME}/.xonotic/data/touch.layout.cfg}"
    local user_layout_legacy="${HOME}/.xonotic/touch.layout.cfg"

    install -m 644 "$root/touch/xonotic.cfg" "$data_dir/xonotic.cfg"
    mkdir -p "$data_dir/touch/profiles"
    cp -a "$root/touch/profiles/." "$data_dir/touch/profiles/"

    {
        echo '// Generated by run-local.sh'
        echo 'developer 0'
        echo 'prvm_traceqc 0'
        echo 'prvm_statementprofiling 0'
        echo 'prvm_timeprofiling 0'
        if [ -f "$data_dir/touch/profiles/${touch_profile}.cfg" ]; then
            echo "exec touch/profiles/${touch_profile}.cfg"
        fi
        if [ -f "$data_dir/touch/profiles/${touch_perf}.cfg" ]; then
            echo "exec touch/profiles/${touch_perf}.cfg"
        fi
        if [ -f "$user_layout" ] || [ -f "$data_dir/touch.layout.cfg" ]; then
            echo "exec touch.layout.cfg"
        elif [ -f "$user_layout_legacy" ]; then
            echo "exec ${user_layout_legacy}"
        fi
    } > "$startup_cfg"
}

xonotic_write_screen_layout() {
    local root data_dir layout_cfg screen_calc
    root="$(xonotic_root)"
    data_dir="$(xonotic_data_dir)"
    layout_cfg="$data_dir/screen.layout.cfg"
    screen_calc="$root/touch/screen-calc.sh"

    if [ -f "$screen_calc" ]; then
        # shellcheck source=/dev/null
        . "$screen_calc"
        xonotic_screen_calc "$layout_cfg"
    else
        cat > "$layout_cfg" <<EOF
vid_width ${XONOTIC_DEFAULT_WIDTH:-1280}
vid_height ${XONOTIC_DEFAULT_HEIGHT:-720}
vid_touchscreen_xdpi ${XONOTIC_TOUCH_XDPI:-320}
vid_touchscreen_ydpi ${XONOTIC_TOUCH_YDPI:-320}
vid_touchscreen_density ${XONOTIC_TOUCH_DENSITY:-2.0}
EOF
    fi
}

xonotic_preflight_run() {
    local root data_dir
    root="$(xonotic_root)"
    data_dir="$(xonotic_data_dir)"

    if [ ! -x "$(xonotic_bin)" ]; then
        xonotic_usage "Missing $(xonotic_bin) — run: ./scripts/compile-and-install-deps.sh" 1
    fi
    if [ ! -d "$data_dir/xonotic-data.pk3dir/qcsrc" ]; then
        xonotic_usage "Missing game data — run: ./scripts/fetch-sources.sh code" 1
    fi
    if [ ! -f "$data_dir/xonotic-data.pk3dir/quake.rc" ]; then
        xonotic_usage 'Missing quake.rc — run: ./scripts/fetch-sources.sh code' 1
    fi
    if [ ! -f "$data_dir/xonotic-data.pk3dir/gfx/mainmenu.tga" ] \
        && [ ! -f "$data_dir/xonotic-data.pk3dir/gfx/mainmenu.png" ]; then
        printf 'Note: menu gfx/maps may be missing. For a playable build run:\n' >&2
        printf '  ./scripts/fetch-sources.sh assets\n' >&2
    fi
}

# Sets XONOTIC_TOUCH_ASSET_FETCH_ACTIVE / XONOTIC_TOUCH_ASSETS_READY and may
# start a background fetch. Must run in the launcher shell (not a subshell) so
# XONOTIC_ASSET_FETCH_PID stays owned for shutdown-on-exit.
xonotic_touch_begin_asset_fetch() {
    local data_dir asset_lib discover_lib progress_file needle pid cmdline
    data_dir="$(xonotic_data_dir)"
    asset_lib="$(xonotic_root)/scripts/lib/asset-fetch.sh"
    discover_lib="$(xonotic_root)/scripts/lib/asset-discover.sh"
    # Plain name under touch/: DarkPlaces rejects dot-prefixed paths, so a
    # progress file the engine cannot open would leave the wizard blank.
    progress_file="$data_dir/touch/asset-progress.txt"
    mkdir -p "$data_dir/touch"

    XONOTIC_TOUCH_ASSET_FETCH_ACTIVE=0
    XONOTIC_TOUCH_ASSETS_READY=0

    if [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" = "1" ] || [ ! -f "$asset_lib" ]; then
        return 0
    fi

    # shellcheck source=/dev/null
    . "$asset_lib"
    if [ -f "$discover_lib" ]; then
        # shellcheck source=/dev/null
        . "$discover_lib"
    fi
    export XONOTIC_ASSET_FETCH_PROGRESS="$progress_file"

    # Fast sync only: ready? → no wizard. Otherwise fullscreen wizard + background.
    if xonotic_assets_are_ready "$data_dir"; then
        XONOTIC_TOUCH_ASSETS_READY=1
        return 0
    fi

    XONOTIC_TOUCH_ASSET_FETCH_ACTIVE=1

    # Avoid stacking fetch jobs when the engine relaunches after setup.
    if [ "${XONOTIC_ASSET_FETCH_JOB_STARTED:-0}" = "1" ]; then
        return 0
    fi
    XONOTIC_ASSET_FETCH_JOB_STARTED=1

    {
        printf '%s\n' discover
        printf '%s\n' 5
        printf '%s\n' "Checking your game data..."
    } > "$progress_file"

    (
        needle="$data_dir/.fetch-tmp/"
        trap 'for pid in $(pgrep -x curl 2>/dev/null || true) $(pgrep -x wget 2>/dev/null || true); do
            cmdline="$(tr "\0" " " < "/proc/$pid/cmdline" 2>/dev/null || true)"
            case "$cmdline" in *"$needle"*) kill "$pid" 2>/dev/null || true ;; esac
        done; exit 143' TERM INT
        if command -v flock >/dev/null 2>&1; then
            flock -n 9 || exit 0
        fi
        # Drop leftover writers from previous launcher copies.
        for pid in $(pgrep -x curl 2>/dev/null || true) $(pgrep -x wget 2>/dev/null || true); do
            cmdline="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
            case "$cmdline" in
                *"$needle"*) kill "$pid" 2>/dev/null || true ;;
            esac
        done
        if declare -F xonotic_resolve_missing_assets >/dev/null 2>&1; then
            xonotic_resolve_missing_assets "$data_dir"
        else
            xonotic_fetch_game_assets "$data_dir"
        fi
    ) 9>"$data_dir/touch/fetch.lock" &
    XONOTIC_ASSET_FETCH_PID=$!
}

xonotic_touch_stop_asset_fetch() {
    local data_dir progress_file pid child pct
    data_dir="$(xonotic_data_dir)"
    progress_file="$data_dir/touch/asset-progress.txt"
    pid="${XONOTIC_ASSET_FETCH_PID:-}"
    XONOTIC_ASSET_FETCH_PID=""

    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        for child in $(pgrep -P "$pid" 2>/dev/null || true); do
            kill "$child" 2>/dev/null || true
        done
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi

    # Belt-and-braces: any remaining writers into this fetch-tmp.
    local needle="$data_dir/.fetch-tmp/" cmdline
    for pid in $(pgrep -x curl 2>/dev/null || true) $(pgrep -x wget 2>/dev/null || true); do
        cmdline="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
        case "$cmdline" in
            *"$needle"*) kill "$pid" 2>/dev/null || true ;;
        esac
    done

    if [ ! -f "$data_dir/.assets-ready" ]; then
        pct="$(sed -n '2p' "$progress_file" 2>/dev/null || echo 0)"
        case "$pct" in
            ''|*[!0-9]*) pct=0 ;;
        esac
        {
            printf '%s\n' paused
            printf '%s\n' "$pct"
            printf '%s\n' "Download paused. Reopen Xonotic Touch to continue."
        } > "${progress_file}.tmp.$$" \
            && mv -f "${progress_file}.tmp.$$" "$progress_file"
    fi
}

xonotic_run_native() {
    local root base_dir bin touch_profile touch_perf fullscreen
    root="$(xonotic_root)"
    base_dir="$root/engine"
    bin="$(xonotic_bin)"
    touch_profile="${XONOTIC_TOUCH_PROFILE:-standard}"
    touch_perf="${XONOTIC_TOUCH_PERF_PROFILE:-thermal}"
    fullscreen="${XONOTIC_FULLSCREEN:-0}"

    xonotic_preflight_run
    xonotic_stage_touch_runtime
    xonotic_write_screen_layout

    XONOTIC_ASSET_FETCH_PID=""
    xonotic_touch_begin_asset_fetch

    # Mirrors packaging/start.sh: the download wizard asks for a relaunch so the
    # engine picks up packs that appeared after it built its search path.
    local restart_marker restart_marker_home status
    restart_marker="$(xonotic_data_dir)/touch/relaunch-request.txt"
    restart_marker_home="${HOME}/.xonotic/data/touch/relaunch-request.txt"
    rm -f "$restart_marker" "$restart_marker_home"
    status=0

    cd "$base_dir"

    printf 'Launching with basedir %s (gamedir data/)\n' "$base_dir"
    printf '  binary:  %s\n' "$bin"
    printf '  profile: %s (+ %s)\n' "$touch_profile" "$touch_perf"
    printf '  screen:  %sx%s (mouse = touch on desktop)\n' \
        "${XONOTIC_VID_WIDTH:-?}" "${XONOTIC_VID_HEIGHT:-?}"
    printf '\n'

    # Download stops with the app; partial zips resume on the next launch.
    trap 'xonotic_touch_stop_asset_fetch' EXIT
    trap 'xonotic_touch_stop_asset_fetch; exit 130' INT
    trap 'xonotic_touch_stop_asset_fetch; exit 143' TERM

    while :; do
        "$bin" -xonotic \
            -customgamename "Xonotic Touch" \
            +exec xonotic.cfg \
            +exec screen.layout.cfg \
            +exec config.cfg \
            +exec autoexec.cfg \
            +exec touch/startup.cfg \
            +set _touch_asset_fetch_active "${XONOTIC_TOUCH_ASSET_FETCH_ACTIVE:-0}" \
            +set _touch_assets_ready "${XONOTIC_TOUCH_ASSETS_READY:-0}" \
            +vid_fullscreen "$fullscreen" \
            +vid_touchscreen 1 \
            +cl_movement 1 \
            +con_closeontoggle 1 \
            +developer 0 \
            "$@" || status=$?

        if [ ! -f "$restart_marker" ] && [ ! -f "$restart_marker_home" ]; then
            break
        fi
        rm -f "$restart_marker" "$restart_marker_home"
        printf 'Relaunching engine so downloaded game data is loaded\n'
        xonotic_touch_begin_asset_fetch
        status=0
    done

    trap - EXIT INT TERM
    xonotic_touch_stop_asset_fetch
    return "$status"
}
