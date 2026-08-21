#!/bin/bash
# Build the Xonotic Touch Android APK for one ABI.
#
#   scripts/android-deps.sh    native libraries darkplaces links against
#   scripts/android-stage-assets.sh   the slim data payload inside the APK
#   this script                       branding, SDL glue, gradle
#
# darkplaces itself is compiled by android/app/src/main/cpp/CMakeLists.txt, which
# reads its source list out of the engine's own makefile.inc.
set -euo pipefail

ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
ANDROID_DIR="$ROOT/android"
CACHE_DIR="${ANDROID_CACHE_DIR:-$ROOT/build/android}"
OUT_DIR="${OUT_DIR:-$ROOT/build/android-out}"

ABI="${ANDROID_ABI:-arm64-v8a}"
VERSION="${PROJECT_VERSION:-1.0.0}"
VERSION_CODE="${PROJECT_CODE:-1}"
GRADLE="${GRADLE:-gradle}"
SDL_VERSION="${SDL_VERSION:-2.32.10}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--abi arm64-v8a|armeabi-v7a] [--version X.Y.Z] [--code N] [--out DIR]

Environment:
  ANDROID_SDK_ROOT / ANDROID_HOME   Android SDK (required)
  ANDROID_NDK_ROOT                  Android NDK r26+ (required)
  ANDROID_KEYSTORE / ANDROID_KEYSTORE_PASSWORD / ANDROID_KEY_ALIAS
                                    Release signing; a throwaway key is
                                    generated when unset (see docs/ANDROID.md).
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --abi) ABI="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --code) VERSION_CODE="$2"; shift 2 ;;
        --out) OUT_DIR="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

case "$ABI" in
    arm64-v8a|armeabi-v7a) ;;
    *) echo "Unsupported ABI: $ABI (arm64-v8a, armeabi-v7a)" >&2; exit 1 ;;
esac

SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
test -d "${SDK_ROOT:-}" || { echo "Set ANDROID_SDK_ROOT to an Android SDK install." >&2; exit 1; }
test -d "${ANDROID_NDK_ROOT:-}" || { echo "Set ANDROID_NDK_ROOT to an Android NDK install." >&2; exit 1; }
export ANDROID_SDK_ROOT="$(cd "$SDK_ROOT" && pwd)"
export ANDROID_HOME="$ANDROID_SDK_ROOT"
export ANDROID_NDK_ROOT="$(cd "$ANDROID_NDK_ROOT" && pwd)"

PREFIX="$CACHE_DIR/prefix/$ABI"
mkdir -p "$OUT_DIR"

log() { printf '\n== %s\n' "$*"; }

##### Native dependencies #####################################################

log "Native dependencies ($ABI)"
ANDROID_CACHE_DIR="$CACHE_DIR" SDL_VERSION="$SDL_VERSION" \
    bash "$ROOT/scripts/android-deps.sh" "$ABI"

##### SDL Java glue ###########################################################

