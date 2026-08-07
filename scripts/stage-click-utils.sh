#!/bin/bash
# Stage the userspace utilities a confined click app needs at runtime.
#
# Ubuntu Touch runs click apps under AppArmor, which denies exec of host
# binaries outside the click tree — even /usr/bin/dirname. Everything
# packaging/start.sh and the asset helpers call must therefore ship inside the
# package (docs/UBUNTU_TOUCH_LAUNCH.md).
set -euo pipefail

DEST="${1:?usage: stage-click-utils.sh <click-dest>}"
BIN_DIR="$DEST/bin"
LIB_DIR="$DEST/lib"

# Applets the launcher, screen calc, bundle sync and asset helpers rely on.
BUSYBOX_APPLETS=(
    awk basename cat cp cut date dirname du env expr find grep head install
    ln ls mkdir mktemp mv printf readlink rm rmdir sed sleep sort ssl_client
    stat tail tar touch tr unzip wget which xargs
)

mkdir -p "$BIN_DIR" "$LIB_DIR"

target_arch() {
    local arch="${CLICK_ARCH:-${ARCH:-}}"
    if [ -z "$arch" ]; then
        arch="$(uname -m)"
    fi
    case "$arch" in
        arm64|aarch64) printf 'arm64' ;;
        armhf|armv7l|armv7|arm) printf 'armhf' ;;
        amd64|x86_64) printf 'amd64' ;;
        *)
            printf 'stage-click-utils: unsupported arch %s\n' "$arch" >&2
            return 1
            ;;
    esac
}

# `file` output fragment that identifies each Debian architecture.
arch_elf_signature() {
    case "$1" in
        arm64) printf 'ARM aarch64' ;;
        armhf) printf 'ARM, EABI' ;;
        amd64) printf 'x86-64' ;;
    esac
}

binary_matches_arch() {
    local binary="$1"
    local arch="$2"
    local signature
    signature="$(arch_elf_signature "$arch")"

    if ! command -v file >/dev/null 2>&1; then
        # Without `file` we cannot tell a cross-built binary from a host one.
        return 1
    fi
    file -bL "$binary" 2>/dev/null | grep -qF "$signature"
}

copy_binary_with_libs() {
    local binary="$1"
    local dest_name="${2:-$(basename "$binary")}"

    # The name may already be a busybox applet link; never install through it.
    rm -f "$BIN_DIR/$dest_name"
    install -m 755 "$binary" "$BIN_DIR/$dest_name"

    local lib
    while IFS= read -r lib; do
        if [ -f "$lib" ]; then
            install -m 755 "$lib" "$LIB_DIR/"
        fi
    done < <(ldd "$binary" 2>/dev/null | awk '/=> \/.*\// {print $3}')
}

BUSYBOX_TMP=""
cleanup() {
    if [ -n "$BUSYBOX_TMP" ]; then
        rm -rf "$BUSYBOX_TMP"
    fi
}
trap cleanup EXIT

# Prefer a target-arch busybox from the archive; fall back to the host binary
# only for native builds.
download_busybox() {
    local arch="$1"
    local deb package candidate
    local updated=0
    for package in "busybox-static:${arch}" "busybox:${arch}"; do
        if ! ( cd "$BUSYBOX_TMP" && apt-get download "$package" >/dev/null 2>&1 ); then
            # Missing foreign arch or stale lists are the usual causes.
            if [ "$updated" -eq 0 ]; then
                updated=1
                dpkg --add-architecture "$arch" >/dev/null 2>&1 || true
                apt-get update >/dev/null 2>&1 || true
                ( cd "$BUSYBOX_TMP" && apt-get download "$package" >/dev/null 2>&1 ) || continue
            else
                continue
            fi
        fi
        deb="$(find "$BUSYBOX_TMP" -maxdepth 1 -name '*.deb' -print -quit)"
        if [ -z "$deb" ] || ! dpkg-deb -x "$deb" "$BUSYBOX_TMP/root" 2>/dev/null; then
            continue
        fi
        for candidate in "$BUSYBOX_TMP/root/bin/busybox" "$BUSYBOX_TMP/root/usr/bin/busybox"; do
            if [ -f "$candidate" ]; then
                printf '%s' "$candidate"
                return 0
            fi
        done
    done

    return 1
}

stage_busybox() {
    local arch="$1"
    local busybox=""
    local candidate

    # Native builds can use the host binary; cross builds must pull the archive.
    for candidate in /bin/busybox /usr/bin/busybox; do
        if [ -x "$candidate" ] && binary_matches_arch "$candidate" "$arch"; then
            busybox="$candidate"
            break
        fi
    done

    if [ -z "$busybox" ]; then
        # Created here so the EXIT trap can clean it up: download_busybox runs in
        # a command substitution subshell and cannot export the path back.
        BUSYBOX_TMP="$(mktemp -d)"
        candidate="$(download_busybox "$arch" || true)"
        if [ -n "$candidate" ] && binary_matches_arch "$candidate" "$arch"; then
            busybox="$candidate"
        fi
    fi

    if [ -z "$busybox" ]; then
        return 1
    fi

    install -m 755 "$busybox" "$BIN_DIR/busybox"
    local applet
    for applet in "${BUSYBOX_APPLETS[@]}"; do
        ln -sfn busybox "$BIN_DIR/$applet"
    done
    printf 'stage-click-utils: staged busybox (%s) with %d applets\n' \
        "$arch" "${#BUSYBOX_APPLETS[@]}"
}

ARCH_NAME="$(target_arch)"

if ! stage_busybox "$ARCH_NAME"; then
    if [ "${XONOTIC_ALLOW_MISSING_BUSYBOX:-0}" = "1" ]; then
        printf 'stage-click-utils: WARNING no %s busybox staged — the app cannot launch confined\n' \
            "$ARCH_NAME" >&2
    else
        printf 'stage-click-utils: no %s busybox available (apt-get download busybox-static:%s failed)\n' \
            "$ARCH_NAME" "$ARCH_NAME" >&2
        printf 'stage-click-utils: set XONOTIC_ALLOW_MISSING_BUSYBOX=1 to package anyway\n' >&2
        exit 1
    fi
fi

# curl/unzip are nicer than the busybox applets for large downloads, but only a
# target-arch build is usable on the device.
for util in curl unzip; do
    util_path="$(command -v "$util" 2>/dev/null || true)"
    if [ -z "$util_path" ]; then
        continue
    fi
    if binary_matches_arch "$util_path" "$ARCH_NAME"; then
        copy_binary_with_libs "$util_path"
    else
        printf 'stage-click-utils: skipping host %s (not %s) — busybox applet is used instead\n' \
            "$util" "$ARCH_NAME"
    fi
done
