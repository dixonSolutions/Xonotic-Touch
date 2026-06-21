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
    if [ -z "$triplet" ] && [ "${ARCH:-}" = "arm64" ]; then
        triplet=aarch64-linux-gnu
    fi
    if [ -n "$triplet" ] && command -v "${triplet}-gcc" >/dev/null 2>&1; then
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
            printf 'For cross-builds (Clickable arm64): also install the target arch package.\n' >&2
        fi
    fi

    if ! xonotic_has_gmp_headers; then
        printf 'Warning: gmp.h still missing for %s — d0_blind_id build may fail.\n' "$(xonotic_compiler_triplet 2>/dev/null || echo "$(xonotic_compiler)")" >&2
        printf 'Clickable: run "clickable clean" once to refresh the SDK image, then rebuild.\n' >&2
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

xonotic_has_clickable() {
    command -v clickable >/dev/null 2>&1
}

xonotic_install_clickable_cli() {
    if xonotic_has_clickable; then
        printf 'Clickable already installed: %s\n' "$(command -v clickable)"
        return 0
    fi

    printf 'Installing Clickable CLI...\n'
    if command -v pipx >/dev/null 2>&1; then
        pipx install clickable
    elif command -v pip3 >/dev/null 2>&1; then
        pip3 install --user clickable
        export PATH="${HOME}/.local/bin:${PATH}"
    elif command -v apt-get >/dev/null 2>&1; then
        xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq
        xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq pipx python3-pip
        pipx ensurepath 2>/dev/null || true
        pipx install clickable
    else
        xonotic_usage 'Install Clickable manually: pipx install clickable' 1
    fi

    xonotic_has_clickable || xonotic_usage 'Clickable install failed.' 1
}

xonotic_has_container_runtime() {
    command -v docker >/dev/null 2>&1 || command -v podman >/dev/null 2>&1
}

xonotic_install_container_runtime() {
    if xonotic_has_container_runtime; then
        printf 'Container runtime already present.\n'
        return 0
    fi

    if ! command -v apt-get >/dev/null 2>&1; then
        xonotic_usage 'Install Docker or Podman manually for Clickable container builds.' 1
    fi

    printf 'Installing Podman for Clickable container builds...\n'
    xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    xonotic_maybe_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        podman podman-docker fuse-overlayfs slirp4netns

    xonotic_has_container_runtime || xonotic_usage 'Container runtime install failed.' 1
}

xonotic_clean_container_artifacts() {
    printf 'Cleaning Clickable/Podman build artifacts...\n'
    buildah rm -a 2>/dev/null || true
    podman container prune -f 2>/dev/null || true
    podman image prune -f 2>/dev/null || true
    if [ -d "${HOME}/.local/share/containers/storage" ]; then
        printf 'Podman storage: %s\n' "$(du -sh "${HOME}/.local/share/containers/storage" | cut -f1)"
    fi
}

xonotic_clickable_setup_container() {
    xonotic_install_clickable_cli
    xonotic_install_container_runtime
    if xonotic_has_clickable; then
        clickable setup docker 2>/dev/null || clickable setup 2>/dev/null || true
    fi
}

