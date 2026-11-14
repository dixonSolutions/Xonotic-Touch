#!/bin/bash
# Vercel's build of the browser version, run by Vercel's Git integration on
# every push to main (vercel.json). One build makes the whole site: engine,
# QuakeC, page, and the game data: the pinned Godnotic data releases,
# content-addressed by scripts/stage-web-data.sh. Per profile, the split set
# (godnotic-data-<profile>.json and its archive) when it is published, else
# what there was before it: the stock Xonotic pk3s and the slim ASTC set,
# split per map by the stager.
#
#   scripts/vercel-build.sh --ignore    Vercel's "ignored build step": exit 0
#                                       (skip) for anything but main and
#                                       work/web* (web loader previews)
#   scripts/vercel-build.sh --install   build tools + Emscripten SDK
#   scripts/vercel-build.sh             build into build/web
#
# Runs on Vercel's build image (Amazon Linux 2023). Locally it works in a
# container of that image; see docs/WEB.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EMSDK_VERSION="${EMSDK_VERSION:-7.0.0}"   # the version the web build was tested with
# .vercel/cache survives between Vercel builds; the SDK is the slow part.
EMSDK="${EMSDK:-$ROOT/.vercel/cache/emsdk}"
AUTOBUILD="https://github.com/dixonSolutions/Godnotic-data/releases/download/data-20261107"
DATA_ARCHIVE="Godnotic-data-20261107.zip"
# The slim phone set (ASTC, zstd, Opus): the "astc" profile, for browsers on
# GPUs with WEBGL_compressed_texture_astc. Pinned by hash, and the build
# fails when the asset cannot be fetched or is not the pinned one: a deploy
# without the profile would move every ASTC browser to the 1 GB set, and a
# changed asset would make every visitor download it again. "data-slim" is a
# tag that moves; this pins the immutable copy (data-slim-<date>-<n>).
# Move the pin on purpose when a new set is published.
SLIM_URL="${XT_SLIM_DATA_ARCHIVE:-https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim-20261107-1/Godnotic-data.zip}"
SLIM_SHA256="${XT_SLIM_DATA_SHA256:-4940b5a49c13a928d447b18c3cf430d0879dbbaf3bc27750ece001877d21dec0}"
# A prebuilt "web" repack of the stock set (zstd entries, re-encoded JPEGs,
# DXT kept: repack-phone-assets.py --profile slim --textures keep), for the
# "web" profile. Empty: the web profile is the stock pk3s as they are.
WEB_URL="${XT_WEB_DATA_ARCHIVE:-}"
WEB_SHA256="${XT_WEB_DATA_SHA256:-}"
# The split set (docs/DATA_FETCH.md): godnotic-data-astc.json and
# godnotic-data-dxt.json beside the apps' archive on the moving release,
# each naming one stored zip on an immutable release ("archive": url, size,
# sha256). A profile whose manifest is not there (HTTP 404) is built as
# before, from the archives above. One that is there is used, checked
# against its own size and sha256, and against the set pinned here: a set
# that changes makes every visitor download it again, so the build fails
# until the pin is moved on purpose (an empty pin takes whatever is there).
# "unpublished": no split set is published yet, and none is taken until the
# pins name the set ids of the published manifests (the "set" of
# godnotic-data-astc.json and -dxt.json in the publishing run's artifacts).
# Until then a published manifest stops the build and the previous deployment
# stays up.
SPLIT_BASE="${XT_SPLIT_DATA_BASE:-https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim}"
SPLIT_SET_ASTC="${XT_SPLIT_SET_ASTC-unpublished}"
SPLIT_SET_DXT="${XT_SPLIT_SET_DXT-unpublished}"

# The QuakeC sources use #elifdef, which "cc -E" (GCC 11 on Amazon Linux
# 2023) rejects; GCC 12+ takes it. Prints a directory to put first in PATH
# whose "cc" can, or nothing when the system one already does.
cc_ok() { printf '#if 1\n#elifdef X\n#endif\n' | "$1" -xc -E -Werror - >/dev/null 2>&1; }
cc_dir() {
    cc_ok cc && return 0
    for c in gcc15-cc gcc14-cc gcc13-cc gcc12-cc; do
        if command -v "$c" >/dev/null 2>&1 && cc_ok "$c"; then
            mkdir -p "$ROOT/.vercel/bin"
            ln -sf "$(command -v "$c")" "$ROOT/.vercel/bin/cc"
            echo "$ROOT/.vercel/bin"
            return 0
        fi
    done
    return 1
}

