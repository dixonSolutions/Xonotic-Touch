#!/bin/bash
# Shared game asset download helpers (build-time and first-run).
# Only tighten shell options when executed as a script — sourcing from
# packaging/start.sh must not turn on nounset/errexit for the launcher.
if [ "${BASH_SOURCE[0]-}" = "${0-}" ]; then
    set -euo pipefail
fi

XONOTIC_DATA_PK3DIR_ASSET_DIRS=(textures models gfx sound particles demos cubemaps maps)

XONOTIC_AUTOBUILD_URL="${XONOTIC_AUTOBUILD_URL:-https://beta.xonotic.org/autobuild}"
XONOTIC_AUTOBUILD_USER="${XONOTIC_AUTOBUILD_USER:-xonotic}"
XONOTIC_AUTOBUILD_PASS="${XONOTIC_AUTOBUILD_PASS:-g-23}"

xonotic_asset_dirs_missing() {
    local data_dir="$1"
    local pk3dir="$data_dir/xonotic-data.pk3dir"
    local dir

    if compgen -G "$data_dir/xonotic-*-data.pk3" >/dev/null; then
        return 1
    fi

    for dir in "${XONOTIC_DATA_PK3DIR_ASSET_DIRS[@]}"; do
        if [ ! -d "$pk3dir/$dir" ] || [ -z "$(ls -A "$pk3dir/$dir" 2>/dev/null)" ]; then
            return 0
        fi
    done
    return 1
}

xonotic_maps_assets_missing() {
    local data_dir="$1"

    if compgen -G "$data_dir/xonotic-*-maps.pk3" >/dev/null; then
        return 1
    fi
    if [ -d "$data_dir/xonotic-maps.pk3dir/maps" ] \
        && [ -n "$(ls -A "$data_dir/xonotic-maps.pk3dir/maps" 2>/dev/null)" ]; then
        return 1
    fi
    return 0
}

xonotic_music_assets_missing() {
    local data_dir="$1"

    if compgen -G "$data_dir/xonotic-*-music.pk3" >/dev/null; then
        return 1
    fi
    if [ -d "$data_dir/xonotic-music.pk3dir/music" ] \
        && [ -n "$(ls -A "$data_dir/xonotic-music.pk3dir/music" 2>/dev/null)" ]; then
        return 1
    fi
    return 0
}

# Default menu / game-open theme (xonotic-common.cfg menu_cdtrack).
XONOTIC_MENU_THEME_TRACK="${XONOTIC_MENU_THEME_TRACK:-rising-of-the-phoenix}"
XONOTIC_MENU_THEME_URL="${XONOTIC_MENU_THEME_URL:-https://gitlab.com/xonotic/xonotic-music.pk3dir/-/raw/master/sound/cdtracks/${XONOTIC_MENU_THEME_TRACK}.ogg}"

xonotic_menu_theme_path() {
    local data_dir="$1"
    echo "$data_dir/xonotic-touch-theme.pk3dir/sound/cdtracks/${XONOTIC_MENU_THEME_TRACK}.ogg"
}

# True when the open-menu theme ogg is on disk (tiny boot pk3dir or full music pack).
xonotic_menu_theme_ready() {
    local data_dir="$1"
    local theme
    theme="$(xonotic_menu_theme_path "$data_dir")"
    if [ -f "$theme" ] && [ "$(xonotic_file_size "$theme")" -gt 100000 ]; then
        return 0
    fi
    # Full music pack may already ship the track under a pk3dir tree.
    if [ -f "$data_dir/xonotic-music.pk3dir/sound/cdtracks/${XONOTIC_MENU_THEME_TRACK}.ogg" ]; then
        return 0
    fi
    return 1
}

xonotic_mark_music_ready() {
    local data_dir="$1"
    mkdir -p "$data_dir/touch"
    : > "$data_dir/touch/music-ready.txt"
}

