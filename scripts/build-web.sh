#!/bin/bash
# Build Xonotic Touch for the browser (WebAssembly + WebGL 2).
#
#   scripts/build-web.sh                 engine + QuakeC + page, into build/web
#   scripts/build-web.sh --engine-only   just relink the engine
#   scripts/build-web.sh --serve         build, then serve build/web on :8080
#
# Needs the Emscripten SDK. EMSDK points at it (default ~/.cache/emsdk); the
# script sources emsdk_env.sh itself. Game data is staged separately by
# scripts/stage-web-data.sh, because the stock pk3s are a 1.2 GB download
# that does not change between engine builds. See docs/WEB.md.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
OUT="${XT_WEB_OUT:-$ROOT/build/web}"
EMSDK="${EMSDK:-$HOME/.cache/emsdk}"
ENGINE_ONLY=0
SERVE=0
DEBUG="${XT_WEB_DEBUG:-0}"

for arg in "$@"; do
    case "$arg" in
        --engine-only) ENGINE_ONLY=1 ;;
        --serve) SERVE=1 ;;
        --debug) DEBUG=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "build-web: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v emcc >/dev/null 2>&1; then
    if [ -f "$EMSDK/emsdk_env.sh" ]; then
        # shellcheck disable=SC1091
        . "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
    fi
fi
command -v emcc >/dev/null 2>&1 || {
    cat >&2 <<EOF
emcc not found. Install the Emscripten SDK:
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK"
  "$EMSDK/emsdk" install latest && "$EMSDK/emsdk" activate latest
EOF
    exit 1
}

# ---- QuakeC (arch-neutral; the native gmqcc compiles it) -----------------
if [ "$ENGINE_ONLY" != "1" ]; then
    gmqcc="$ROOT/engine/gmqcc/gmqcc"
    if [ ! -x "$gmqcc" ]; then
        make -C "$ROOT/engine/gmqcc" -j"$(nproc)" STRIP=: gmqcc
    fi
    make -C "$ROOT/engine/data/xonotic-data.pk3dir" QCC="$gmqcc" XON_BUILDSYSTEM=1 \
        QCCFLAGS_WATERMARK="${QCCFLAGS_WATERMARK:-web}" -j"$(nproc)" qc
    make -C "$ROOT/engine/data/xonotic-data.pk3dir/qcsrc" QCC="$gmqcc" XON_BUILDSYSTEM=1 \
        QCCFLAGS_WATERMARK="${QCCFLAGS_WATERMARK:-web}" ../menu.dat
fi

# ---- Engine -------------------------------------------------------------
# Link flags. Notes:
# - WebGL 2 only; the GLES2 renderer runs on it. FULL_ES3 emulates the
#   client-side vertex arrays the renderer still uses in places.
# - GL_ENABLE_GET_PROC_ADDRESS: SDL_GL_GetProcAddress resolves GL entry points.
# - Memory grows on demand up to 4 GB; pk3 data lives in the JS heap (MEMFS),
#   not in wasm memory, so the wasm heap only holds what the engine loads.
# - The page (web/shell.html + web/shell.js) starts the engine itself after
#   the data is in place, hence INVOKE_RUN=0.
LDFLAGS_WEB=(
    -lm
    -sUSE_SDL=2 -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1 -sUSE_ZLIB=1 -sUSE_OGG=1 -sUSE_VORBIS=1
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1
    -sGL_ENABLE_GET_PROC_ADDRESS=1
    -sINITIAL_MEMORY=256mb -sMAXIMUM_MEMORY=4gb -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8mb
    -sENVIRONMENT=web
    -sINVOKE_RUN=0 -sEXIT_RUNTIME=0
    -sFORCE_FILESYSTEM=1 -lidbfs.js
    "-sEXPORTED_RUNTIME_METHODS=callMain,FS,IDBFS,ccall,UTF8ToString,stringToNewUTF8"
)
if [ "$DEBUG" = "1" ]; then
    LDFLAGS_WEB+=(-sASSERTIONS=1 -g2)
else
    LDFLAGS_WEB+=(-sASSERTIONS=0)
fi

cd "$ROOT/engine/darkplaces"
# CFLAGS changes (debug vs release) need a clean object dir.
stamp="build-obj/release/xonotic-touch.js/.web-flags"
want="debug=$DEBUG"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" != "$want" ]; then
    rm -rf build-obj/release/xonotic-touch.js
fi
emmake make emscripten-release -j"$(nproc)" \
    EXE_WASMJS=xonotic-touch.js \
    DP_VIDEO_CAPTURE=disabled \
    STRIP=: \
    LDFLAGS_WASMJS="${LDFLAGS_WEB[*]}"
mkdir -p "$(dirname "$stamp")"
echo "$want" > "$stamp"

# ---- Page ---------------------------------------------------------------
mkdir -p "$OUT"
install -m 644 xonotic-touch.js xonotic-touch.wasm "$OUT/"
install -m 644 "$ROOT/web/index.html" "$ROOT/web/shell.js" "$ROOT/web/shell.css" \
    "$ROOT/web/manifest.webmanifest" "$ROOT/web/serve.py" "$ROOT/web/vercel.json" "$OUT/"
for icon in "$ROOT"/web/icon-*.png; do
    [ -f "$icon" ] && install -m 644 "$icon" "$OUT/"
done
# Site config: XT_WEB_DATA_URL points the page at game data hosted elsewhere
# (the GitHub Pages copy uses the data deployed with the Vercel copy).
printf 'window.XT_CONFIG = %s;\n' "$(python3 -c 'import json,os; print(json.dumps({k: v for k, v in {"dataUrl": os.environ.get("XT_WEB_DATA_URL", "")}.items() if v}))')" > "$OUT/config.js"
# The page uses this to tell builds apart (cache-busting the engine).
build_id="$(date -u +%Y%m%d%H%M%S)-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo local)"
printf '{"build":"%s"}\n' "$build_id" > "$OUT/build.json"

printf 'Built %s (%s)\n' "$OUT" "$(du -sh "$OUT/xonotic-touch.wasm" | cut -f1) wasm)"
if [ ! -f "$OUT/data/manifest.json" ]; then
    echo "No game data staged yet: run scripts/stage-web-data.sh" >&2
fi

if [ "$SERVE" = "1" ]; then
    exec python3 "$ROOT/web/serve.py" "$OUT"
fi