# The SDK needs Python 3.10+; Amazon Linux 2023's python3 is 3.9.
find_python() {
    for py in python3.13 python3.12 python3.11 python3.10 python3; do
        if command -v "$py" >/dev/null 2>&1 &&
            "$py" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
            command -v "$py"
            return 0
        fi
    done
    return 1
}

case "${1:-}" in
--ignore)
    # Vercel builds every branch unless told otherwise. Only main publishes;
    # work/web* branches get a preview deployment to test the loader on.
    case "${VERCEL_GIT_COMMIT_REF:-main}" in
        main|work/web*)
            echo "${VERCEL_GIT_COMMIT_REF:-main}: building"
            exit 1
            ;;
    esac
    echo "${VERCEL_GIT_COMMIT_REF}: not main, skipping"
    exit 0
    ;;
--install)
    need=()
    for tool in gcc g++ make zip unzip python3 git xz bzip2 tar curl which zstd; do
        command -v "$tool" >/dev/null 2>&1 || need+=("$tool")
    done
    if [ "${#need[@]}" -gt 0 ]; then
        echo "Installing: ${need[*]}"
        pkgs=()
        for tool in "${need[@]}"; do
            case "$tool" in
                g++) pkgs+=(gcc-c++) ;;
                *) pkgs+=("$tool") ;;
            esac
        done
        dnf install -y -q "${pkgs[@]}"
    fi
    if ! find_python >/dev/null; then
        echo "Installing: python3.11"
        dnf install -y -q python3.11
    fi
    if ! cc_dir >/dev/null; then
        echo "Installing: gcc14 (the QuakeC preprocessor needs GCC 12+)"
        dnf install -y -q gcc14
    fi
    EMSDK_PYTHON="$(find_python)"
    export EMSDK_PYTHON
    if ! grep -qx "$EMSDK_VERSION" "$EMSDK/.xt-version" 2>/dev/null; then
        rm -rf "$EMSDK"
        mkdir -p "$(dirname "$EMSDK")"
        git clone -q --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK"
        "$EMSDK/emsdk" install "$EMSDK_VERSION" >/dev/null
        "$EMSDK/emsdk" activate "$EMSDK_VERSION" >/dev/null
        echo "$EMSDK_VERSION" > "$EMSDK/.xt-version"
    fi
    echo "Emscripten $EMSDK_VERSION ready in $EMSDK"
    exit 0
    ;;
"") ;;
*) sed -n '2,13p' "$0" >&2; exit 2 ;;
esac

EMSDK_PYTHON="$(find_python)" || { echo "vercel-build: no Python 3.10+; run --install first" >&2; exit 1; }
ccdir="$(cc_dir)" || { echo "vercel-build: no C preprocessor with #elifdef (GCC 12+); run --install first" >&2; exit 1; }
# emsdk_env.sh drops EMSDK_PYTHON, and Emscripten's launchers then take the
# first python3 on PATH: make that one 3.10+.
mkdir -p "$ROOT/.vercel/bin"
ln -sf "$EMSDK_PYTHON" "$ROOT/.vercel/bin/python3"
PATH="$ROOT/.vercel/bin:$PATH"
[ -z "$ccdir" ] || PATH="$ccdir:$PATH"
PYTHON="$EMSDK_PYTHON"
export EMSDK EMSDK_PYTHON PYTHON PATH
XT_WEB_DEPS="$ROOT/.vercel/cache/web-deps" QCCFLAGS_WATERMARK="web-${VERCEL_GIT_COMMIT_SHA:0:7}" EMSDK="$EMSDK" bash "$ROOT/scripts/build-web.sh"

work="$(mktemp -d "${TMPDIR:-/tmp}/xt-vercel.XXXXXX")"
trap 'rm -rf "$work"' EXIT
echo "Fetching the pinned Godnotic data…"
curl -sfL --retry 3 --retry-delay 5 -o "$work/Xonotic-latest.zip" \
    "$AUTOBUILD/$DATA_ARCHIVE"
