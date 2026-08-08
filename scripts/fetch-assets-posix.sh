#!/bin/sh
# POSIX first-launch asset download for Ubuntu Touch click confinement.
#
# packaging/start.sh prefers the bash helpers when bash is available. On
# confined devices bash is usually not exec'able, so this script covers the
# autobuild zip path with busybox wget/unzip only (docs/UBUNTU_TOUCH_LAUNCH.md).
set -eu

DATA_DIR="${1:?usage: fetch-assets-posix.sh <user-data-dir>}"
AUTOBUILD_URL="${XONOTIC_AUTOBUILD_URL:-https://beta.xonotic.org/autobuild}"
AUTOBUILD_USER="${XONOTIC_AUTOBUILD_USER:-xonotic}"
AUTOBUILD_PASS="${XONOTIC_AUTOBUILD_PASS:-g-23}"
PROGRESS="${XONOTIC_ASSET_FETCH_PROGRESS:-}"

progress_write() {
    status="$1"
    percent="$2"
    message="$3"
    if [ -z "$PROGRESS" ]; then
        return 0
    fi
    # Atomic replace — menu polls this every frame.
    tmp="${PROGRESS}.tmp.$$"
    mkdir -p "$(dirname "$PROGRESS")"
    {
        printf '%s\n' "$status"
        printf '%s\n' "$percent"
        printf '%s\n' "$message"
    } > "$tmp"
    mv -f "$tmp" "$PROGRESS"
}

has_pk3() {
    # $1 = glob under DATA_DIR, e.g. 'xonotic-*-data.pk3'
    # Intentional unquoted expand so the glob is evaluated.
    # shellcheck disable=SC2086
    set -- "$DATA_DIR"/$1
    [ -f "$1" ]
}

assets_ready() {
    if [ -f "$DATA_DIR/.assets-ready" ]; then
        return 0
    fi
    if has_pk3 'xonotic-*-data.pk3' \
        && has_pk3 'xonotic-*-maps.pk3' \
        && has_pk3 'xonotic-*-music.pk3'; then
        : > "$DATA_DIR/.assets-ready"
        return 0
    fi
    return 1
}

file_size() {
    if [ -f "$1" ]; then
        wc -c < "$1" | tr -d ' '
    else
        printf '%s\n' 0
    fi
}

download_zip() {
    zip_path="$1"
    zip_name="$2"
    pct_lo="$3"
    pct_hi="$4"
    url="${AUTOBUILD_URL}/${zip_name}"
    expected=0
    have=0
    pct=0
    mb=0
    emb=0

    progress_write running "$pct_lo" "Downloading ${zip_name}..."

    if command -v curl >/dev/null 2>&1; then
        expected=$(
            curl -sI -L --user "${AUTOBUILD_USER}:${AUTOBUILD_PASS}" "$url" \
                | awk 'BEGIN{c=0} tolower($1)=="content-length:" {c=$2} END{print c+0}' \
                | tr -d '\r'
        )
        mkdir -p "$(dirname "$zip_path")"
        # Resume partial downloads across relaunch / orphan cleanup.
        have=$(file_size "$zip_path")
        if [ "$expected" -gt 0 ] && [ "$have" -ge "$expected" ]; then
            progress_write running "$pct_hi" "Downloaded ${zip_name}"
            return 0
        fi
        curl -fL -C - --user "${AUTOBUILD_USER}:${AUTOBUILD_PASS}" \
            -o "$zip_path" "$url" &
        cpid=$!
        while kill -0 "$cpid" 2>/dev/null; do
            have=$(file_size "$zip_path")
            pct=$pct_lo
            if [ "$expected" -gt 0 ]; then
                pct=$((pct_lo + have * (pct_hi - pct_lo) / expected))
                if [ "$pct" -gt "$pct_hi" ]; then
                    pct=$pct_hi
                fi
                emb=$((expected / 1048576))
            fi
            mb=$((have / 1048576))
            if [ "$expected" -gt 0 ]; then
                progress_write running "$pct" \
                    "Downloading ${zip_name} (${mb} / ${emb} MB)..."
            else
                progress_write running "$pct" \
                    "Downloading ${zip_name} (${mb} MB)..."
            fi
            sleep 1
        done
        wait "$cpid"
        return $?
    fi

    if command -v wget >/dev/null 2>&1; then
        scheme="${AUTOBUILD_URL%%://*}"
        host_path="${AUTOBUILD_URL#*://}"
        # Busybox wget: no live percent; bump message before/after.
        progress_write running "$pct_lo" "Downloading ${zip_name} (please wait)..."
        wget -O "$zip_path" \
            "${scheme}://${AUTOBUILD_USER}:${AUTOBUILD_PASS}@${host_path}/${zip_name}"
        progress_write running "$pct_hi" "Downloaded ${zip_name}"
        return
    fi
    echo "xonotic-touch: curl or wget required to download game assets" >&2
    return 1
}