# ~6–7 MB menu theme so the wizard can play BGM long before the full music zip.
xonotic_ensure_menu_theme_track() {
    local data_dir="$1"
    local dest partial dest_dir

    if xonotic_menu_theme_ready "$data_dir"; then
        xonotic_mark_music_ready "$data_dir"
        return 0
    fi

    dest="$(xonotic_menu_theme_path "$data_dir")"
    dest_dir="$(dirname "$dest")"
    partial="${dest}.partial"
    mkdir -p "$dest_dir"

    echo "xonotic-touch: fetching menu theme (${XONOTIC_MENU_THEME_TRACK})..." >&2
    if ! command -v curl >/dev/null 2>&1; then
        echo "xonotic-touch: curl missing — cannot fetch menu theme early" >&2
        return 1
    fi

    # Keep the main pack progress intact; only nudge if we have not started yet.
    if [ ! -f "${XONOTIC_ASSET_FETCH_PROGRESS:-}" ]; then
        :
    fi

    if curl -fL --connect-timeout 20 --max-time 180 -C - \
        -o "$partial" "$XONOTIC_MENU_THEME_URL"; then
        mv -f "$partial" "$dest"
        xonotic_mark_music_ready "$data_dir"
        echo "xonotic-touch: menu theme ready at $dest" >&2
        return 0
    fi
    rm -f "$partial" 2>/dev/null || true
    return 1
}

xonotic_nexcompat_assets_missing() {
    local data_dir="$1"

    if compgen -G "$data_dir/xonotic-*-nexcompat.pk3" >/dev/null; then
        return 1
    fi
    if [ -d "$data_dir/xonotic-nexcompat.pk3dir/textures" ] \
        && [ -n "$(ls -A "$data_dir/xonotic-nexcompat.pk3dir/textures" 2>/dev/null)" ]; then
        return 1
    fi
    return 0
}

xonotic_assets_need_fetch() {
    local data_dir="$1"

    xonotic_asset_dirs_missing "$data_dir" \
        || xonotic_maps_assets_missing "$data_dir" \
        || xonotic_music_assets_missing "$data_dir" \
        || xonotic_nexcompat_assets_missing "$data_dir"
}

xonotic_assets_ready_marker() {
    local data_dir="$1"
    echo "$data_dir/.assets-ready"
}

xonotic_assets_mark_ready() {
    local data_dir="$1"
    local marker
    marker="$(xonotic_assets_ready_marker "$data_dir")"
    : > "$marker"
}

xonotic_assets_are_ready() {
    local data_dir="$1"
    local marker
    marker="$(xonotic_assets_ready_marker "$data_dir")"
    if [ -f "$marker" ]; then
        return 0
    fi
    if ! xonotic_assets_need_fetch "$data_dir"; then
        xonotic_assets_mark_ready "$data_dir"
        return 0
    fi
    return 1
}

xonotic_progress_write() {
    local status="$1"
    local percent="$2"
    local message="$3"
    local file="${XONOTIC_ASSET_FETCH_PROGRESS:-}"
    local tmp

    if [ -z "$file" ]; then
        return 0
    fi
    mkdir -p "$(dirname "$file")"
    # Atomic replace so the menu never reads a half-written tick.
    tmp="${file}.tmp.$$"
    {
        printf '%s\n' "$status"
        printf '%s\n' "$percent"
        printf '%s\n' "$message"
    } > "$tmp"
    mv -f "$tmp" "$file"
}

xonotic_fetch_pk3dir_sparse() {
    local data_dir="$1"
    local data_url="${DATA_URL:-https://gitlab.com/xonotic/xonotic-data.pk3dir.git}"
    local pk3dir="$data_dir/xonotic-data.pk3dir"
    local tmp
    local dir

    if ! command -v git >/dev/null 2>&1; then
        return 1
    fi

    tmp="$(mktemp -d)"
    git clone --depth 1 --filter=blob:none --sparse "$data_url" "$tmp/data"
    (
        cd "$tmp/data"
        git sparse-checkout set "${XONOTIC_DATA_PK3DIR_ASSET_DIRS[@]}"
    )
    mkdir -p "$pk3dir"
    for dir in "${XONOTIC_DATA_PK3DIR_ASSET_DIRS[@]}"; do
        if [ -d "$tmp/data/$dir" ]; then
            mkdir -p "$pk3dir/$dir"
            rsync -a "$tmp/data/$dir/" "$pk3dir/$dir/"
        fi
    done
    rm -rf "$tmp"
}