mkdir -p "$work/stock"
unzip -q -j -o "$work/Xonotic-latest.zip" 'Xonotic/data/*.pk3' -d "$work/stock"
rm -f "$work/Xonotic-latest.zip"
stage_args=(--stock "$work/stock")
fetch_set() { # URL SHA256 NAME -> the archive's path; the build fails without it
    if ! curl -sfL --retry 3 --retry-delay 5 -o "$work/$3.zip" "$1"; then
        echo "vercel-build: could not fetch $1 (the $3 profile); not deploying without it" >&2
        return 1
    fi
    if [ -z "$2" ]; then
        echo "vercel-build: no sha256 pinned for $1 (the $3 profile)" >&2
        return 1
    fi
    if ! echo "$2  $work/$3.zip" | sha256sum -c --quiet - >/dev/null 2>&1; then
        echo "vercel-build: $1 is not the pinned build: expected sha256 $2, got $(sha256sum "$work/$3.zip" | cut -d' ' -f1)" >&2
        echo "vercel-build: move the pin in scripts/vercel-build.sh on purpose if the data was meant to change" >&2
        return 1
    fi
    echo "$work/$3.zip"
}
# The split set of one profile: fetches $work/split-<profile>.json and
# $work/split-<profile>.zip. 0: there and right. 1: not published (the
# caller builds that profile the old way). Anything wrong with a published
# one stops the build.
fetch_split() { # PROFILE PINNED_SET
    local m="$work/split-$1.json" code got
    code="$(curl -sL --retry 3 --retry-delay 5 -o "$m" -w '%{http_code}' "$SPLIT_BASE/godnotic-data-$1.json" || true)"
    if [ "$code" = 404 ]; then
        rm -f "$m"
        return 1
    fi
    if [ "$code" != 200 ]; then
        echo "vercel-build: $SPLIT_BASE/godnotic-data-$1.json: HTTP $code; not deploying without knowing whether the split set is published" >&2
        exit 1
    fi
    # set, archive url, size, sha256: one per line (the stager checks the rest).
    got="$("$EMSDK_PYTHON" -I - "$m" <<'PY'
import json, re, sys
m = json.load(open(sys.argv[1]))
a = m.get('archive') or {}
ok = (m.get('format') == 1 and isinstance(m.get('set'), str) and m['set'] and re.match(r'https?://', a.get('url') or '')
      and isinstance(a.get('size'), int) and a['size'] > 0 and re.fullmatch(r'[0-9a-f]{64}', a.get('sha256') or ''))
if not ok:
    sys.exit('not a format 1 data manifest with an archive url, size and sha256')
print(m['set'], a['url'], a['size'], a['sha256'], sep='\n')
PY
    )" || { echo "vercel-build: godnotic-data-$1.json is published but unusable" >&2; exit 1; }
    local set url size sha
    { read -r set; read -r url; read -r size; read -r sha; } <<<"$got"
    if [ -n "$2" ] && [ "$set" != "$2" ]; then
        echo "vercel-build: the published $1 split set is $set, the build is pinned to $2" >&2
        echo "vercel-build: move the pin in scripts/vercel-build.sh on purpose if the data was meant to change" >&2
        exit 1
    fi
    if ! curl -sfL --retry 3 --retry-delay 5 -o "$work/split-$1.zip" "$url"; then
        echo "vercel-build: could not fetch $url (the $1 split set $set)" >&2
        exit 1
    fi
    if [ "$(stat -c %s "$work/split-$1.zip")" != "$size" ] || ! echo "$sha  $work/split-$1.zip" | sha256sum -c --quiet - >/dev/null 2>&1; then
        echo "vercel-build: $url is not the archive godnotic-data-$1.json lists (size $size, sha256 $sha)" >&2
        exit 1
    fi
    echo "The $1 split set $set"
}
if fetch_split astc "$SPLIT_SET_ASTC"; then
    stage_args+=(--astc "$work/split-astc.zip" --astc-manifest "$work/split-astc.json")
else
    echo "Fetching the slim (ASTC) data…"
    slim="$(fetch_set "$SLIM_URL" "$SLIM_SHA256" astc)"
    stage_args+=(--astc "$slim")
fi
if fetch_split dxt "$SPLIT_SET_DXT"; then
    stage_args+=(--web "$work/split-dxt.zip" --web-manifest "$work/split-dxt.json")
elif [ -n "$WEB_URL" ]; then
    echo "Fetching the web repack…"
    webset="$(fetch_set "$WEB_URL" "$WEB_SHA256" web)"
    stage_args+=(--web "$webset")
fi
PYTHON="$EMSDK_PYTHON" bash "$ROOT/scripts/stage-web-data.sh" "${stage_args[@]}"
rm -f "$ROOT/build/web/serve.py"
du -sh "$ROOT/build/web"