# SDLActivity and friends ship with the SDL sources rather than as a Maven
# artifact, so take them from the same tarball the native library was built from
# — a version skew between the two shows up as a JNI signature mismatch at
# runtime, not at build time.
SDL_JAVA_SRC="$CACHE_DIR/src/SDL2-$SDL_VERSION/android-project/app/src/main/java/org/libsdl/app"
SDL_JAVA_DST="$ANDROID_DIR/app/src/main/java/org/libsdl/app"
test -d "$SDL_JAVA_SRC" || { echo "Missing SDL Java sources at $SDL_JAVA_SRC" >&2; exit 1; }
rm -rf "$SDL_JAVA_DST"
mkdir -p "$SDL_JAVA_DST"
cp -f "$SDL_JAVA_SRC"/*.java "$SDL_JAVA_DST/"

##### Shared libraries the APK carries ########################################

JNI_LIBS="$ANDROID_DIR/app/src/main/jniLibs/$ABI"
rm -rf "$JNI_LIBS"
mkdir -p "$JNI_LIBS"
# libSDL2.so is linked; libpng16.so is dlopen()ed by image_png.c. Both have to be
# real files in the install's lib directory.
for so in libSDL2.so libpng16.so; do
    test -f "$PREFIX/lib/$so" || { echo "Missing $PREFIX/lib/$so" >&2; exit 1; }
    install -m 644 "$PREFIX/lib/$so" "$JNI_LIBS/$so"
done

##### Branding ################################################################

ICON_SRC="${ANDROID_ICON_SRC:-$ROOT/engine/misc/logos/icons_png/xonotic_512.png}"
test -f "$ICON_SRC" || { echo "Missing official icon: $ICON_SRC" >&2; exit 1; }
MAGICK="$(command -v magick || command -v convert || true)"
test -n "$MAGICK" || { echo "ImageMagick (magick/convert) is required" >&2; exit 1; }

# The same logo stage-click.sh installs, at the densities Android asks for.
log "Generating launcher icons from $(basename "$ICON_SRC")"
while read -r density size; do
    dir="$ANDROID_DIR/app/src/main/res/mipmap-$density"
    mkdir -p "$dir"
    "$MAGICK" "$ICON_SRC" -resize "${size}x${size}" "$dir/icon.png"
done <<'DENSITIES'
mdpi 48
hdpi 72
xhdpi 96
xxhdpi 144
xxxhdpi 192
DENSITIES

##### Game data payload #######################################################

if [ ! -f "$ANDROID_DIR/app/src/main/assets/xonotic-slim-data.zip" ]; then
    log "Staging slim game data"
    ANDROID_CACHE_DIR="$CACHE_DIR" bash "$ROOT/scripts/android-stage-assets.sh"
fi

##### Signing #################################################################

KEYSTORE="${ANDROID_KEYSTORE:-}"
STOREPASS="${ANDROID_KEYSTORE_PASSWORD:-}"
ALIAS="${ANDROID_KEY_ALIAS:-}"
if [ -z "$KEYSTORE" ]; then
    KEYSTORE="$CACHE_DIR/throwaway.keystore"
    STOREPASS="${STOREPASS:-xonotictouch}"
    ALIAS="${ALIAS:-xonotictouch}"
    if [ ! -f "$KEYSTORE" ]; then
        log "No ANDROID_KEYSTORE set — generating a throwaway signing key"
        echo "   Installs from different builds will NOT upgrade in place." >&2
        keytool -genkeypair -v \
            -keystore "$KEYSTORE" \
            -storepass "$STOREPASS" -keypass "$STOREPASS" \
            -alias "$ALIAS" \
            -keyalg RSA -keysize 2048 -validity 10000 \
            -dname "CN=Xonotic Touch, OU=CI, O=dixonSolutions, C=US" >/dev/null
    fi
fi
KEYSTORE="$(cd "$(dirname "$KEYSTORE")" && pwd)/$(basename "$KEYSTORE")"

##### Build ###################################################################

NDK_VERSION="$(sed -n 's/^Pkg.Revision *= *//p' "$ANDROID_NDK_ROOT/source.properties" | tr -d '\r')"
test -n "$NDK_VERSION" || { echo "Cannot read NDK version from $ANDROID_NDK_ROOT/source.properties" >&2; exit 1; }

# AGP resolves android.ndkVersion under $SDK/ndk/<version> and nowhere else, so
# an NDK installed elsewhere has to be visible there.
if [ ! -e "$ANDROID_SDK_ROOT/ndk/$NDK_VERSION" ]; then
    mkdir -p "$ANDROID_SDK_ROOT/ndk"
    ln -sfn "$ANDROID_NDK_ROOT" "$ANDROID_SDK_ROOT/ndk/$NDK_VERSION"
fi

log "Gradle assembleRelease ($ABI, $VERSION+$VERSION_CODE, NDK $NDK_VERSION)"
( cd "$ANDROID_DIR" && "$GRADLE" --no-daemon assembleRelease \
    -Pdeps_prefix="$PREFIX" \
    -Pcompile_abi="$ABI" \
    -Pversion_name="$VERSION" \
    -Pversion_code="$VERSION_CODE" \
    -Pndk_version="$NDK_VERSION" \
    -Pkeystore="$KEYSTORE" \
    -Pstorepass="$STOREPASS" \
    -Palias="$ALIAS" )

APK="$(find "$ANDROID_DIR/app/build/outputs/apk/release" -name '*.apk' -type f \
       -printf '%T@ %p\n' | sort -rn | head -1 | cut -d' ' -f2-)"
test -n "$APK" || { echo "No APK produced" >&2; find "$ANDROID_DIR/app/build/outputs" -type f >&2 || true; exit 1; }
cp -v "$APK" "$OUT_DIR/XonoticTouch-$VERSION-$ABI.apk"

log "APK in $OUT_DIR"
ls -lh "$OUT_DIR"
