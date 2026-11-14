#!/bin/bash
# Build Godnotic for the browser (WebAssembly + WebGL 2).
#
#   scripts/build-web.sh                 engine + QuakeC + page, into build/web
#   scripts/build-web.sh --engine-only   just relink the engine
#   scripts/build-web.sh --serve         build, then serve build/web on :8080
#   scripts/build-web.sh --profile       release build that keeps function names,
#                                        for browser CPU profiles
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
PROFILE="${XT_WEB_PROFILE:-0}"

for arg in "$@"; do
    case "$arg" in
        --engine-only) ENGINE_ONLY=1 ;;
        --serve) SERVE=1 ;;
        --debug) DEBUG=1 ;;
        --profile) PROFILE=1 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
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

# ---- Opus (the slim data's music and sounds) ------------------------------
# libopus + libopusfile, built with emcc once and linked statically: the
# engine's Opus loader (touch_assets_opus.c) loads libopusfile with dlopen
# elsewhere, which WebAssembly does not have. Kept in XT_WEB_DEPS between
# builds (Vercel: .vercel/cache).
OPUS_VERSION=1.5.2
OPUS_SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
OPUSFILE_VERSION=0.12
OPUSFILE_SHA256=118d8601c12dd6a44f52423e68ca9083cc9f2bfe72da7a8c1acb22a80ae3550b
DEPS="${XT_WEB_DEPS:-$ROOT/build/web-deps}"
OPUS_PREFIX="$DEPS/opus-$OPUS_VERSION-opusfile-$OPUSFILE_VERSION"
web_fetch() { # URL SHA256 DIR: download and unpack a pinned source tarball
    local tgz="$3.tar.gz"
    curl -sfL --retry 3 -o "$tgz" "$1"
    echo "$2  $tgz" | sha256sum -c --quiet - || { echo "build-web: $1 does not match its pinned hash" >&2; exit 1; }
    mkdir -p "$3"
    tar -xzf "$tgz" -C "$3" --strip-components=1
    rm -f "$tgz"
}
if [ ! -f "$OPUS_PREFIX/lib/libopusfile.a" ]; then
    echo "Building libopus $OPUS_VERSION and opusfile $OPUSFILE_VERSION with emcc…"
    rm -rf "$OPUS_PREFIX" "$DEPS/src-opus" "$DEPS/src-opusfile"
    mkdir -p "$DEPS" "$OPUS_PREFIX/lib" "$OPUS_PREFIX/include/opus"
    web_fetch "https://github.com/xiph/opus/releases/download/v$OPUS_VERSION/opus-$OPUS_VERSION.tar.gz" "$OPUS_SHA256" "$DEPS/src-opus"
    web_fetch "https://github.com/xiph/opusfile/releases/download/v$OPUSFILE_VERSION/opusfile-$OPUSFILE_VERSION.tar.gz" "$OPUSFILE_SHA256" "$DEPS/src-opusfile"
    (cd "$DEPS/src-opus" && emconfigure ./configure -q --prefix="$OPUS_PREFIX" --host=wasm32-unknown-emscripten \
        --disable-shared --enable-static --disable-doc --disable-extra-programs --disable-asm --disable-rtcd \
        --disable-intrinsics --disable-stack-protector --disable-hardening --disable-dred --disable-deep-plc \
        CFLAGS="-O2" >/dev/null && emmake make -s -j"$(nproc)" install >/dev/null)
    # opusfile: four sources and no HTTP, so no configure (it wants pkg-config).
    of="$DEPS/src-opusfile"
    for s in info internal opusfile stream; do
        emcc -O2 -sUSE_OGG=1 -DOP_DISABLE_HTTP -DOP_HAVE_LRINTF -I"$of/include" -I"$OPUS_PREFIX/include/opus" \
            -c "$of/src/$s.c" -o "$of/$s.o"
    done
    emar rcs "$OPUS_PREFIX/lib/libopusfile.a" "$of"/info.o "$of"/internal.o "$of"/opusfile.o "$of"/stream.o
    cp "$of/include/opusfile.h" "$OPUS_PREFIX/include/opus/"
    rm -rf "$DEPS/src-opus" "$DEPS/src-opusfile"
fi
# Headers for touch_assets_opus.c (XT_WEB_OPUSFILE: linked, not dlopen'ed).
export EMCC_CFLAGS="${EMCC_CFLAGS:-} -DXT_WEB_OPUSFILE -I$OPUS_PREFIX/include -I$OPUS_PREFIX/include/opus"