xonotic_clone_pk3dir_repo() {
    local data_dir="$1"
    local dest_name="$2"
    local url="$3"
    local marker="$4"

    if ! command -v git >/dev/null 2>&1; then
        return 1
    fi

    local dest="$data_dir/$dest_name"
    if [ -e "$dest/$marker" ] || [ -d "$dest/$marker" ]; then
        return 0
    fi
    rm -rf "$dest"
    git clone --depth 1 "$url" "$dest"
}

xonotic_fetch_git_assets() {
    local data_dir="$1"

    if ! command -v git >/dev/null 2>&1; then
        return 1
    fi

    xonotic_progress_write running 10 "Downloading core game data..."
    xonotic_fetch_pk3dir_sparse "$data_dir" || return 1

    if xonotic_maps_assets_missing "$data_dir"; then
        xonotic_progress_write running 45 "Downloading maps..."
        xonotic_clone_pk3dir_repo "$data_dir" xonotic-maps.pk3dir \
            "${MAPS_URL:-https://gitlab.com/xonotic/xonotic-maps.pk3dir.git}" maps || return 1
    fi
    if xonotic_music_assets_missing "$data_dir"; then
        xonotic_progress_write running 70 "Downloading music..."
        xonotic_clone_pk3dir_repo "$data_dir" xonotic-music.pk3dir \
            "${MUSIC_URL:-https://gitlab.com/xonotic/xonotic-music.pk3dir.git}" music || return 1
    fi
    if xonotic_nexcompat_assets_missing "$data_dir"; then
        xonotic_progress_write running 88 "Downloading compatibility pack..."
        xonotic_clone_pk3dir_repo "$data_dir" xonotic-nexcompat.pk3dir \
            "${NEXCOMPAT_URL:-https://gitlab.com/xonotic/xonotic-nexcompat.pk3dir.git}" textures || return 1
    fi
}

# How many pack zips to pull at once (device can usually saturate the link
# with 2–4). Override with XONOTIC_FETCH_PARALLEL=1 for serial.
xonotic_fetch_parallel_jobs() {
    local n="${XONOTIC_FETCH_PARALLEL:-}"
    local cpus
    if [ -n "$n" ]; then
        echo "$n"
        return 0
    fi
    cpus="$(nproc 2>/dev/null || echo 2)"
    if [ "$cpus" -ge 8 ] 2>/dev/null; then
        echo 4
    elif [ "$cpus" -ge 4 ] 2>/dev/null; then
        echo 3
    else
        echo 2
    fi
}

# Extra HTTP connections per large zip (aria2c). 1 = single-stream curl.
xonotic_fetch_connections() {
    local n="${XONOTIC_FETCH_CONNECTIONS:-}"
    if [ -n "$n" ]; then
        echo "$n"
        return 0
    fi
    if command -v aria2c >/dev/null 2>&1; then
        echo 6
    else
        echo 1
    fi
}

xonotic_autobuild_content_length() {
    local url="$1"
    curl -sI -L --user "${XONOTIC_AUTOBUILD_USER}:${XONOTIC_AUTOBUILD_PASS}" "$url" \
        | awk 'BEGIN{c=0} tolower($1)=="content-length:" {c=$2} END{print c+0}' \
        | tr -d '\r'
}

xonotic_file_size() {
    if [ -f "$1" ]; then
        wc -c < "$1" | tr -d ' '
    else
        echo 0
    fi
}

# Last background download PID started by xonotic_start_autobuild_download.
# Must NOT be captured via $(...) — that runs the starter in a subshell so the
# PID is not a child of the fetch loop and `wait` aborts the whole job.
XONOTIC_DOWNLOAD_PID=0

