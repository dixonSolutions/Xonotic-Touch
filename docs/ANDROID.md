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

Three more only showed up on a device, each of which alone stopped the game
reaching its menu:

* **No directory listing worked at all.** `listdirectory()` decides between the
  real filesystem and APK-asset emulation with `basepath[0] != '/'`, but
  `FS_AddGameDirectory` passes the whole directory as `path` and leaves
  `basepath` empty — and `""` fails that test exactly as a relative name does.
  Every listing went down the asset path, looked for an `ls.txt` that does not
  exist, and returned nothing, so **no `.pk3` in a writable gamedir was ever
  found**. The test now uses the composed path.
* **The engine did not know it was Xonotic.** darkplaces picks its game from
  `argv[0]`, which SDL sets to `app_process`, so it started as plain Quake
  looking for `id1/`. `XonoticActivity` passes `-xonotic` explicitly.
* **The session lock is unobtainable, not contended.** `Host_LockSession` calls
  `FS_SysOpen(p, "wl", ...)`, and under `USE_RWOPS` a locked open *always*
  returns invalid. Treating that as fatal meant the game could never start, so
  on that path it warns instead.

Two more stopped the *map* from drawing once the menu worked. Both are cases
where mobile support written for Quake 1 meets a Xonotic-sized map:

* **32bit draw indices.** `Mod_AllocSurfMesh` only builds the 16bit index array
  for models of 65536 vertices or fewer; anything larger is drawn with
  `GL_UNSIGNED_INT`. That is core in desktop GL, but on ES 2.0 it needs
  `GL_OES_element_index_uint`, and nothing checked for it. Every Xonotic map is
  far past 65536 vertices, so on a device without that extension every world
  draw was a `GL_INVALID_ENUM` that drew nothing — while the 2D menu, which
  indexes quads with 16bit `quadelement3s`, drew perfectly. The renderer now
  detects the extension, and when it is absent forces the dynamic batch path,
  which renumbers each batch from zero and so fits 16bit indices. It copies
  vertex arrays every frame and is slower; it is also the path that draws.
* **The far plane did not reach the far side of the map.** The `DP_MOBILETOUCH`
  block pinned `r_farclip_base` at 4096 and switched `r_farclip_world` off
  entirely, so the far plane was a flat 4096 units regardless of map size. That
  is reasonable for the Quake 1 maps this branch was written against, and
  useless for Xonotic, whose maps are routinely several times that across:
  everything past a few rooms was clipped away. `r_farclip_world` now keeps its
  default, so the far plane scales with the map as it does on desktop. The near
  plane stays at 4 and the far plane stays finite — both genuinely help the
  depth precision this branch was worried about.

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
2. Fetch `Xonotic-latest.zip` from the Xonotic autobuild server and extract its
   `.pk3` payloads — about 1.16 GB. That one archive already carries data, maps,
   music and fonts; the `-mappingsupport` and `-high` builds that
   `scripts/fetch-assets-posix.sh` also pulls on Ubuntu Touch duplicate it, for
   1.6 GB of download and no new files. It is extracted straight off the socket
   rather than staged, so it needs no temporary space and a failure costs only a
   retry.
3. Start `XonoticActivity`.

## Updating

A sideloaded app has no store behind it, so it watches its own release feed.
`BootActivity` asks GitHub for the latest release before unpacking anything —
an update would replace the payload it was about to extract — and offers it:

* **Update** streams the ABI-matching APK straight into a `PackageInstaller`
  session and hands it to the system.
* **Not now** goes straight into the game and asks again next launch.
* **Skip this version** suppresses that one release.

Android confirms every package install itself, so the app cannot update behind
the player's back, and there is no path where a failed or slow check keeps
anyone out of the game — any error just falls through to launching.

`scripts/android-verify-update-feed.sh` runs after each release and fails the
build if the release could not drive an update. The updater finds its download
by tag shape (`vX.Y.Z`) and asset filename (must contain the ABI), and both are
conventions rather than contracts: renaming the APKs would strand every install
on its current build with no error anywhere.

### What the updater will and will not install

The three Touch projects (this one, SuperTuxKart Touch and Potato Tomato) share one
self-update model, and this is the part of it worth stating out loud: the only
thing that ever reaches the package installer is an `.apk` release asset served
by `github.com` for this repository's own path.

`isTrustedApkUrl` enforces that — https, `github.com`, this repo, an `.apk`
suffix, no `..` — and it is checked twice: once when picking the asset out of
the release feed, and again in `install()` immediately before the bytes are
streamed into the `PackageInstaller` session. The second check is not
redundant. It is the one guarding the actual install, and it does not depend on
a caller having remembered the first.

The URL already arrives inside a TLS response from `api.github.com`, so this is
defence in depth rather than the only thing standing in the way. It costs one
string comparison, and what it guards is an unattended package install.

A session that fails partway is abandoned rather than merely closed: closing
only drops the handle, and the staged bytes -- a whole partial APK -- would sit
there until the system reaped them, so a retried download would otherwise cost
the player storage every time.

### What survives an update

Everything the player has. An update is an ordinary same-signature upgrade, so
Android keeps the app's storage as-is, and:

* saved configs and keybinds live in `<basedir>/userdata`, which the update
  path never touches;
* downloaded maps, textures and music are `.pk3` files in `<basedir>/data` and
  are not re-fetched;
* the bundled slim payload is re-extracted on a version change — it overwrites
  its own files and deletes nothing else.

This depends on both builds being signed with the same key. Android refuses an
update whose signature changed, and the only way out of that is uninstalling,
which takes the data with it. See **Signing** below.

## Debugging on a device

A phone has no launcher command line, so `XonoticActivity` appends anything in
`<basedir>/xonotic.args` to the engine's arguments — whitespace separated, `#`
for comments. `-developer` routes the engine's console to logcat (tagged
`app_process`, because that is SDL's `argv[0]`), and `-condebug` writes
`<basedir>/userdata/data/qconsole.log`, which `adb shell cat` can read whole:

```bash
adb shell "echo '-xonotic -condebug' > /storage/emulated/0/Android/data/io.github.dixonsolutions.xonotictouch/files/xonotic.args"
```

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

Both repositories are signed with the **same** key, so one keystore covers every
Android build here. That is fine — a signing key is not tied to a package name —
and it means one backup to keep rather than two.

Without those secrets the build still succeeds, but `scripts/android-build.sh`
generates a throwaway key. **APKs signed with a throwaway key cannot upgrade an
existing install**, which also breaks in-app updates: Android refuses an update
whose signature changed, and uninstalling to get around it takes the player's
data with it.

```bash
keytool -genkeypair -v -keystore release.keystore -alias xonotictouch \
  -keyalg RSA -keysize 2048 -validity 10000
```