extract_pk3() {
    zip_path="$1"
    extract_dir="$2"

    if ! command -v unzip >/dev/null 2>&1; then
        echo "xonotic-touch: unzip required to extract game assets" >&2
        return 1
    fi

    mkdir -p "$extract_dir" "$DATA_DIR"
    unzip -q "$zip_path" "Xonotic/data/*.pk3" -d "$extract_dir"
    # Busybox ash: expand safely so a missing glob does not create a bogus name.
    set -- "$extract_dir"/Xonotic/data/*.pk3
    if [ ! -f "$1" ]; then
        echo "xonotic-touch: no pk3 files extracted from $zip_path" >&2
        return 1
    fi
    mv "$@" "$DATA_DIR/"
    rm -rf "$extract_dir/Xonotic"
}

if [ "${XONOTIC_SKIP_ASSET_FETCH:-0}" = "1" ]; then
    exit 0
fi

mkdir -p "$DATA_DIR"

if assets_ready; then
    progress_write "done" 100 "Game data already installed"
    exit 0
fi

progress_write running 5 "Starting download from Xonotic servers..."
echo "xonotic-touch: downloading game assets (first launch may take several minutes)..." >&2

tmp="$DATA_DIR/.fetch-tmp"
mkdir -p "$tmp"
zip_path="$tmp/xonotic.zip"
extract_dir="$tmp/extract"

if ! has_pk3 'xonotic-*-data.pk3'; then
    download_zip "$zip_path" "Xonotic-latest.zip" 10 34
    progress_write running 35 "Installing core game data..."
    extract_pk3 "$zip_path" "$extract_dir"
    rm -f "$zip_path"
fi

if ! has_pk3 'xonotic-*-maps.pk3'; then
    download_zip "$zip_path" "Xonotic-latest-mappingsupport.zip" 40 58
    progress_write running 60 "Installing maps..."
    extract_pk3 "$zip_path" "$extract_dir"
    rm -f "$zip_path"
fi

if ! has_pk3 'xonotic-*-music.pk3'; then
    download_zip "$zip_path" "Xonotic-latest-high.zip" 65 83
    progress_write running 85 "Installing music..."
    extract_pk3 "$zip_path" "$extract_dir"
    rm -f "$zip_path"
fi

if ! has_pk3 'xonotic-*-nexcompat.pk3'; then
    # Nexcompat ships in the main zip; re-fetch if still missing after core.
    if ! has_pk3 'xonotic-*-data.pk3'; then
        download_zip "$zip_path" "Xonotic-latest.zip" 88 94
        progress_write running 95 "Installing compatibility pack..."
        extract_pk3 "$zip_path" "$extract_dir"
        rm -f "$zip_path"
    fi
fi

rm -rf "$tmp"

if assets_ready; then
    progress_write "done" 100 "Download complete"
    exit 0
fi

progress_write error 0 "Download failed — check network and retry"
exit 1