# Start one zip download in the background (no progress loop).
# Sets XONOTIC_DOWNLOAD_PID (0 = already complete on disk).
xonotic_start_autobuild_download() {
    local zip_path="$1"
    local zip_name="$2"
    local url="${XONOTIC_AUTOBUILD_URL}/${zip_name}"
    local expected="${3:-0}"
    local conns

    XONOTIC_DOWNLOAD_PID=0
    mkdir -p "$(dirname "$zip_path")"
    if [ "${expected:-0}" -gt 0 ] 2>/dev/null \
        && [ "$(xonotic_file_size "$zip_path")" -ge "$expected" ]; then
        return 0
    fi

    conns="$(xonotic_fetch_connections)"
    # Close fetch.lock fd 9 in the child so orphan curls cannot block the next
    # fetchd after the parent shell dies (flock would otherwise stay held).
    if [ "$conns" -gt 1 ] 2>/dev/null && command -v aria2c >/dev/null 2>&1; then
        # Multi-connection: much better on high-latency / rate-limited links.
        (
            exec 9>&-
            aria2c -c --console-log-level=warn --summary-interval=0 \
                --http-user="${XONOTIC_AUTOBUILD_USER}" \
                --http-passwd="${XONOTIC_AUTOBUILD_PASS}" \
                -x "$conns" -s "$conns" -j 1 -k 1M \
                -d "$(dirname "$zip_path")" -o "$(basename "$zip_path")" \
                "$url" >/dev/null 2>&1
        ) &
        XONOTIC_DOWNLOAD_PID=$!
        return 0
    fi

    if command -v curl >/dev/null 2>&1; then
        (
            exec 9>&-
            curl -fL -C - --user "${XONOTIC_AUTOBUILD_USER}:${XONOTIC_AUTOBUILD_PASS}" \
                -o "$zip_path" "$url" >/dev/null 2>&1
        ) &
        XONOTIC_DOWNLOAD_PID=$!
        return 0
    fi

    if command -v wget >/dev/null 2>&1; then
        local scheme="${XONOTIC_AUTOBUILD_URL%%://*}"
        local host_path="${XONOTIC_AUTOBUILD_URL#*://}"
        (
            exec 9>&-
            wget -q -O "$zip_path" \
                "${scheme}://${XONOTIC_AUTOBUILD_USER}:${XONOTIC_AUTOBUILD_PASS}@${host_path}/${zip_name}" \
                >/dev/null 2>&1
        ) &
        XONOTIC_DOWNLOAD_PID=$!
        return 0
    fi

    echo "xonotic: curl, wget, or aria2c required to download game assets" >&2
    return 1
}

# Wait for a download PID. Handles orphans from a previous fetchd (not children
# of this shell) by polling; success is "process gone + zip size OK".
xonotic_wait_download_pid() {
    local pid="$1"
    local zip_path="$2"
    local expected="${3:-0}"
    local have

    [ -n "$pid" ] && [ "$pid" != "0" ] || return 0

    if kill -0 "$pid" 2>/dev/null; then
        # Our child: wait for exit status. Not our child: poll until gone.
        if ! wait "$pid" 2>/dev/null; then
            while kill -0 "$pid" 2>/dev/null; do
                sleep 1
            done
        fi
    else
        wait "$pid" 2>/dev/null || true
    fi

    have="$(xonotic_file_size "$zip_path")"
    if [ "$expected" -gt 0 ] 2>/dev/null && [ "$have" -lt "$expected" ]; then
        echo "xonotic-touch: download incomplete for $(basename "$zip_path") ($have / $expected bytes)" >&2
        return 1
    fi
    if [ "$have" -le 0 ] 2>/dev/null; then
        echo "xonotic-touch: download produced empty file: $zip_path" >&2
        return 1
    fi
    return 0
}

