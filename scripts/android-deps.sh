#!/bin/bash
# Build the native libraries darkplaces needs on Android, for one ABI.
#
# darkplaces normally dlopen()s its codecs, but sys.h forces LINK_TO_ZLIB,
# LINK_TO_LIBVORBIS and DP_FREETYPE_STATIC on __ANDROID__, and jpeg.c refuses to
# dlopen on Android at all. So everything except libpng and libcurl is linked
# statically:
#
#   zlib        NDK sysroot (-lz)
#   ogg/vorbis  static  — LINK_TO_LIBVORBIS
#   opus/opusfile static — LINK_TO_LIBOPUSFILE (Opus music of the phone packs,
#               engine/darkplaces/touch_assets_opus.c)
#   freetype    static  — DP_FREETYPE_STATIC
#   libjpeg     static  — LINK_TO_LIBJPEG (set by our CMakeLists)
#   libpng      SHARED, installed as libpng.so so that image_png.c's existing
#               dlopen name list finds it inside the APK. Patching the engine to
#               link PNG statically would mean rewriting 30-odd function pointers.
#   libcurl     SHARED, dlopen()ed by libcurl.c for server map downloads, with
#               mbedTLS linked into it for https
#   SDL2        shared  — also supplies the Java glue the APK needs
#   d0_blind_id SHARED, libd0_blind_id.so and libd0_rijndael.so, dlopen()ed by
#               crypto.c (player IDs, stats, encrypted connections), built
#               from engine/d0_blind_id with GMP linked statically into it
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
CACHE_DIR="${ANDROID_CACHE_DIR:-$ROOT/build/android}"
SRC_DIR="$CACHE_DIR/src"
ABI="${1:?usage: android-deps.sh <abi>   (arm64-v8a | armeabi-v7a | x86_64 | x86)}"
PREFIX="$CACHE_DIR/prefix/$ABI"
API="${ANDROID_API_LEVEL:-21}"

SDL_VERSION="${SDL_VERSION:-2.32.10}"
OGG_VERSION="${OGG_VERSION:-1.3.6}"
VORBIS_VERSION="${VORBIS_VERSION:-1.3.7}"
OPUS_VERSION="${OPUS_VERSION:-1.5.2}"
OPUSFILE_VERSION="${OPUSFILE_VERSION:-0.12}"
FREETYPE_TAG="${FREETYPE_TAG:-VER-2-13-3}"
JPEG_VERSION="${JPEG_VERSION:-3.1.2}"
PNG_VERSION="${PNG_VERSION:-1.6.50}"
MBEDTLS_VERSION="${MBEDTLS_VERSION:-3.6.7}"
CURL_VERSION="${CURL_VERSION:-8.22.0}"
GMP_VERSION="${GMP_VERSION:-6.3.0}"
GMP_SHA256="${GMP_SHA256:-a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898}"

NDK="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"
if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
    echo "Set ANDROID_NDK_ROOT to an Android NDK (r26+)." >&2
    exit 1
fi
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"
test -f "$TOOLCHAIN" || { echo "Missing $TOOLCHAIN" >&2; exit 1; }

mkdir -p "$SRC_DIR" "$PREFIX"
JOBS="${JOBS:-$(nproc)}"

# A restored cache predating 16 KiB support must not silently ship 4 KiB .so
# files. Only the shared libraries need relinking; static archives are reusable.
PAGE_SIZE_STAMP="$PREFIX/.shared-16k-v1"
case "$ABI" in
    arm64-v8a|x86_64)
        if [ ! -f "$PAGE_SIZE_STAMP" ]; then
            rm -f "$PREFIX/lib/libSDL2.so" "$PREFIX/lib/libpng16.so" "$PREFIX/lib/libcurl.so" \
                "$PREFIX/lib/libd0_blind_id.so" "$PREFIX/lib/libd0_rijndael.so"
        fi
        ;;
esac

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
        -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        "$@" >/dev/null
    cmake --build "$build" --parallel "$JOBS" >/dev/null
    cmake --install "$build" >/dev/null
}

##### SDL2 ####################################################################

# The source tree is needed even when the library is already built:
# android-build.sh copies SDL's Java glue out of it. CI caches the prefix and
# the sources separately, so a restored prefix can come without the sources.
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz" "SDL2-$SDL_VERSION"
if [ ! -f "$PREFIX/lib/libSDL2.so" ]; then
    log "SDL $SDL_VERSION"
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

##### opus / opusfile ########################################################

if [ ! -f "$PREFIX/lib/libopus.a" ]; then
    log "libopus $OPUS_VERSION"
    fetch "https://github.com/xiph/opus/releases/download/v$OPUS_VERSION/opus-$OPUS_VERSION.tar.gz" "opus-$OPUS_VERSION"
    cmake_build "$SRC_DIR/opus-$OPUS_VERSION" opus -DBUILD_SHARED_LIBS=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_DRED=OFF -DOPUS_DEEP_PLC=OFF
