#!/bin/bash
# Runs a command inside the GNOME SDK (headers and libraries the Flatpak runs on),
# with this user's rustup toolchain. Used by build.sh and for local development.
set -euo pipefail
SDK="${XT_GNOME_SDK:-org.gnome.Sdk//50}"
exec flatpak run --filesystem=home --share=network --env=PATH="$HOME/.cargo/bin:/usr/bin:/bin" \
    --env=CARGO_HOME="$HOME/.cargo" --env=RUSTUP_HOME="$HOME/.rustup" \
    --command=bash "$SDK" -c "cd '$PWD' && $*"