# Download one autobuild zip while updating the wizard percent between pct_lo..pct_hi.
# Kept for callers that still want a single serial job.
xonotic_download_autobuild_zip() {
    local zip_path="$1"
    local zip_name="$2"
    local pct_lo="${3:-15}"
    local pct_hi="${4:-$((pct_lo + 18))}"
    local url="${XONOTIC_AUTOBUILD_URL}/${zip_name}"
    local expected=0 have=0 pct mb pid

    xonotic_progress_write running "$pct_lo" "Downloading ${zip_name}..."
    if command -v curl >/dev/null 2>&1; then
        expected="$(xonotic_autobuild_content_length "$url")"
    fi
    xonotic_start_autobuild_download "$zip_path" "$zip_name" "$expected" || return 1
    pid="${XONOTIC_DOWNLOAD_PID:-0}"
    if [ "${pid:-0}" = "0" ]; then
        xonotic_progress_write running "$pct_hi" "Downloaded ${zip_name}"
        return 0
    fi
    while kill -0 "$pid" 2>/dev/null; do
        have="$(xonotic_file_size "$zip_path")"
        pct="$pct_lo"
        if [ "${expected:-0}" -gt 0 ] 2>/dev/null; then
            pct=$((pct_lo + have * (pct_hi - pct_lo) / expected))
            [ "$pct" -gt "$pct_hi" ] && pct="$pct_hi"
            xonotic_progress_write running "$pct" \
                "Downloading ${zip_name} ($((have / 1048576)) / $((expected / 1048576)) MB)..."
        else
            xonotic_progress_write running "$pct" \
                "Downloading ${zip_name} ($((have / 1048576)) MB)..."
        fi
        sleep 1
    done
    xonotic_wait_download_pid "$pid" "$zip_path" "$expected" || return 1
}

xonotic_extract_autobuild_pk3() {
    local zip_path="$1"
    local data_dir="$2"
    local extract_dir="$3"

    if ! command -v unzip >/dev/null 2>&1; then
        echo "xonotic: unzip required to extract game assets" >&2
        return 1
    fi
    if [ ! -f "$zip_path" ]; then
        echo "xonotic: missing zip for extract: $zip_path" >&2
        return 1
    fi

    mkdir -p "$extract_dir" "$data_dir"
    unzip -q "$zip_path" "Xonotic/data/*.pk3" -d "$extract_dir"
    mv "$extract_dir/Xonotic/data/"*.pk3 "$data_dir/"
    rm -rf "$extract_dir/Xonotic"
}

# Install one finished zip as soon as it lands so later packs keep downloading
# and the wizard can start menu music without waiting for the whole queue.
xonotic_install_autobuild_zip() {
    local data_dir="$1"
    local zip_path="$2"
    local kind="$3" # core|maps|music
    local extract_dir="$data_dir/.fetch-tmp/extract"

    case "$kind" in
        core)
            xonotic_progress_write running 88 "Installing core game data..."
            xonotic_extract_autobuild_pk3 "$zip_path" "$data_dir" "$extract_dir" || return 1
            ;;
        maps)
            xonotic_progress_write running 92 "Installing maps..."
            xonotic_extract_autobuild_pk3 "$zip_path" "$data_dir" "$extract_dir" || return 1
            ;;
        music)
            xonotic_progress_write running 96 "Installing music..."
            xonotic_extract_autobuild_pk3 "$zip_path" "$data_dir" "$extract_dir" || return 1
            # Menu polls this (plain name — no leading dot) to start BGM early.
            xonotic_mark_music_ready "$data_dir"
            ;;
        *) return 1 ;;
    esac
    # Keep the zip until install succeeded; only then free the disk.
    rm -f "$zip_path"
}