fi

if [ ! -f "$PREFIX/lib/libopusfile.a" ]; then
    log "opusfile $OPUSFILE_VERSION"
    fetch "https://github.com/xiph/opusfile/releases/download/v$OPUSFILE_VERSION/opusfile-$OPUSFILE_VERSION.tar.gz" "opusfile-$OPUSFILE_VERSION"
    # the release tarball only has autotools; four sources need no more than this
    cat > "$SRC_DIR/opusfile-$OPUSFILE_VERSION/CMakeLists.txt" <<'EOF_CMAKE'
cmake_minimum_required(VERSION 3.10)
project(opusfile C)
find_path(OGG_INCLUDE ogg/ogg.h REQUIRED)
find_path(OPUS_INCLUDE opus/opus.h REQUIRED)
add_library(opusfile STATIC src/info.c src/internal.c src/opusfile.c src/stream.c)
target_include_directories(opusfile PRIVATE include ${OGG_INCLUDE} ${OPUS_INCLUDE}/opus)
# 32-bit bionic before API 24 has no fseeko with 64-bit off_t; only the
# memory reader is used anyway
target_compile_definitions(opusfile PRIVATE OP_DISABLE_HTTP OP_HAVE_LRINTF _FILE_OFFSET_BITS=32)
install(TARGETS opusfile ARCHIVE DESTINATION lib)
install(FILES include/opusfile.h DESTINATION include/opus)
EOF_CMAKE
    cmake_build "$SRC_DIR/opusfile-$OPUSFILE_VERSION" opusfile
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

##### mbedTLS + libcurl (shared, dlopen'd) ####################################

# libcurl.c dlopen()s libcurl the way image_png.c does libpng, and Android has
# no system copy to find. Without it a server's map pk3 can never be fetched
# (sv_curl_defaulturl is the only way Xonotic servers hand out maps), so any
# server running a map outside the stock packs leaves the world black.
#
# TLS comes from mbedTLS, linked statically into libcurl.so: most map mirrors
# are https. The CA store is Android's own -- libcurl.c points CURLOPT_CAPATH
# at it at runtime, so there is no bundle here to go stale.
if [ ! -f "$PREFIX/lib/libmbedtls.a" ]; then
    log "mbedTLS $MBEDTLS_VERSION"
    fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2" "mbedtls-$MBEDTLS_VERSION"
    cmake_build "$SRC_DIR/mbedtls-$MBEDTLS_VERSION" mbedtls \
        -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DGEN_FILES=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
fi

if [ ! -f "$PREFIX/lib/libcurl.so" ]; then
    log "libcurl $CURL_VERSION"
    fetch "https://curl.se/download/curl-$CURL_VERSION.tar.xz" "curl-$CURL_VERSION"
    # HTTP(S) only: that is all sv_curl_defaulturl and the engine's other
    # curl users (server-side stats, pic downloads) ever ask for.
    cmake_build "$SRC_DIR/curl-$CURL_VERSION" curl \
        -DBUILD_SHARED_LIBS=ON -DBUILD_STATIC_LIBS=OFF \
        -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF \
        -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DCURL_USE_PKGCONFIG=OFF \
        -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DMBEDTLS_INCLUDE_DIR="$PREFIX/include" \
        -DMBEDTLS_LIBRARY="$PREFIX/lib/libmbedtls.a" \
        -DMBEDX509_LIBRARY="$PREFIX/lib/libmbedx509.a" \
        -DMBEDCRYPTO_LIBRARY="$PREFIX/lib/libmbedcrypto.a" \
        -DHTTP_ONLY=ON -DCURL_DISABLE_LDAP=ON -DCURL_USE_LIBPSL=OFF \
        -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF -DUSE_NGHTTP2=OFF \
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ZLIB=ON \
        -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DCURL_CA_FALLBACK=OFF \
        -DENABLE_THREADED_RESOLVER=ON
    test -f "$PREFIX/lib/libcurl.so" || { echo "curl build produced no shared object" >&2; ls -la "$PREFIX/lib" >&2; exit 1; }
fi

##### GMP + d0_blind_id (shared, dlopen'd) ##################################

