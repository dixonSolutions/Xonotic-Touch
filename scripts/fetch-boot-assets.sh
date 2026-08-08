#!/bin/bash
# Stage the minimum game assets a slim package needs to reach a working menu.
#
# The packaged app deliberately ships without the ~3 GB of maps, textures and
# music: those download on first launch. That split only works when the engine
# can boot far enough to *show* the download wizard, and the menu is drawn
# entirely from `gfx/menu/<skin>/`. Ship none of it and the first screen is a
# grid of missing-texture placeholders behind a barely readable dialog.
#
# This script stages that boot subset only (~31 MB). It reuses a local
# `engine/data/xonotic-data.pk3dir` when the developer already ran
# `scripts/fetch-sources.sh assets`, otherwise it pulls the files out of the
# upstream data repository with a blob-filtered sparse checkout, so a build
# transfers the boot subset instead of the whole repository.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
DATA_URL="${DATA_URL:-https://gitlab.com/xonotic/xonotic-data.pk3dir.git}"

# Keep in sync with `seta menu_skin` in xonotic-client.cfg: the menu falls back
# to notexture when the configured skin directory is missing.
BOOT_MENU_SKIN="${XONOTIC_BOOT_MENU_SKIN:-luma}"

DEST=""
SOURCE_PK3DIR="${XONOTIC_BOOT_ASSET_SOURCE:-$ROOT/engine/data/xonotic-data.pk3dir}"
FORCE=0

usage() {
    cat >&2 <<'EOF'
usage: fetch-boot-assets.sh --dest <data-dir> [--skin <name>] [--source <pk3dir>] [--force]

  --dest    Data directory being staged (the one that holds xonotic-data.pk3dir).
  --skin    Menu skin to stage (default: luma, matching menu_skin).
  --source  Local xonotic-data.pk3dir to copy from instead of cloning.
  --force   Re-stage even when the destination already looks populated.
EOF
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dest) DEST="${2:?--dest needs a directory}"; shift 2 ;;
        --skin) BOOT_MENU_SKIN="${2:?--skin needs a name}"; shift 2 ;;
        --source) SOURCE_PK3DIR="${2:?--source needs a directory}"; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage ;;
        *) echo "fetch-boot-assets: unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "$DEST" ] || usage

# Paths are relative to xonotic-data.pk3dir. Everything here is load-bearing
# before the first download finishes; the rest of gfx/ (HUD art, crosshairs,
# reticles, in-game fonts) is only reachable once gameplay is unlocked.
boot_asset_paths() {
    printf '%s\n' \
        "gfx/menu/${BOOT_MENU_SKIN}" \
        "gfx/colors" \
        "gfx/conchars.tga" \
        "gfx/conback.tga" \
        "gfx/conback2.tga" \
        "gfx/conback3.tga" \
        "gfx/loading.tga"
}

path_is_populated() {
    local path="$1"
    if [ -d "$path" ]; then
        [ -n "$(ls -A "$path" 2>/dev/null)" ]
        return
    fi
    [ -s "$path" ]
}

missing_boot_paths() {
    local dest_pk3dir="$1"
    local path
    while IFS= read -r path; do
        path_is_populated "$dest_pk3dir/$path" || printf '%s\n' "$path"
    done < <(boot_asset_paths)
}

copy_boot_paths() {
    local source_pk3dir="$1"
    local dest_pk3dir="$2"
    shift 2
    local path

    for path in "$@"; do
        if [ ! -e "$source_pk3dir/$path" ]; then
            return 1
        fi
    done

    for path in "$@"; do
        mkdir -p "$dest_pk3dir/$(dirname "$path")"
        cp -a "$source_pk3dir/$path" "$dest_pk3dir/$path"
    done
}

# Cone mode would also drag in every sibling file of the requested directories
# (all of gfx/, ~50 MB of reticles and crosshairs), so ask for exact paths.
clone_boot_paths() {
    local dest_pk3dir="$1"
    shift
    local tmp path
    local -a patterns=()

    for path in "$@"; do
        patterns+=("/$path")
    done

    tmp="$(mktemp -d)"
    # shellcheck disable=SC2064  # expand $tmp now, not at trap time
    trap "rm -rf '$tmp'" RETURN

    git clone --depth 1 --filter=blob:none --no-checkout "$DATA_URL" "$tmp/data" >&2
    (
        cd "$tmp/data"
        git sparse-checkout set --no-cone "${patterns[@]}"
        git checkout --quiet
    )
    copy_boot_paths "$tmp/data" "$dest_pk3dir" "$@"
}

DEST_PK3DIR="$DEST/xonotic-data.pk3dir"
mkdir -p "$DEST_PK3DIR"

if [ "$FORCE" = "1" ]; then
    mapfile -t MISSING < <(boot_asset_paths)
else
    mapfile -t MISSING < <(missing_boot_paths "$DEST_PK3DIR")
fi

if [ "${#MISSING[@]}" -eq 0 ]; then
    echo "fetch-boot-assets: boot assets already staged in $DEST_PK3DIR"
    exit 0
fi

if copy_boot_paths "$SOURCE_PK3DIR" "$DEST_PK3DIR" "${MISSING[@]}" 2>/dev/null; then
    echo "fetch-boot-assets: copied ${#MISSING[@]} boot asset paths from $SOURCE_PK3DIR"
else
    echo "fetch-boot-assets: fetching ${#MISSING[@]} boot asset paths from $DATA_URL"
    clone_boot_paths "$DEST_PK3DIR" "${MISSING[@]}"
fi

echo "fetch-boot-assets: staged $(du -sh "$DEST_PK3DIR/gfx" | cut -f1) of boot assets (menu skin: $BOOT_MENU_SKIN)"