# Pull every missing pack zip at once (capped). Resume partials with curl -C - /
# aria2c -c; never re-download a pack that is already installed or a zip that
# already matches Content-Length. Extract each pack as soon as its zip finishes.
xonotic_fetch_autobuild_assets() {
    local data_dir="$1"
    local tmp="$data_dir/.fetch-tmp"
    local parallel jobs=0 next=0
    local -a names=() paths=() expecteds=() pids=() kinds=() installed=()
    local idx name path url expected pid alive have total_exp total_have pct mb emb
    local kind

    mkdir -p "$tmp" "$data_dir/touch"
    parallel="$(xonotic_fetch_parallel_jobs)"

    # Menu theme first (~7 MB): wizard can play rising-of-the-phoenix while the
    # multi-GB packs continue. Full music zip still fills out the soundtrack.
    xonotic_ensure_menu_theme_track "$data_dir" || true
    if ! xonotic_music_assets_missing "$data_dir"; then
        xonotic_mark_music_ready "$data_dir"
    fi

    # Skip anything already present on disk — never re-download finished packs.
    # Music zip is queued first so parallel slots prefer soundtrack completion.
    if xonotic_music_assets_missing "$data_dir"; then
        names+=("Xonotic-latest-high.zip")
        paths+=("$tmp/xonotic-music.zip")
        kinds+=("music")
    fi
    if xonotic_asset_dirs_missing "$data_dir" || xonotic_nexcompat_assets_missing "$data_dir"; then
        names+=("Xonotic-latest.zip")
        paths+=("$tmp/xonotic.zip")
        kinds+=("core")
    fi
    if xonotic_maps_assets_missing "$data_dir"; then
        names+=("Xonotic-latest-mappingsupport.zip")
        paths+=("$tmp/xonotic-maps.zip")
        kinds+=("maps")
    fi

    if [ "${#names[@]}" -eq 0 ]; then
        return 0
    fi

    xonotic_progress_write running 8 \
        "Resuming ${#names[@]} pack download(s) (up to ${parallel} at once)..."

    for idx in "${!names[@]}"; do
        url="${XONOTIC_AUTOBUILD_URL}/${names[$idx]}"
        expected=0
        if command -v curl >/dev/null 2>&1; then
            expected="$(xonotic_autobuild_content_length "$url")"
        fi
        expecteds+=("$expected")
        pids+=("")
        installed+=(0)
    done

    next=0
    while [ "$next" -lt "${#names[@]}" ] || [ "$jobs" -gt 0 ]; do
        while [ "$jobs" -lt "$parallel" ] && [ "$next" -lt "${#names[@]}" ]; do
            name="${names[$next]}"
            path="${paths[$next]}"
            expected="${expecteds[$next]}"
            kind="${kinds[$next]}"
            # Already-complete zip from a previous session: install, don't re-get.
            if [ "$expected" -gt 0 ] 2>/dev/null \
                && [ "$(xonotic_file_size "$path")" -ge "$expected" ]; then
                xonotic_install_autobuild_zip "$data_dir" "$path" "$kind" || return 1
                pids[$next]=0
                installed[$next]=1
                next=$((next + 1))
                continue
            fi
            # curl -C - / aria2c -c continue the partial file in place.
            # Call directly (not $(...)) so $! stays a child of this shell.
            xonotic_start_autobuild_download "$path" "$name" "$expected" || return 1
            pid="${XONOTIC_DOWNLOAD_PID:-0}"
            pids[$next]="$pid"
            if [ "$pid" != "0" ]; then
                jobs=$((jobs + 1))
                have="$(xonotic_file_size "$path")"
                if [ "$have" -gt 0 ] 2>/dev/null; then
                    echo "xonotic-touch: resuming $name at $((have / 1048576)) MB (pid $pid)" >&2
                else
                    echo "xonotic-touch: download started: $name (pid $pid)" >&2
                fi
            fi
            next=$((next + 1))
        done

        alive=0
        total_exp=0
        total_have=0
        for idx in "${!names[@]}"; do
            expected="${expecteds[$idx]:-0}"
            have="$(xonotic_file_size "${paths[$idx]}")"
            # Count installed packs as fully done for the bar.
            if [ "${installed[$idx]:-0}" = "1" ] && [ "$expected" -gt 0 ]; then
                have="$expected"
            fi
            [ "$expected" -gt 0 ] 2>/dev/null && total_exp=$((total_exp + expected))
            total_have=$((total_have + have))
            pid="${pids[$idx]:-}"
            if [ -n "$pid" ] && [ "$pid" != "0" ] && kill -0 "$pid" 2>/dev/null; then
                alive=$((alive + 1))
            fi
        done

        pct=10
        if [ "$total_exp" -gt 0 ]; then
            pct=$((10 + total_have * 78 / total_exp))
            [ "$pct" -gt 88 ] && pct=88
        fi
        mb=$((total_have / 1048576))
        emb=$((total_exp / 1048576))
        if [ "$emb" -gt 0 ]; then
            xonotic_progress_write running "$pct" \
                "Downloading ${#names[@]} packs (${mb} / ${emb} MB, ${alive} active) — resumes if interrupted..."
        else
            xonotic_progress_write running "$pct" \
                "Downloading ${#names[@]} packs (${mb} MB, ${alive} active) — resumes if interrupted..."
        fi

        jobs=0
        for idx in "${!pids[@]}"; do
            pid="${pids[$idx]}"
            [ -n "$pid" ] && [ "$pid" != "0" ] || continue
            if kill -0 "$pid" 2>/dev/null; then
                jobs=$((jobs + 1))
            else
                xonotic_wait_download_pid "$pid" "${paths[$idx]}" "${expecteds[$idx]:-0}" \
                    || return 1
                pids[$idx]=0
                if [ "${installed[$idx]:-0}" != "1" ]; then
                    xonotic_install_autobuild_zip "$data_dir" "${paths[$idx]}" "${kinds[$idx]}" \
                        || return 1
                    installed[$idx]=1
                fi
            fi
        done

        [ "$alive" -eq 0 ] && [ "$next" -ge "${#names[@]}" ] && break
        sleep 1
    done

    for idx in "${!pids[@]}"; do
        pid="${pids[$idx]}"
        [ -n "$pid" ] && [ "$pid" != "0" ] || continue
        xonotic_wait_download_pid "$pid" "${paths[$idx]}" "${expecteds[$idx]:-0}" || return 1
        pids[$idx]=0
        if [ "${installed[$idx]:-0}" != "1" ]; then
            xonotic_install_autobuild_zip "$data_dir" "${paths[$idx]}" "${kinds[$idx]}" \
                || return 1
            installed[$idx]=1
        fi
    done

    # Leave any unrelated leftovers; never wipe partial zips on pause/error.
    rmdir "$tmp/extract" 2>/dev/null || true
}

