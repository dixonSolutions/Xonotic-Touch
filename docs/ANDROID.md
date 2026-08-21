# Android package

Xonotic Touch ships a native Android APK alongside the Flatpak and Ubuntu Touch
`.click`. There is no upstream Android build of darkplaces to inherit, so this
one is assembled here: `android/` is a plain gradle project whose native library
is darkplaces compiled with the NDK.

| | |
|---|---|
| Package id | `io.github.dixonsolutions.xonotictouch` |
| App name | Xonotic Touch |
| Launcher icon | `engine/misc/logos/icons_png/xonotic_512.png` — the same logo the Click and Flatpak packages use |
| ABIs | `arm64-v8a`, `armeabi-v7a` |
| min / target SDK | 21 / 35 |
| Renderer | GLES2 (`USE_GLES2`, already set for `__ANDROID__` in `sys.h`) |
| APK payload | Slim data only; maps, textures and music download on first launch |

## Why it works at all

darkplaces has never had an Android *build*, but it has carried Android *support*
for years. `sys.h` already turns on, for `__ANDROID__`:

```
USE_GLES2  USE_RWOPS  LINK_TO_ZLIB  LINK_TO_LIBVORBIS  DP_MOBILETOUCH  DP_FREETYPE_STATIC
```

That support had not seen a compiler in a long time, though, so pointing the NDK
at it turned up a handful of things:

* **Entry point.** SDL's Android glue calls `SDL_main` over JNI; darkplaces
  spells it `main`. `sys_sdl.c` is compiled with `-Dmain=SDL_main`.
* **Codecs.** Everything the engine normally `dlopen()`s is linked statically
  instead, because Android has no system copies — see `scripts/android-deps.sh`.
  The one exception is libpng, which stays dynamic (rewriting its thirty-odd
  function pointers is not worth it) and ships in the APK as `libpng16.so`.
  `image_png.c` gained that unversioned name in its lookup list.
* **Basedir.** `fs.c` defaults Android to `/sdcard/xonotic`, which scoped storage
  made unwritable in Android 10. `XonoticActivity` passes an explicit `-basedir`
  under the app's private external files directory.
* **GLES2 symbol gaps.** `glquake.h`'s GLES2 branch is a list of `qgl* -> gl*`
  defines that stops at core ES 2.0, but `gl_backend.c` still names `GLAPIENTRY`,
  `GL_BGRA` and the `ARB_debug_output` enums on `RENDERPATH_GL32` branches a
  GLES2 build never takes. They now parse; they still do nothing.
* **KTX textures.** `gl_textures.c` includes `ktx10/include/ktx.h` under a bare
  `__ANDROID__` guard. That library is not in this tree and Xonotic ships no
  `.ktx` textures, so the path moved behind `DP_ANDROID_KTX`.
* **Video capture is off.** Its frame readback wants GLES 3 pixel-pack buffers.
  `snd_main.c` reads `cls.capturevideo.active` without the
  `CONFIG_VIDEO_CAPTURE` guard its six sibling references have — now guarded.
* **`const` drift.** `vid_sdl.c`'s `DP_MOBILETOUCH` branch assigns through
  `mode`, which went `const` since that branch was last built. Cast, as the
  `WIN32` path beside it already does.

The touch HUD needs nothing special: it is the same `touch_ui.c` /
`DP_MOBILETOUCH` path the Ubuntu Touch build uses.

## Layout

```
android/
  app/src/main/cpp/CMakeLists.txt   darkplaces -> libmain.so
  app/src/main/java/…/BootActivity      unpacks + downloads game data
  app/src/main/java/…/GameData          the unpack/download itself
  app/src/main/java/…/XonoticActivity   SDLActivity subclass, passes -basedir
scripts/android-deps.sh           SDL2, ogg, vorbis, freetype, jpeg, png per ABI
scripts/android-stage-assets.sh   QuakeC + slim data -> the APK's assets
scripts/android-build.sh          branding, SDL glue, gradle
```

`CMakeLists.txt` does not carry its own source list — it parses `OBJ_COMMON` and
friends out of `engine/darkplaces/makefile.inc`. The Touch port adds engine files
(`touch_ui.c`, `vid_touchdetect.c`), and a duplicated list would quietly rot.

## Building locally

```bash
ANDROID_SDK_ROOT=~/Android/Sdk ANDROID_NDK_ROOT=~/Android/Sdk/ndk/27.3.13750724 \
  ./scripts/android-build.sh --abi arm64-v8a --version 1.2.0
```

Needs `gradle` 8.9+, `cmake`, `ninja`, ImageMagick, and the usual host toolchain
for gmqcc. Everything else is downloaded into `build/android/` and cached there:
SDL2, libogg, libvorbis, freetype, libjpeg-turbo and libpng sources, built once
per ABI.

## What the app does on first launch

`BootActivity` runs before the engine, because darkplaces scans its basedir
during `FS_Init` and would not see anything Java staged afterwards.

1. Unpack `assets/xonotic-slim-data.zip` — game logic, configs, and the menu skin
   the download screen itself needs. Re-runs after an app update so new progs
   never sit beside old ones.
2. Fetch `Xonotic-latest.zip`, `-mappingsupport` and `-high` from the Xonotic
   autobuild server and extract their `.pk3` payloads. Same source, same
   archives, as `scripts/fetch-assets-posix.sh` uses on Ubuntu Touch. About 1 GB.
3. Start `XonoticActivity`.

## Known gaps

* **libcurl is absent**, so in-game map downloads and the update check are
  disabled. The first-launch download does not use it — that is plain Java.
* **d0_blind_id is absent**, so server-side player authentication and stats are
  unavailable. Plain connections work.
* No AAB, so this is sideload / F-Droid shaped rather than Play Store shaped.

## Signing

CI signs release APKs with a repository keystore when one is configured:

| Secret | Meaning |
|---|---|
| `ANDROID_KEYSTORE_BASE64` | `base64 -w0 release.keystore` |
| `ANDROID_KEYSTORE_PASSWORD` | store *and* key password |
| `ANDROID_KEY_ALIAS` | key alias |

Without them the build still succeeds, but `scripts/android-build.sh` generates a
throwaway key. **APKs signed with a throwaway key cannot upgrade an existing
install** — Android refuses an update whose signature changed. Set the secrets
before pointing anyone at the download.

```bash
keytool -genkeypair -v -keystore release.keystore -alias xonotictouch \
  -keyalg RSA -keysize 2048 -validity 10000
```