# crypto.c dlopen()s libd0_blind_id.so (and libd0_rijndael.so for encrypted
# connections) the way image_png.c does libpng. Without them the engine has
# no player ID: no stats (XonStat), no server-side authentication, no
# crypto_* commands. The library is the copy in engine/d0_blind_id, built
# here with a small CMake file of our own (its build is autotools); its big
# numbers come from GMP, linked statically and with its symbols hidden.
# GMP is portable C here (--disable-assembly): the library only does a few
# modular exponentiations per connection and one key generation.
if [ ! -f "$PREFIX/lib/libgmp.a" ]; then
    log "GMP $GMP_VERSION"
    command -v m4 >/dev/null || { echo "GMP's configure needs m4 (apt install m4)" >&2; exit 1; }
    gmp_archive="$SRC_DIR/gmp-$GMP_VERSION.tar.xz"
    if [ ! -d "$SRC_DIR/gmp-$GMP_VERSION" ] && [ ! -f "$gmp_archive" ]; then
        curl -fL --retry 3 --retry-delay 5 -o "$gmp_archive.part" "https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VERSION.tar.xz" \
            || curl -fL --retry 3 --retry-delay 5 -o "$gmp_archive.part" "https://gmplib.org/download/gmp/gmp-$GMP_VERSION.tar.xz"
        echo "$GMP_SHA256  $gmp_archive.part" | sha256sum -c - >/dev/null || { echo "gmp-$GMP_VERSION.tar.xz: checksum mismatch" >&2; rm -f "$gmp_archive.part"; exit 1; }
        mv -f "$gmp_archive.part" "$gmp_archive"
    fi
    fetch "https://ftp.gnu.org/gnu/gmp/gmp-$GMP_VERSION.tar.xz" "gmp-$GMP_VERSION"
    case "$ABI" in
        arm64-v8a)   GMP_HOST=aarch64-linux-android ;;
        armeabi-v7a) GMP_HOST=armv7a-linux-androideabi ;;
        x86_64)      GMP_HOST=x86_64-linux-android ;;
        x86)         GMP_HOST=i686-linux-android ;;
    esac
    NDK_BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
    gmp_build="$CACHE_DIR/build/$ABI/gmp"
    rm -rf "$gmp_build"
    mkdir -p "$gmp_build"
    (
        cd "$gmp_build"
        CC="$NDK_BIN/$GMP_HOST$API-clang" AR="$NDK_BIN/llvm-ar" RANLIB="$NDK_BIN/llvm-ranlib" \
        NM="$NDK_BIN/llvm-nm" STRIP="$NDK_BIN/llvm-strip" CFLAGS="-O2 -fPIC" \
            "$SRC_DIR/gmp-$GMP_VERSION/configure" --host="$GMP_HOST" --prefix="$PREFIX" \
            --enable-static --disable-shared --with-pic --disable-assembly --disable-cxx >/dev/null
        make -j"$JOBS" >/dev/null
        make install >/dev/null
    )
    test -f "$PREFIX/lib/libgmp.a" || { echo "GMP build produced no libgmp.a" >&2; exit 1; }
fi

if [ ! -f "$PREFIX/lib/libd0_blind_id.so" ] || [ ! -f "$PREFIX/lib/libd0_rijndael.so" ]; then
    log "d0_blind_id (engine/d0_blind_id)"
    D0_CMAKE="$CACHE_DIR/build/$ABI/d0-cmake"
    mkdir -p "$D0_CMAKE"
    cat > "$D0_CMAKE/CMakeLists.txt" <<EOF_CMAKE
cmake_minimum_required(VERSION 3.10)
project(d0_blind_id C)
set(D0 "$ROOT/engine/d0_blind_id")
find_path(GMP_INCLUDE gmp.h REQUIRED)
find_library(GMP_LIBRARY NAMES libgmp.a REQUIRED)
# the sources and flags of its Makefile.am (GMP back end)
add_library(d0_blind_id SHARED \${D0}/d0_blind_id.c \${D0}/d0.c \${D0}/d0_iobuf.c \${D0}/sha2.c \${D0}/d0_bignum-gmp.c)
target_include_directories(d0_blind_id PRIVATE \${GMP_INCLUDE})
target_link_libraries(d0_blind_id PRIVATE \${GMP_LIBRARY})
# sha2.c wants BYTE_ORDER, which configure would find; bionic has it in endian.h
target_compile_options(d0_blind_id PRIVATE -include endian.h)
add_library(d0_rijndael SHARED \${D0}/d0_rijndael.c)
foreach(t d0_blind_id d0_rijndael)
    target_compile_options(\${t} PRIVATE -fvisibility=hidden)
    # GMP's symbols stay inside; nothing may be left undefined for dlopen to trip on
    target_link_options(\${t} PRIVATE -Wl,--exclude-libs,ALL -Wl,--no-undefined)
endforeach()
install(TARGETS d0_blind_id d0_rijndael LIBRARY DESTINATION lib)
EOF_CMAKE
    cmake_build "$D0_CMAKE" d0_blind_id
    for so in libd0_blind_id.so libd0_rijndael.so; do
        test -f "$PREFIX/lib/$so" || { echo "d0_blind_id build produced no $so" >&2; ls -la "$PREFIX/lib" >&2; exit 1; }
    done
fi

case "$ABI" in
    arm64-v8a|x86_64) touch "$PAGE_SIZE_STAMP" ;;
esac
log "dependencies ready in $PREFIX"
ls -1 "$PREFIX/lib" | sed 's/^/   /'