# ---- Engine -------------------------------------------------------------
# Link flags. Notes:
# - WebGL 2 only; the GLES2 renderer runs on it. FULL_ES3 emulates the
#   client-side vertex arrays the renderer still uses in places.
# - GL_ENABLE_GET_PROC_ADDRESS: SDL_GL_GetProcAddress resolves GL entry points.
# - Memory grows on demand up to 4 GB; pk3 data stays in the browser's
#   storage and is read on demand by the page (web/lazyfs.js), or copied into
#   the JS heap (MEMFS) where that cannot work, never into wasm memory.
# - The page (web/shell.html + web/shell.js) starts the engine itself after
#   the data is in place, hence INVOKE_RUN=0.
LDFLAGS_WEB=(
    -lm
    -sUSE_SDL=2 -sUSE_LIBPNG=1 -sUSE_LIBJPEG=1 -sUSE_ZLIB=1 -sUSE_OGG=1 -sUSE_VORBIS=1
    -sUSE_FREETYPE=1
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sFULL_ES3=1
    -sGL_ENABLE_GET_PROC_ADDRESS=1
    -sINITIAL_MEMORY=256mb -sMAXIMUM_MEMORY=4gb -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=8mb
    -sENVIRONMENT=web
    -sINVOKE_RUN=0 -sEXIT_RUNTIME=0
    -sFORCE_FILESYSTEM=1 -lidbfs.js
    "-sEXPORTED_RUNTIME_METHODS=callMain,FS,IDBFS,ccall,UTF8ToString,stringToNewUTF8"
    "$OPUS_PREFIX/lib/libopusfile.a" "$OPUS_PREFIX/lib/libopus.a"
)
if [ "$DEBUG" = "1" ]; then
    LDFLAGS_WEB+=(-sASSERTIONS=1 -g2)
else
    LDFLAGS_WEB+=(-sASSERTIONS=0)
fi
# Same optimisation as a release, plus the wasm name section, so profiles
# show function names for the code that ships.
if [ "$PROFILE" = "1" ]; then
    LDFLAGS_WEB+=(--profiling-funcs)
fi

cd "$ROOT/engine/darkplaces"
# CFLAGS changes (debug vs release) need a clean object dir.
stamp="build-obj/release/godnotic.js/.web-flags"
want="debug=$DEBUG opus=$OPUS_VERSION/$OPUSFILE_VERSION"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" != "$want" ]; then
    rm -rf build-obj/release/godnotic.js
fi
# Link flags (--profile) only need a relink, which make does not see.
link_stamp="build-obj/release/godnotic.js/.web-link"
want_link="${LDFLAGS_WEB[*]}"
if [ ! -f "$link_stamp" ] || [ "$(cat "$link_stamp")" != "$want_link" ]; then
    rm -f godnotic.js godnotic.wasm
fi
emmake make emscripten-release -j"$(nproc)" \
    EXE_WASMJS=godnotic.js \
    DP_VIDEO_CAPTURE=disabled \
    STRIP=: \
    LDFLAGS_WASMJS="${LDFLAGS_WEB[*]}"
mkdir -p "$(dirname "$stamp")"
echo "$want" > "$stamp"
echo "$want_link" > "$link_stamp"

# ---- Page ---------------------------------------------------------------
# Everything the page loads by name gets its content hash in the name, under
# a/ (served immutable, docs/WEB.md), and index.html, the one file that is
# always revalidated, names them: no build.json round trip, no ?v= busting,
# and an unchanged engine stays in the browser's cache across deploys.
mkdir -p "$OUT"
rm -rf "$OUT/a"
mkdir -p "$OUT/a"
install -m 644 "$ROOT/web/manifest.webmanifest" "$ROOT/web/serve.py" "$OUT/"
# Digital Asset Links: lets Android open the installed Godnotic app for this
# site's links (docs/WEB.md, "An existing install"). Only works on a host
# that serves it from its root (Vercel; not a GitHub Pages project path).
install -D -m 644 "$ROOT/web/.well-known/assetlinks.json" "$OUT/.well-known/assetlinks.json"
for icon in "$ROOT"/web/icon-*.png; do
    [ -f "$icon" ] && install -m 644 "$icon" "$OUT/"