_xonotic_asset_discover_lib() {
    local here
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    if [ -f "$here/asset-discover.sh" ]; then
        # shellcheck source=asset-discover.sh
        . "$here/asset-discover.sh"
    fi
}

xonotic_fetch_game_assets() {
    local data_dir="$1"

    mkdir -p "$data_dir"

    if xonotic_assets_are_ready "$data_dir"; then
        xonotic_progress_write done 100 "Game assets ready"
        return 0
    fi

    _xonotic_asset_discover_lib
    if declare -F xonotic_try_discover_assets >/dev/null 2>&1; then
        xonotic_try_discover_assets "$data_dir" || true
    fi
    if xonotic_assets_are_ready "$data_dir"; then
        xonotic_progress_write done 100 "Game assets ready"
        return 0
    fi

    # Autobuild zips first: predictable size + live progress. Git sparse clones
    # are opt-in — they hang for a long time on "Preparing download..." when
    # gitlab is slow/unreachable inside Flatpak.
    xonotic_progress_write running 5 "Starting download..."
    echo "xonotic-touch: downloading game assets (first launch may take several minutes)..."
    # Theme before the big zips so the open-menu track is available ASAP.
    xonotic_ensure_menu_theme_track "$data_dir" || true
    if xonotic_fetch_autobuild_assets "$data_dir" && xonotic_assets_are_ready "$data_dir"; then
        xonotic_assets_mark_ready "$data_dir"
        xonotic_progress_write done 100 "Download complete"
        return 0
    fi

    if [ "${XONOTIC_ALLOW_GIT_ASSET_FETCH:-0}" = "1" ] && xonotic_fetch_git_assets "$data_dir"; then
        xonotic_assets_mark_ready "$data_dir"
        xonotic_progress_write done 100 "Download complete"
        return 0
    fi

    xonotic_progress_write error 0 "Download failed — check network and retry"
    return 1
}
