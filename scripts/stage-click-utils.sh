#!/bin/bash
# Copy curl/unzip (and shared libs) into a click package for confined downloads.
set -euo pipefail

DEST="${1:?usage: stage-click-utils.sh <click-dest>}"
BIN_DIR="$DEST/bin"
LIB_DIR="$DEST/lib"

mkdir -p "$BIN_DIR" "$LIB_DIR"

copy_binary_with_libs() {
    local binary="$1"
    local dest_name="${2:-$(basename "$binary")}"

    if [ ! -x "$binary" ]; then
        return 1
    fi

    install -m 755 "$binary" "$BIN_DIR/$dest_name"

    local lib
    while IFS= read -r lib; do
        case "$lib" in
            linux-vdso.so.*|not\ a\ dynamic\ executable|'')
                continue
            esac
        if [ -f "$lib" ]; then
            install -m 755 "$lib" "$LIB_DIR/"
        fi
    done < <(ldd "$binary" | awk '/=> \/.*\// {print $3}')
}

for util in curl unzip; do
    if command -v "$util" >/dev/null 2>&1; then
        copy_binary_with_libs "$(command -v "$util")" || true
    fi
done
