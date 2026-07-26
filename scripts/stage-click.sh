#!/bin/bash
# Stage Ubuntu Touch click package tree (slim data; assets on first launch).
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
# shellcheck source=lib/xonotic-shlib.sh
. "$ROOT/scripts/lib/xonotic-shlib.sh"

DEST="${DEST:-${DESTDIR:-$ROOT/build/click}}"
CLICK_NAME="${CLICK_NAME:-xonotictouch}"
CLICK_VERSION="${CLICK_VERSION:-1.1.1}"
CLICK_FRAMEWORK="${CLICK_FRAMEWORK:-ubuntu-touch-24.04-1.x}"
CLICK_ARCH="${CLICK_ARCH:-}"

resolve_click_arch() {
    if [ -n "$CLICK_ARCH" ]; then
        printf '%s' "$CLICK_ARCH"
        return 0
    fi
    case "${ARCH:-}" in
        arm64|aarch64) printf 'arm64'; return 0 ;;
        armhf|armv7l) printf 'armhf'; return 0 ;;
        amd64|x86_64) printf 'amd64'; return 0 ;;
    esac
    if [ -n "${ARCH_TRIPLET:-}" ]; then
        case "$ARCH_TRIPLET" in
            aarch64-*) printf 'arm64'; return 0 ;;
            arm-linux-gnueabihf|armv7*) printf 'armhf'; return 0 ;;
            x86_64-*|amd64*) printf 'amd64'; return 0 ;;
        esac
    fi
    local machine
    machine="$(uname -m)"
    case "$machine" in
        aarch64|arm64) printf 'arm64' ;;
        armv7*|armhf) printf 'armhf' ;;
        x86_64|amd64) printf 'amd64' ;;
        *)
            printf 'Unsupported architecture for click: %s\n' "$machine" >&2
            exit 1
            ;;
    esac
}

copy_shared_libs() {
    local binary="$1"
    local lib_dir="$2"
    local lib base dest
    local list seen_file
    list="$(mktemp)"
    seen_file="$(mktemp)"
    printf '%s\n' "$binary" > "$list"

    while [ -s "$list" ]; do
        local current
        current="$(head -n1 "$list")"
        sed -i '1d' "$list"
        grep -Fxq "$current" "$seen_file" 2>/dev/null && continue
        printf '%s\n' "$current" >> "$seen_file"

        while IFS= read -r lib; do
            case "$lib" in
                ''|linux-vdso.so.*) continue ;;
            esac
            [ -f "$lib" ] || continue
            base="$(basename "$lib")"
            case "$base" in
                ld-linux*.so*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libgcc_s.so*|libstdc++.so*)
                    continue
                    ;;
            esac
            dest="$lib_dir/$base"
            if [ ! -f "$dest" ]; then
                install -m 755 "$lib" "$dest"
            fi
            if ! grep -Fxq "$lib" "$seen_file" 2>/dev/null; then
                printf '%s\n' "$lib" >> "$list"
            fi
        done < <(ldd "$current" 2>/dev/null | awk '/=> \// {print $3}')
    done

    rm -f "$list" "$seen_file"
}

CLICK_ARCH="$(resolve_click_arch)"
BIN="$ROOT/build/bin/xonotic"
test -x "$BIN" || {
    echo "Missing build/bin/xonotic — run: ARCH=${ARCH:-} ./scripts/build.sh" >&2
    exit 1
}

rm -rf "$DEST"
mkdir -p "$DEST/bin" "$DEST/lib" "$DEST/data" "$DEST/share/xonotic"

install -m 755 "$BIN" "$DEST/bin/xonotic"
install -m 755 "$ROOT/packaging/start.sh" "$DEST/bin/start.sh"
install -m 755 "$ROOT/touch/screen-calc.sh" "$DEST/share/xonotic/screen-calc.sh"
install -m 755 "$ROOT/scripts/fetch-assets-runtime.sh" "$DEST/share/xonotic/fetch-assets-runtime.sh"
install -m 755 "$ROOT/scripts/sync-bundle-data.sh" "$DEST/share/xonotic/sync-bundle-data.sh"
install -m 644 "$ROOT/scripts/lib/asset-fetch.sh" "$DEST/share/xonotic/asset-fetch.sh"
install -m 644 "$ROOT/scripts/lib/asset-discover.sh" "$DEST/share/xonotic/asset-discover.sh"

bash "$ROOT/scripts/stage-slim-data.sh" "$DEST/data"
bash "$ROOT/scripts/stage-click-utils.sh" "$DEST"
copy_shared_libs "$DEST/bin/xonotic" "$DEST/lib"

# Official Xonotic icon — OpenStore extracts this from the click on revision upload.
ICON_SRC="$ROOT/engine/misc/logos/icons_png/xonotic_256.png"
test -f "$ICON_SRC" || {
    echo "Missing official icon: $ICON_SRC" >&2
    exit 1
}
install -m 644 "$ICON_SRC" "$DEST/xonotic.png"

install -m 644 "$ROOT/click/xonotic.desktop" "$DEST/xonotic.desktop"
if [ -f "$ROOT/click/xonotic.apparmor" ]; then
    # Prefer clickable ENV substitution when present; otherwise keep concrete policy.
    if [ -n "${APPARMOR_POLICY:-}" ]; then
        sed "s/\"policy_version\": \"[^\"]*\"/\"policy_version\": \"${APPARMOR_POLICY}\"/" \
            "$ROOT/click/xonotic.apparmor" > "$DEST/xonotic.apparmor"
    else
        install -m 644 "$ROOT/click/xonotic.apparmor" "$DEST/xonotic.apparmor"
    fi
fi

sed \
    -e "s|@CLICK_NAME@|${CLICK_NAME}|g" \
    -e "s|@CLICK_VERSION@|${CLICK_VERSION}|g" \
    -e "s|@CLICK_ARCH@|${CLICK_ARCH}|g" \
    -e "s|@CLICK_FRAMEWORK@|${CLICK_FRAMEWORK}|g" \
    "$ROOT/click/manifest.json.in" > "$DEST/manifest.json"

echo "Staged click tree to $DEST (arch=${CLICK_ARCH}, version=${CLICK_VERSION})"
