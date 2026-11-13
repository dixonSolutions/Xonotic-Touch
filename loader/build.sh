#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the Linux app (godnotic-loader) against the GNOME runtime the Flatpak
# runs on, inside that SDK, with this user's rustup toolchain.
# Output: loader/dist/godnotic-loader-<arch>
#
#   flatpak install --user flathub org.gnome.Sdk//48
#   loader/build.sh            # release build for this machine's arch
#   loader/build.sh --check    # also clippy -D warnings and the core tests
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
GNOME="${XT_GNOME_VERSION:-48}"
SDK=""
for b in "$GNOME" 50 49 48; do
    if flatpak info --user "org.gnome.Sdk//$b" >/dev/null 2>&1 || flatpak info "org.gnome.Sdk//$b" >/dev/null 2>&1; then SDK="org.gnome.Sdk//$b"; break; fi
done
[ -n "$SDK" ] || { echo "No org.gnome.Sdk installed (flatpak install --user flathub org.gnome.Sdk//$GNOME)" >&2; exit 1; }
echo "building in $SDK (the Flatpak runs on GNOME $GNOME; features pin GTK 4.18 / libadwaita 1.7)"
export XT_GNOME_SDK="$SDK"
./build-in-sdk.sh "cargo build --release --locked -p godnotic-loader"
if [ "${1:-}" = "--check" ]; then
    ./build-in-sdk.sh "cargo clippy --release --locked -p godnotic-loader -- -D warnings"
    cargo clippy --locked -p godnotic-loader-core --features http-ureq --all-targets -- -D warnings
    cargo test --locked -p godnotic-loader-core --features http-ureq
fi
ARCH="$(uname -m)"
mkdir -p dist
install -m 755 target/release/godnotic-loader "dist/godnotic-loader-$ARCH"
ls -l dist