xonotic_clickable_setup_desktop() {
    xonotic_install_clickable_cli
    xonotic_install_native_deps
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

xonotic_clickable_build_args() {
    local mode="$1"
    local arch="${XONOTIC_CLICKABLE_ARCH:-}"
    case "$mode" in
        desktop)
            arch="${arch:-amd64}"
            printf '%s\n' --container-mode --arch "$arch"
            ;;
        container)
            arch="${arch:-arm64}"
            printf '%s\n' --arch "$arch"
            ;;
        *)
            xonotic_usage "Unknown Clickable mode: $mode" 1
            ;;
    esac
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
    make clean >/dev/null 2>&1 || true
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
    make $MAKEFLAGS clean >/dev/null 2>&1 || true
    make $MAKEFLAGS CPPFLAGS="$d0_cppflags" CFLAGS="$d0_cflags" LDFLAGS="$d0_ldflags" LIBS="$gmp_libs"

    cd "$root/engine/gmqcc"
    # If a pre-built gmqcc exists but won't run on this environment (e.g. GLIBC
    # mismatch when building inside a Clickable SDK container), clean it so make
    # recompiles from source rather than skipping the target.
    if [ -f gmqcc ] && ! ./gmqcc --version >/dev/null 2>&1; then
        printf 'gmqcc binary incompatible with current environment — rebuilding from source\n'
        make clean >/dev/null 2>&1 || true
    fi
    make $MAKEFLAGS gmqcc

    cd "$root/engine/data/xonotic-data.pk3dir"
    make QCC="$gmqcc" XON_BUILDSYSTEM=1 QCCFLAGS_WATERMARK="$QCCFLAGS_WATERMARK" $MAKEFLAGS
    # Ensure menu.dat is rebuilt when menu sources change (make may skip 'all').
    make QCC="$gmqcc" XON_BUILDSYSTEM=1 QCCFLAGS_WATERMARK="$QCCFLAGS_WATERMARK" -C qcsrc ../menu.dat

    cd "$root/engine/darkplaces"
    make clean >/dev/null 2>&1 || true
    # The Clickable SDK places a broken sdl2-config at /usr/local/bin that has
    # prefix=/. — ensure the real /usr/bin/sdl2-config is found first.
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
    local touch_perf="${XONOTIC_TOUCH_PERF_PROFILE:-balanced}"
    local user_layout="${XONOTIC_TOUCH_LAYOUT:-${HOME}/.xonotic/touch.layout.cfg}"

    install -m 644 "$root/touch/xonotic.cfg" "$data_dir/xonotic.cfg"
    mkdir -p "$data_dir/touch/profiles"
    cp -a "$root/touch/profiles/." "$data_dir/touch/profiles/"

    {
        echo '// Generated by run-local-no-clickable.sh'
        if [ -f "$data_dir/touch/profiles/${touch_profile}.cfg" ]; then
            echo "exec touch/profiles/${touch_profile}.cfg"
        fi
        if [ -f "$data_dir/touch/profiles/${touch_perf}.cfg" ]; then
            echo "exec touch/profiles/${touch_perf}.cfg"
        fi
        if [ -f "$user_layout" ]; then
            echo "exec ${user_layout}"
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

xonotic_run_native() {
    local root base_dir bin touch_profile touch_perf fullscreen
    root="$(xonotic_root)"
    base_dir="$root/engine"
    bin="$(xonotic_bin)"
    touch_profile="${XONOTIC_TOUCH_PROFILE:-standard}"
    touch_perf="${XONOTIC_TOUCH_PERF_PROFILE:-balanced}"
    fullscreen="${XONOTIC_FULLSCREEN:-0}"

    xonotic_preflight_run
    xonotic_stage_touch_runtime
    xonotic_write_screen_layout

    cd "$base_dir"

    printf 'Launching with basedir %s (gamedir data/)\n' "$base_dir"
    printf '  binary:  %s\n' "$bin"
    printf '  profile: %s (+ %s)\n' "$touch_profile" "$touch_perf"
    printf '  screen:  %sx%s (mouse = touch on desktop)\n' \
        "${XONOTIC_VID_WIDTH:-?}" "${XONOTIC_VID_HEIGHT:-?}"
    printf '\n'

    exec "$bin" -xonotic \
        +exec xonotic.cfg \
        +exec screen.layout.cfg \
        +vid_fullscreen "$fullscreen" \
        +vid_touchscreen 1 \
        +cl_movement 1 \
        +con_closeontoggle 1 \
        "$@"
}

xonotic_stage_click_build() {
    local root
    root="$(xonotic_root)"

    test -x "$(xonotic_bin)" || xonotic_usage 'Build failed: build/bin/xonotic missing' 1
    test -f "$root/packaging/start.sh" || xonotic_usage 'Missing packaging/start.sh' 1
    test -f "$root/touch/xonotic.cfg" || xonotic_usage 'Missing touch/xonotic.cfg' 1
    test -f "$root/touch/screen-calc.sh" || xonotic_usage 'Missing touch/screen-calc.sh' 1
    test -f "$root/touch/profiles/standard.cfg" || xonotic_usage 'Missing touch/profiles/standard.cfg' 1

    mkdir -p "$root/data"
    cp -f "$root/touch/xonotic.cfg" "$root/data/xonotic.cfg"
    printf 'Click package build ready for %s\n' "${ARCH:-unknown}"
}

xonotic_ensure_clickable_ready() {
    local mode="$1"
    local root
    root="$(xonotic_root)"

    if ! xonotic_has_clickable; then
        printf 'Clickable not found — running install-clickable.sh --%s\n' "$mode"
        bash "$root/scripts/install-clickable.sh" "--$mode"
    fi
    if [ "$mode" = container ] && ! xonotic_has_container_runtime; then
        printf 'Container runtime missing — running install-clickable.sh --container\n'
        bash "$root/scripts/install-clickable.sh" --container
    fi
    if [ "$mode" = desktop ] && ! xonotic_has_native_build_deps; then
        printf 'Native build deps missing — running install-clickable.sh --desktop\n'
        bash "$root/scripts/install-clickable.sh" --desktop
    fi
}
