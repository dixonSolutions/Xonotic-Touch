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

# POSIX sh has no local variables: every name a function sets is the script's.
# This one used to call its temp file `tmp`, which is also the download dir
# below, so the first progress update sent every later download and extract
# into "asset-progress.txt.tmp.<pid>/" and the fetch died there.
progress_write() {
    _pw_status="$1"
    _pw_percent="$2"
    _pw_message="$3"
    if [ -z "$PROGRESS" ]; then
        return 0
    fi
    # Atomic replace — menu polls this every frame.
    _pw_tmp="${PROGRESS}.tmp.$$"
    mkdir -p "$(dirname "$PROGRESS")"
    {
        printf '%s\n' "$_pw_status"
        printf '%s\n' "$_pw_percent"
        printf '%s\n' "$_pw_message"
    } > "$_pw_tmp"
    mv -f "$_pw_tmp" "$PROGRESS"
}

# The click ships curl without its libraries and leans on the phone's libcurl.
# Use it only if it actually runs; busybox wget (through openssl) is the
# fallback, about 2.4 times slower from the autobuild server.
curl_usable() {
    command -v curl >/dev/null 2>&1 && curl --version >/dev/null 2>&1
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

# stat, not `wc -c`: busybox wc reads the whole file, and the progress loop
# asks every second while a zip grows towards 1.2 GB.
file_size() {
    if [ -f "$1" ]; then
        stat -c %s "$1"
    else
        printf '%s\n' 0
    fi
}

# Report a download's size until <pid> exits, then return its status.
wait_with_progress() {
    _wp_pid="$1"
    _wp_path="$2"
    _wp_name="$3"
    _wp_expected="$4"
    _wp_lo="$5"
    _wp_hi="$6"
    while kill -0 "$_wp_pid" 2>/dev/null; do
        _wp_have=$(file_size "$_wp_path")
        _wp_mb=$((_wp_have / 1048576))
        if [ "$_wp_expected" -gt 0 ]; then
            _wp_pct=$((_wp_lo + _wp_have * (_wp_hi - _wp_lo) / _wp_expected))
            if [ "$_wp_pct" -gt "$_wp_hi" ]; then
                _wp_pct=$_wp_hi
            fi
            progress_write running "$_wp_pct" \
                "Downloading ${_wp_name} (${_wp_mb} / $((_wp_expected / 1048576)) MB)..."
        else
            progress_write running "$_wp_lo" \
                "Downloading ${_wp_name} (${_wp_mb} MB)..."
        fi
        sleep 1
    done
    wait "$_wp_pid"
}

download_zip() {
    zip_path="$1"
    zip_name="$2"
    pct_lo="$3"
    pct_hi="$4"
    url="${AUTOBUILD_URL}/${zip_name}"
    expected=0
    have=0

    progress_write running "$pct_lo" "Downloading ${zip_name}..."
    mkdir -p "$(dirname "$zip_path")"

    if curl_usable; then
        expected=$(
            curl -sI -L --user "${AUTOBUILD_USER}:${AUTOBUILD_PASS}" "$url" \
                | awk 'BEGIN{c=0} tolower($1)=="content-length:" {c=$2} END{print c+0}' \
                | tr -d '\r'
        )
        # Resume partial downloads across relaunch / orphan cleanup.
        have=$(file_size "$zip_path")
        if [ "$expected" -gt 0 ] && [ "$have" -ge "$expected" ]; then
            progress_write running "$pct_hi" "Downloaded ${zip_name}"
            return 0
        fi
        curl -fL -C - --user "${AUTOBUILD_USER}:${AUTOBUILD_PASS}" \
            -o "$zip_path" "$url" &
        wait_with_progress $! "$zip_path" "$zip_name" "$expected" "$pct_lo" "$pct_hi"
        return $?
    fi

    if command -v wget >/dev/null 2>&1; then
        scheme="${AUTOBUILD_URL%%://*}"
        host_path="${AUTOBUILD_URL#*://}"
        url="${scheme}://${AUTOBUILD_USER}:${AUTOBUILD_PASS}@${host_path}/${zip_name}"
        # Busybox wget prints the response headers on stderr with -S.
        expected=$(
            wget -S --spider "$url" 2>&1 \
                | awk 'BEGIN{c=0} tolower($1)=="content-length:" {c=$2} END{print c+0}' \
                | tr -d '\r'
        )
        # A phone suspends or closes the app mid-download, and the wizard
        # promises partials resume: -c continues, and a complete file is not
        # asked for again (busybox wget fails on the 416 that would get).
        have=$(file_size "$zip_path")
        if [ "$expected" -gt 0 ] && [ "$have" -ge "$expected" ]; then
            progress_write running "$pct_hi" "Downloaded ${zip_name}"
            return 0
        fi
        wget -q -c -O "$zip_path" "$url" &
        wait_with_progress $! "$zip_path" "$zip_name" "$expected" "$pct_lo" "$pct_hi"
        return $?
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

progress_write running 5 "Starting downloads from Xonotic servers..."
echo "xonotic-touch: downloading game assets (first launch may take several minutes)..." >&2

tmp="$DATA_DIR/.fetch-tmp"
mkdir -p "$tmp"
extract_dir="$tmp/extract"
# pid files for background curls
: > "$tmp/pids"

start_bg_curl() {
    _bg_path="$1"
    _bg_name="$2"
    _bg_url="${AUTOBUILD_URL}/${_bg_name}"
    if curl_usable; then
        curl -fL -C - --user "${AUTOBUILD_USER}:${AUTOBUILD_PASS}" \
            -o "$_bg_path" "$_bg_url" >/dev/null 2>&1 &
        echo $! >> "$tmp/pids"
        return 0
    fi
    # Fallback: serial wget path via download_zip.
    download_zip "$_bg_path" "$_bg_name" 10 80
}

# Aggregate progress until every background curl exits.
wait_bg_downloads() {
    while [ -s "$tmp/pids" ]; do
        alive=0
        have=0
        for f in "$tmp"/xonotic.zip "$tmp"/xonotic-maps.zip "$tmp"/xonotic-music.zip; do
            [ -f "$f" ] || continue
            have=$((have + $(file_size "$f")))
        done
        mb=$((have / 1048576))
        : > "$tmp/pids.new"
        while read -r cpid; do
            [ -n "$cpid" ] || continue
            if kill -0 "$cpid" 2>/dev/null; then
                alive=$((alive + 1))
                echo "$cpid" >> "$tmp/pids.new"
            else
                wait "$cpid" || exit 1
            fi
        done < "$tmp/pids"
        mv -f "$tmp/pids.new" "$tmp/pids"
        progress_write running 20 \
            "Downloading packs (${mb} MB, ${alive} active)..."
        [ "$alive" -eq 0 ] && break
        sleep 1
    done
}

# The core zip carries the data, maps, music and compat packs, so it goes first
# and alone; the maps and music zips are only for a pack it did not provide.
# Deciding all three up front fetched 2.7 GB where 1.2 GB was enough.
if ! has_pk3 'xonotic-*-data.pk3' || ! has_pk3 'xonotic-*-nexcompat.pk3'; then
    start_bg_curl "$tmp/xonotic.zip" "Xonotic-latest.zip"
    wait_bg_downloads
    progress_write running 88 "Installing core game data..."
    extract_pk3 "$tmp/xonotic.zip" "$extract_dir"
    rm -f "$tmp/xonotic.zip"
fi

need_maps=0
need_music=0
if ! has_pk3 'xonotic-*-maps.pk3'; then
    need_maps=1
    start_bg_curl "$tmp/xonotic-maps.zip" "Xonotic-latest-mappingsupport.zip"
fi
if ! has_pk3 'xonotic-*-music.pk3'; then
    need_music=1
    start_bg_curl "$tmp/xonotic-music.zip" "Xonotic-latest-high.zip"
fi
wait_bg_downloads

if [ "$need_maps" = "1" ]; then
    progress_write running 92 "Installing maps..."
    extract_pk3 "$tmp/xonotic-maps.zip" "$extract_dir"
    rm -f "$tmp/xonotic-maps.zip"
fi
if [ "$need_music" = "1" ]; then
    progress_write running 96 "Installing music..."
    extract_pk3 "$tmp/xonotic-music.zip" "$extract_dir"
    rm -f "$tmp/xonotic-music.zip"
fi

rm -rf "$tmp"

if assets_ready; then
    progress_write "done" 100 "Download complete"
    exit 0
fi

progress_write error 0 "Download failed — check network and retry"
exit 1
