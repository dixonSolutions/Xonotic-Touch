#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the launcher for x86_64 and, when an aarch64 GNOME runtime is
# installed, aarch64 too (Kotlin/Native cross-compiles; it has no aarch64
# host). Output: launcher/dist/xt-launcher-<arch>
#
# Headers and libraries come from Flatpak GNOME runtimes, the same ones the
# Flatpak runs on (runtime-version in flatpak/*.yml). Override with
# XT_GTK_INCLUDE (an SDK files/ dir), XT_GTK_LIB_X64, XT_GTK_LIB_ARM64.
#
#   flatpak install --user flathub org.gnome.Sdk//48
#   flatpak install --user --arch=aarch64 flathub org.gnome.Platform//48   # optional
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
GNOME="${XT_GNOME_VERSION:-48}"

runtime_files() { # <id> <arch> <branch>
    local base
    for base in "${FLATPAK_USER_DIR:-$HOME/.local/share/flatpak}" /var/lib/flatpak; do
        if [ -d "$base/runtime/$1/$2/$3/active/files" ]; then
            echo "$base/runtime/$1/$2/$3/active/files"
            return 0
        fi
    done
    return 1
}

any_sdk() {
    local b
    for b in "$GNOME" 50 49 51 48 47; do runtime_files org.gnome.Sdk x86_64 "$b" && return 0; done
    return 1
}

INC="${XT_GTK_INCLUDE:-$(any_sdk || true)}"
[ -n "$INC" ] && [ -f "$INC/include/libadwaita-1/adwaita.h" ] || {
    echo "No GNOME SDK headers found; install org.gnome.Sdk//$GNOME or set XT_GTK_INCLUDE" >&2
    exit 1
}
LIBX="${XT_GTK_LIB_X64:-}"
if [ -z "$LIBX" ]; then
    d="$(runtime_files org.gnome.Platform x86_64 "$GNOME" || runtime_files org.gnome.Sdk x86_64 "$GNOME" || true)"
    [ -n "$d" ] && LIBX="$d/lib/x86_64-linux-gnu" || LIBX="$INC/lib/x86_64-linux-gnu"
fi
LIBA="${XT_GTK_LIB_ARM64:-}"
if [ -z "$LIBA" ]; then
    d="$(runtime_files org.gnome.Platform aarch64 "$GNOME" || runtime_files org.gnome.Sdk aarch64 "$GNOME" || true)"
    [ -n "$d" ] && LIBA="$d/lib/aarch64-linux-gnu"
fi

echo "headers: $INC"
echo "x86_64 libraries: $LIBX"
echo "aarch64 libraries: ${LIBA:-none (skipping aarch64)}"

props=(-Pxt.gtk.include="$INC" -Pxt.gtk.lib.x64="$LIBX")
tasks=(:app:linkReleaseExecutableLinuxX64)
if [ -n "$LIBA" ]; then
    props+=(-Pxt.gtk.lib.arm64="$LIBA")
    tasks+=(:app:linkReleaseExecutableLinuxArm64)
fi

cd "$HERE"
./gradlew --no-daemon --console=plain "${props[@]}" "${tasks[@]}" "$@"

mkdir -p dist
install -m 755 app/build/bin/linuxX64/releaseExecutable/xt-launcher.kexe dist/xt-launcher-x86_64
if [ -n "$LIBA" ]; then
    install -m 755 app/build/bin/linuxArm64/releaseExecutable/xt-launcher.kexe dist/xt-launcher-aarch64
fi
ls -l dist