done
rm -f "$OUT/config.js" "$OUT/build.json" "$OUT/godnotic.js" "$OUT/godnotic.wasm" \
    "$OUT/shell.js" "$OUT/shell.css" "$OUT/store.js" "$OUT/lazyfs.js" "$OUT/appdata.js" "$OUT/godnotic-banner.png" "$OUT/godnotic-banner.webp"
rm -f "$OUT/mods.js"
rev="${VERCEL_GIT_COMMIT_SHA:-${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo local)}}"
XT_BUILD_ID="$(date -u +%Y%m%d%H%M%S)-${rev:0:7}" XT_WEB_DATA_URL="${XT_WEB_DATA_URL:-}" \
python3 -I - "$ROOT/web" "$PWD" "$OUT" <<'PY'
import hashlib, json, os, re, shutil, sys
web, engine, out = sys.argv[1:4]

def hashed(src, name):
    data = open(src, 'rb').read()
    stem, ext = name.rsplit('.', 1)
    dst = f'a/{stem}.{hashlib.sha256(data).hexdigest()[:12]}.{ext}'
    with open(os.path.join(out, dst), 'wb') as f:
        f.write(data)
    return dst

names = {
    'godnotic.js': hashed(os.path.join(engine, 'godnotic.js'), 'godnotic.js'),
    'godnotic.wasm': hashed(os.path.join(engine, 'godnotic.wasm'), 'godnotic.wasm'),
}
for f in ('shell.js', 'shell.css', 'store.js', 'lazyfs.js', 'appdata.js', 'godnotic-banner.webp'):
    names[f] = hashed(os.path.join(web, f), f)

names['mods.js'] = hashed(os.path.join(web, 'mods.js'), 'mods.js')  # Lua mods from a file

html = open(os.path.join(web, 'index.html'), encoding='utf-8').read()
html, n = re.subn(r'(src=")mods\.js"', r'\g<1>' + names['mods.js'] + '"', html)
assert n == 1, 'mods.js'
for plain in ('shell.js', 'shell.css', 'store.js', 'lazyfs.js', 'appdata.js', 'godnotic-banner.webp'):
    html, n = re.subn(r'((?:src|href)=")' + re.escape(plain) + '"', r'\g<1>' + names[plain] + '"', html)
    assert n == 1, plain
cfg = {'engine': names['godnotic.js'], 'wasm': names['godnotic.wasm'], 'manifest': '',
       'dataUrl': os.environ.get('XT_WEB_DATA_URL', ''), 'build': os.environ['XT_BUILD_ID']}
env = os.path.join(out, 'data', 'web-data.env')
if os.path.exists(env):
    vals = dict(l.split('=', 1) for l in open(env).read().splitlines() if '=' in l)
    cfg['manifest'] = vals.get('XT_DATA_MANIFEST', '')
    if 'XT_DATA_PROFILES' in vals:
        cfg['profiles'] = json.loads(vals['XT_DATA_PROFILES'])
pat = re.compile(r'(<script id="xt-config" type="application/json">)(.*?)(</script>)', re.S)
m = pat.search(html)
html = html[:m.start(2)] + json.dumps(cfg, separators=(',', ':')) + html[m.end(2):]
# Start the engine download while the page checks storage. crossorigin:
# Emscripten fetches the wasm in CORS mode, and a preload only matches a
# request with the same mode. Low priority: on a first visit the page's own
# small files (what draws it and asks for the manifests) go first; on a
# later one they are cached and the engine has the connection to itself.
preload = (f'<link rel="preload" href="{names["godnotic.wasm"]}" as="fetch" type="application/wasm" crossorigin fetchpriority="low">\n'
           f'<link rel="preload" href="{names["godnotic.js"]}" as="script" fetchpriority="low">')
assert '<!--xt:preload-->' in html
html = html.replace('<!--xt:preload-->', preload)
open(os.path.join(out, 'index.html'), 'w', encoding='utf-8').write(html)
PY

printf 'Built %s (%s wasm)\n' "$OUT" "$(du -sh godnotic.wasm | cut -f1)"
if [ ! -f "$OUT/data/web-data.env" ]; then
    echo "No game data staged yet: run scripts/stage-web-data.sh" >&2
fi

if [ "$SERVE" = "1" ]; then
    exec python3 "$ROOT/web/serve.py" "$OUT"
fi
