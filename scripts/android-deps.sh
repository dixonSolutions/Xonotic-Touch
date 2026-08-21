#!/bin/bash
# Build the native libraries darkplaces needs on Android, for one ABI.
#
# darkplaces normally dlopen()s its codecs, but sys.h forces LINK_TO_ZLIB,
# LINK_TO_LIBVORBIS and DP_FREETYPE_STATIC on __ANDROID__, and jpeg.c refuses to
# dlopen on Android at all. So everything except libpng is linked statically:
#
#   zlib        NDK sysroot (-lz)
#   ogg/vorbis  static  — LINK_TO_LIBVORBIS
#   freetype    static  — DP_FREETYPE_STATIC
#   libjpeg     static  — LINK_TO_LIBJPEG (set by our CMakeLists)
#   libpng      SHARED, installed as libpng.so so that image_png.c's existing
#               dlopen name list finds it inside the APK. Patching the engine to
#               link PNG statically would mean rewriting 30-odd function pointers.
#   SDL2        shared  — also supplies the Java glue the APK needs
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CACHE_DIR="${ANDROID_CACHE_DIR:-$ROOT/build/android}"
SRC_DIR="$CACHE_DIR/src"
ABI="${1:?usage: android-deps.sh <abi>   (arm64-v8a | armeabi-v7a)}"
PREFIX="$CACHE_DIR/prefix/$ABI"
API="${ANDROID_API_LEVEL:-21}"

SDL_VERSION="${SDL_VERSION:-2.32.10}"
OGG_VERSION="${OGG_VERSION:-1.3.6}"
VORBIS_VERSION="${VORBIS_VERSION:-1.3.7}"
FREETYPE_TAG="${FREETYPE_TAG:-VER-2-13-3}"
JPEG_VERSION="${JPEG_VERSION:-3.1.2}"
PNG_VERSION="${PNG_VERSION:-1.6.50}"

NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "Set ANDROID_NDK_ROOT to an Android NDK (r26+)." >&2
    exit 1
fi
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
test -f "$TOOLCHAIN" || { echo "Missing $TOOLCHAIN" >&2; exit 1; }

mkdir -p "$SRC_DIR" "$PREFIX"
JOBS="${JOBS:-$(nproc)}"

log() { printf '\n-- [%s] %s\n' "$ABI" "$*"; }

fetch() {
    # fetch <url> <dirname>  -> extracted tree at $SRC_DIR/<dirname>
    local url="$1" name="$2" archive
    archive="$SRC_DIR/$(basename "$url")"
    if [ ! -d "$SRC_DIR/$name" ]; then
        if [ ! -f "$archive" ]; then
            curl -fL --retry 3 --retry-delay 5 -o "$archive.part" "$url"
            mv -f "$archive.part" "$archive"
        fi
        rm -rf "$SRC_DIR/$name.tmp"
        mkdir -p "$SRC_DIR/$name.tmp"
        tar -xf "$archive" -C "$SRC_DIR/$name.tmp" --strip-components=1
        mv "$SRC_DIR/$name.tmp" "$SRC_DIR/$name"
    fi
}

cmake_build() {
    # cmake_build <srcdir> <builddir-suffix> [extra cmake args...]
    local src="$1" tag="$2"; shift 2
    local build="$CACHE_DIR/build/$ABI/$tag"
    cmake -S "$src" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" \
        -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "$@" >/dev/null
    cmake --build "$build" --parallel "$JOBS" >/dev/null
    cmake --install "$build" >/dev/null
}

##### SDL2 ####################################################################

if [ ! -f "$PREFIX/lib/libSDL2.so" ]; then
    log "SDL $SDL_VERSION"
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION"
    cmake_build "$SRC_DIR/SDL2-$SDL_VERSION" sdl2 \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
fi

##### ogg / vorbis ############################################################

if [ ! -f "$PREFIX/lib/libogg.a" ]; then
    log "libogg $OGG_VERSION"
    fetch "https://github.com/xiph/ogg/releases/download/v$OGG_VERSION/libogg-$OGG_VERSION.tar.gz" "libogg-$OGG_VERSION"
    cmake_build "$SRC_DIR/libogg-$OGG_VERSION" ogg -DBUILD_SHARED_LIBS=OFF
fi

if [ ! -f "$PREFIX/lib/libvorbisfile.a" ]; then
    log "libvorbis $VORBIS_VERSION"
    fetch "https://github.com/xiph/vorbis/releases/download/v$VORBIS_VERSION/libvorbis-$VORBIS_VERSION.tar.gz" "libvorbis-$VORBIS_VERSION"
    cmake_build "$SRC_DIR/libvorbis-$VORBIS_VERSION" vorbis -DBUILD_SHARED_LIBS=OFF
fi

##### freetype ################################################################

if [ ! -f "$PREFIX/lib/libfreetype.a" ]; then
    log "freetype $FREETYPE_TAG"
    fetch "https://github.com/freetype/freetype/archive/refs/tags/$FREETYPE_TAG.tar.gz" "freetype-$FREETYPE_TAG"
    # darkplaces only needs the rasteriser; skip the optional shaping stack so
    # the dependency tree stays flat.
    cmake_build "$SRC_DIR/freetype-$FREETYPE_TAG" freetype \
        -DBUILD_SHARED_LIBS=OFF \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON \
        -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
fi

##### libjpeg-turbo ###########################################################

if [ ! -f "$PREFIX/lib/libjpeg.a" ]; then
    log "libjpeg-turbo $JPEG_VERSION"
    fetch "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$JPEG_VERSION/libjpeg-turbo-$JPEG_VERSION.tar.gz" "libjpeg-turbo-$JPEG_VERSION"
    cmake_build "$SRC_DIR/libjpeg-turbo-$JPEG_VERSION" jpeg \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF
fi

##### libpng (shared, dlopen'd) ###############################################

if [ ! -f "$PREFIX/lib/libpng16.so" ]; then
    log "libpng $PNG_VERSION"
    fetch "https://github.com/pnggroup/libpng/archive/refs/tags/v$PNG_VERSION.tar.gz" "libpng-$PNG_VERSION"
    cmake_build "$SRC_DIR/libpng-$PNG_VERSION" png \
        -DPNG_SHARED=ON -DPNG_STATIC=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
        -DPNG_FRAMEWORK=OFF
    # CMake omits versioned sonames on Android, so this installs plain
    # libpng16.so — file name and DT_SONAME agree, which is what dlopen wants.
    # image_png.c's name list carries a matching entry.
    test -f "$PREFIX/lib/libpng16.so" || { echo "libpng build produced no shared object" >&2; ls -la "$PREFIX/lib" >&2; exit 1; }
fi

log "dependencies ready in $PREFIX"
ls -1 "$PREFIX/lib" | sed 's/^/   /'
