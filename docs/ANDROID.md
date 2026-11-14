# Android package

Godnotic ships a native Android APK alongside the Flatpak and Ubuntu Touch
`.click`. There is no upstream Android build of darkplaces to inherit, so this
one is assembled here: `android/` is a plain gradle project whose native library
is darkplaces compiled with the NDK.

| | |
|---|---|
| Package id | `io.github.dixonsolutions.godnotic` |
| App name | Godnotic |
| Launcher icon | Adaptive `mipmap/icon` (eagle foreground in the 66 dp safe zone, dark background, monochrome layer for themed icons) plus a legacy `icon.png` on the same dark plate, all from `scripts/gen-branding.py` (`branding/android/`) |
| ABIs | `arm64-v8a`, `armeabi-v7a`, `x86_64` |
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
  The exceptions are libpng, which stays dynamic (rewriting its thirty-odd
  function pointers is not worth it) and ships in the APK as `libpng16.so` --
  `image_png.c` gained that unversioned name in its lookup list -- and libcurl,
  which ships as `libcurl.so` (see **Joining servers** below).
* **Basedir.** `fs.c` defaulted Android to `/sdcard/xonotic`, which needs a
  storage permission and which scoped storage made unwritable in Android 10.
  `XonoticActivity` passes an explicit `-basedir` under the app's private
  external files directory, and the `/sdcard` fallback is now an error rather
  than a silent attempt.
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

Four more made maps draw with the wrong light and the wrong colours even
after that. The shader ones were found by validating DarkPlaces' builtin GLSL
with glslang as GLSL ES 1.00, assembled exactly as `R_GLSL_CompilePermutation`
does on `RENDERPATH_GLES2`: 217 of 300 reachable permutations failed before,
none after.

* **`sat()` did not compile.** `shader_glsl.h` defined it as `clamp(x, 0, 1)`.
  GLSL ES has no implicit int-to-float conversion, so strict drivers (Mali,
  ANGLE) rejected every permutation using it -- diffuse shading, specular and
  vertex texture blending. The fallback then stripped `DIFFUSE` from the
  deluxemapped world shader, which leaves `diffusetex * Color_Ambient`: the
  lightmap was ignored altogether. The same went for models, whose
  `MODE_LIGHTDIRECTION` used `#define lightcolor 1`. Both are float literals now.
* **Texture updates swapped red and blue.** GLES has no `GL_BGRA` upload, so
  `R_SetupTexture` swaps BGRA data to RGBA when a texture is created, but
  `R_UpdateTexture` sent later updates unswapped. Textures remember the swap now.
* **Render-target textures had no storage.** The GLES colorbuffer textype was
  `GL_RGB565` with `GL_RGBA`, which `glTexImage2D` rejects on every ES version,
  so any view drawn through an FBO (gamma/contrast, blur, bloom) sampled
  nothing. It is 8bit RGBA now.
* **DDS textures on GPUs without S3TC.** The release packs carry world and
  model textures as DDS only -- there are no tga/jpg originals to fall back to
  -- so they are software decoded (quarter resolution). That used to come out
  black because the decoded BGRA was uploaded as `GL_BGRA`; it is swapped to
  RGBA first now, and the menu turns `r_texture_dds_swdecode` on instead of
  turning DDS loading off.

The touch HUD needs nothing special: it is the same `touch_ui.c` /
`DP_MOBILETOUCH` path the Ubuntu Touch build uses.

## Joining servers

The Flatpak and Click packages start the engine through `packaging/start.sh`,
which runs the Touch config chain (`+exec xonotic.cfg`, `config.cfg`,
`autoexec.cfg`, then a generated `touch/startup.cfg`). Android has no shell
launcher, so `XonoticActivity` passes the same `+exec` chain and `GameData`
writes `touch/startup.cfg` into the userdir on every start. It pins the two
settings a public server needs, after `config.cfg` so an archived value
cannot win:

* `cl_csqc_download 2` -- use the bundled Touch CSQC only on a server running
  this exact build; take the server's CSQC everywhere else.
* `cl_curl_enabled 1` -- download the server's map pk3 from its
  `sv_curl_defaulturl`, which is the only way Xonotic servers hand out maps.

For the second, `scripts/android-deps.sh` builds libcurl (HTTP/HTTPS only)
with mbedTLS linked in, and the APK ships it as `libcurl.so`, a name
`libcurl.c` already `dlopen()`s. There is no CA bundle in the APK: on Android
`libcurl.c` points `CURLOPT_CAPATH` at the system store
(`/apex/com.android.conscrypt/cacerts`, or `/system/etc/security/cacerts`
before Android 14).

`-customgamename` is not passed: on its own it also renames the game in master
server queries, and the server browser would list nothing. `start.sh` passes
it for the window title and undoes that with `-customgamenetworkfiltername
Xonotic`.

## In the background

Home, the app switcher, another app on top or the screen turned off do not
end a match, a join or a download:

* **Connected to a server**: the engine keeps running without drawing
  (`touch_android.c`). SDL's default would block its main thread from
  `onPause`/`onStop` until the return; no packet went out and the server
  dropped the player after `net_messagetimeout` (30 s in Xonotic). Now it
  reads and sends packets at `cl_maxidlefps` and the player stays in the
  match, standing still: a public server is never paused, so they can be
  killed meanwhile. A map change while away loads in the background.
* **Joining** (waiting for the server, curl downloading the server's maps,
  loading the map): carries on. The map loads into the GL context bound
  without a surface (`EGL_KHR_surfaceless_context`); on a device without that
  extension the load waits for the return. The player comes back to the
  loading screen while the join runs, or to the game once it is done.
* **First-launch data download**: carries on; when it finishes while away, a
  notification says the game is ready and the next return starts it.
* **Local game** (a bot match from Create or Singleplayer, whatever its
  number of player slots): paused while away (the engine waits for the
  return, using no CPU) and running again on return.
* **Hosting a LAN game** (started from the Host screen), or a local game
  another player has joined: keeps running, like a connection, so the players
  on it are not timed out (`TouchLAN_KeepRunningInBackground` in
  `touch_lan_logic.h`, checked by `scripts/test-lan-logic.sh`). The
  engine writes `hosting` to `touch_netstate`; the foreground service keeps
  the process, and `LanLocks` holds a Wi-Fi multicast lock and a low-latency
  Wi-Fi lock until hosting stops ([LAN.md](LAN.md)). In the main menu with nothing going
  on, the engine waits the same way.

While the engine is connected, joining or downloading (it writes the state to
`userdata/data/touch_netstate`, which `XonoticActivity` reads every 2 s), and
while the first-launch download runs, `KeepAliveService` is a foreground
service with a notification ("Godnotic is staying connected: Connected
to <server> - <map>") and holds a partial wake lock. Without it Android
freezes a background app's process within seconds (Android 14+), can kill it
for memory, and lets the CPU sleep with the screen off. The service stops as
soon as the engine is idle while the game is in front, and 60 s after the last
busy state while it is away (a map change or reconnect that reads idle for one
poll must not lose it: Android 12+ only lets it start while the app is in
front, which is when a join starts; later changes only update the
notification). The notification shows a download's size, speed and time left
under the state; a join that finishes while away posts "Ready to play". While
busy in the background a low-latency Wi-Fi lock keeps the radio out of power
save. The automatic reconnect (docs/NETWORK.md) shows as a bar over the game
with Cancel. If notifications are denied (Android 13+) the service still
runs, without a visible notification.

The launcher icon brings a running game back as it was: the boot screen does
not prepare data or install a downloaded update under it. Under an AndySU host
a live engine swap is declined while connected, joining or downloading and
applies on the next start.

Limits: if Android kills the process anyway (some vendors' battery savers do,
foreground service or not), or the player swipes the game out of the recent
apps, the connection is gone and the next start is a cold one, with no offer
to rejoin. Doze and app standby buckets are not exercised by the test VM; a
foreground service is exempt from Doze's network cut-off, but this has only
been tried on BlissOS (Android 13 x86_64), not on a phone or tablet left
asleep for a long time. Keeping a connection costs battery: the engine runs
at `cl_maxidlefps` while away.

## Permissions

The merged manifest (`aapt2 dump permissions`) declares exactly:

| Permission | Why |
|---|---|
| `INTERNET` | first-launch data download, servers, map downloads, update check |
| `ACCESS_NETWORK_STATE` | the mobile-data setting |
| `POST_NOTIFICATIONS` | download and staying-connected notifications (Android 13+) |
| `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_DATA_SYNC`, `FOREGROUND_SERVICE_SPECIAL_USE` | `KeepAliveService`: the data download, and a server connection kept while in the background |
| `WAKE_LOCK` | the CPU keeps running with the screen off while connected, joining, downloading or hosting a LAN game |
| `CHANGE_WIFI_MULTICAST_STATE` | `LanLocks`: while hosting a LAN game, lets the Wi-Fi driver pass other players' broadcast search through ([LAN.md](LAN.md)) |
| `REQUEST_INSTALL_PACKAGES` | the self-updater hands its APK to `PackageInstaller` |
| `UPDATE_PACKAGES_WITHOUT_USER_ACTION` | later updates without the confirm dialog (Android 12+) |

Only `POST_NOTIFICATIONS` is a runtime permission: the boot screen asks for it
on Android 13+ when the first-launch download starts, and everything works if
it is refused. Nothing asks for storage, Bluetooth, location, nearby
devices or the microphone: SDL's Bluetooth controller support checks
`BLUETOOTH_CONNECT` and quietly skips itself without it, and the engine never
calls `SDL_AndroidRequestPermission`. Everything the app writes is in its own
private storage. The Linux touch detection that reads `/proc/bus/input`, DMI,
`/etc/os-release` and `/dev/input` is compiled out or returns early on Android;
`DP_MOBILETOUCH` decides there.

`REQUEST_INSTALL_PACKAGES` is the one a player may notice: Android shows it as
"install unknown apps" and asks for it in Settings the first time the updater
installs something, never before. Removing it would mean no in-app updates.

## Layout

```
android/
  app/src/main/cpp/CMakeLists.txt   darkplaces -> libmain.so
  app/src/main/java/…/BootActivity      unpacks + downloads game data
  app/src/main/java/…/GameData          the unpack, and what the download needs
  app/src/main/java/…/PackFetch         pk3s out of the remote zip by HTTP Range
  app/src/main/java/…/XonoticActivity   SDLActivity subclass, passes -basedir
scripts/android-deps.sh           SDL2, ogg, vorbis, freetype, jpeg, png, curl+mbedTLS per ABI
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
SDL2, libogg, libvorbis, freetype, libjpeg-turbo, libpng, mbedTLS and curl sources, built once
per ABI.

## Boot screen, banner and the console button

`GodnoticBootActivity` (the launcher entry since the Kotlin shell) is a
subclass of `BootActivity` (the entry older AndySU hosts' manifests name), so
both hosts run one screen. Until there is something to wait for or decide it
shows only the Godnotic banner on black (`BootSplash`). `XonoticActivity`
keeps that same picture over the SDL surface until the engine reports a
frame with something lit in it (`NativeUI_Presented` in `nativeui/engine.c`
reads one row of the back buffer before each swap until it is not black,
then the bridge's `presented()` flag lifts the cover), so a start never shows
black between the boot screen and the engine's loading picture. The banner is
an asset (`xt/godnotic-banner.png`, copied from `branding/` by `build.gradle`),
never an `R.drawable`, for the HostRes reason.

The screen itself is one centred column at most 640 dp wide: the banner, a
28 sp title, one line saying what is downloaded and how big, the bar with a
large percentage, and "233 / 439 MB · 3.6 MB/s · 57 s left" under it, with
56 dp pill buttons. The log names the GPU probe result and the archive used
("GPU probe: Mali-G68, ASTC supported", "Game data set: slim (ASTC) <url>").

The console is the engine's own: a terminal icon in the top right corner
(`engine/darkplaces/touch_shellbtn.c`, every platform) toggles the engine
console sheet. On Android `XonoticActivity` reports the display-cutout and
rounded-corner insets and the density through the JNI command queue
(`set _touch_safe_insets "l t r b density"`), which only the engine thread
drains. The old Android `>_` button and its dialogs are gone; plugins are
managed with the `lua_mod*` console commands and the in-game Mods dialog.

## The native interface

Outside a match the player sees Android views, not the engine's QuakeC menu
(`android/app/src/main/kotlin/.../fe/`). The screens are described once by the
menu VM through the UI bridge ([UI_BRIDGE.md](UI_BRIDGE.md)) and drawn here with
native components; the engine side of the transport is
[FRONTEND_BRIDGE.md](FRONTEND_BRIDGE.md).

* **One overlay, one activity.** `FrontendHost` adds one view tree over the SDL
  surface in `XonoticActivity`. While it shows it takes every touch and key
  (`dispatchKeyEvent`; Back included) and nothing reaches the engine; during
  play it is `GONE` and the engine has the surface and input as before. Text
  fields use Android's IME. Behind an opaque native screen the engine draws
  nothing and idles at `cl_maxidlefps` (after a 45 s window in which
  `touch_warm` prepares the next game at full rate); sound and the network go on.
* **Launch is one screen.** The system splash (Android 12+, black), the boot
  activity and the game's window show the same banner at the same place on the
  same black (`BootSplash.view`); there are no activity animations, the game's
  window has no preview (`Theme.XonoticGame`), and the boot activity hands over
  only after its banner's first frame. Home replaces the banner with one fade.
  Measured on the Tab Active5: `am start -W` TotalTime 230-330 ms, banner drawn
  50-140 ms after `XonoticActivity.onCreate`, Home at 0.8-1.2 s.
* **Renderer and the Rust core.** The Rust loader core (`loader/crates/android`,
  `libgodnotic_android.so`, the same `godnotic-loader-core` store as the Linux
  app) reads the bridge's snapshots and diffs and answers each message with the
  operations of the `UiRenderer` seam (create/update/remove, navigate, dialog,
  toast, rows, images) and the messages for the engine; `fe/RustCore.kt` plays
  them on `ViewRenderer`. One JSON string in and one out per JNI call, on the UI
  thread; Rust holds no Java reference and never calls into Java; a panic is
  caught at the boundary and the native layer then falls back. A build without
  the library (`GODNOTIC_RUST_JNILIBS` unset), or `<basedir>/fe-kotlin-core`,
  uses the Kotlin reader `UibClient` instead, which follows the same rules
  (`cargo test -p godnotic-loader-android` plays every fixture and sequence of
  tests/uibridge/fixtures through the Rust driver; the Gradle unit tests do the
  same for `UibClient`). The library is a host library like
  `libnativeui.so`: an AndySU engine swap does not replace it.
  Lists with `selection` single or multi select on a tap (drawn at once, event
  `row`/`select`) and activate on a double tap, Enter or the accessibility
  action (`row`/`activate`); a card grid with `rows: N` shows N rows that
  scroll sideways, tiles sized from the height. Over the pause scrim the
  header keeps clear of `app.reserved` (the engine's console button) and a
  touch on that rect goes to the engine.
  Lists and card grids are `RecyclerView`s over windows of rows; sticky header,
  toolbar, section headers and action bar; vector icons (`Icons.kt`); headings in
  Xolonium from the game data.
* **In a match.** Xonotic's join dialog (`app.modal == ["join"]` in context
  `game`, docs/CONTROLS.md "Joining") opens by itself for an observer, and the
  player-statistics question (`uid2name`: Yes / No, Back answers No) on a
  server that keeps statistics while `cl_allow_uid2name` is -1: the
  overlay shows that dialog alone over the running match (the pause page under
  it in the bridge's stack is hidden, the engine keeps drawing: `fe screen join
  0`, a light scrim), with no tap needed. Back or a tap outside goes to the
  engine (`back`), which closes it and hides the menu; so does any described
  dialog, while the host's own confirms close locally. The rule is
  `Driver::over_game` (Rust) and `UibClient.overGame` (Kotlin).
* **One loading presentation.** While attached the host keeps
  `_touch_loading_card 1` (UI_BRIDGE.md 4.13): its loading card is then the
  only place the step, file, speed and notify lines show; the engine applies it
  only during a join or load. It goes back to 0 when the native layer falls
  back. It is not toggled per card because a local map load blocks the engine
  from its first frame, before a `set` sent with the card could run.
* **Without the engine's UI bridge** (a build without `touch_uib*.c`), the
  bridge's own Home and pause descriptions ship as assets (`assets/fe/`) and
  their actions map to engine commands; Singleplayer, Multiplayer, Settings and
  Credits open the QuakeC screens with a "Godnotic menu" pill back.
* **Falling back.** No `libnativeui.so` with the front-end bridge (an AndySU
  engine swap can pair an old resident bridge with a new engine), an engine
  without `touch_frontend.c`, `<basedir>/no-native-ui`, or any exception in the
  native layer: the overlay goes and the QuakeC menu is in charge, as before.
  The engine also takes the menu back by itself when the UI stops beating for
  4 s. On an old AndySU host the Kotlin and native code arrive with a payload
  update; the theme and manifest changes (black splash, no preview window, the
  `godnotic://join` link) need a full APK install.
* **Platform state for the engine**: thermal status, battery saver and the map
  Home's Play starts go to `touch_warm` (`touch_device_thermal`,
  `touch_device_powersave`, `touch_warm_map`); `onTrimMemory`/`onLowMemory` send
  `touch_warm_drop`.

## What the app does on first launch

`BootActivity` runs before the engine, because darkplaces scans its basedir
during `FS_Init` and would not see anything Java staged afterwards.

1. Unpack `assets/xonotic-slim-data.zip` — game logic, configs, and the menu skin
   the download screen itself needs. Re-runs after an app update so new progs
   never sit beside old ones.
2. Ask, then fetch the pk3s. `PackFetch` reads `Xonotic-latest.zip`'s central
   directory from the autobuild server with HTTP Range requests (the archive
   is never stored), and the setup step shows what the download costs, how
   much space is free (it refuses to start without room for it, and says how
   much to free). The music pack is part of the game data and always fetched. Each pk3 under
   `Xonotic/data/` then comes from its own byte range straight into its final
   file: data, maps, nexcompat, xoncompat and the two fonts, about 1.05 GB,
   plus 106 MB with music. The big pk3s are stored in the zip, so they are cut
   into 8 MB chunks that 8 connections fetch at once, each writing at its own
   offset; the small deflated fonts are inflated as one stream. A pk3 is
   written as `<name>.part`, checked against the CRC32 in the central
   directory and renamed into place. Peak storage is the pk3s themselves; the
   download-then-unzip it replaced needed the 1.2 GB archive on top.
3. Start `XonoticActivity`.

**Resuming.** How far each chunk got is saved beside its part
(`<name>.part.state`, once a second and on every stop), so Cancel, a dropped
connection (each chunk retries with a back-off for about two minutes), Quit or
a killed process all carry on from the bytes on disk. `data/.packs-setup` pins
the archive's ETag and size and lists the packs the setup finished: when the
autobuild publishes a new build, the parts and those packs are discarded and
the setup starts over rather than mixing two builds, and every range request
carries `If-Range`, so a build that changes mid-download is caught at once.
The parts an older release staged in `files/download` are deleted.

**Music.** There is no music choice: an install without the music pack (from
before it was always part of the data) gets the setup step for just that pack
at the next start, with **Not now** to play first; the old "Download music"
launcher shortcut is removed.

**Asking once.** Download (or Use mobile data) is remembered in the
`boot-download` preferences until the data is in: a Quit, a kill or a lost
connection carries on at the next start without asking again. On mobile data,
a Wi-Fi without internet (Android then routes over mobile data) or a Wi-Fi
marked as metered, the setup step says which (`Net.kt`) and offers **Use
mobile data** or **Wait for Wi-Fi**; waiting shows that state and starts by
itself when an unmetered network comes (a default-network callback). Quit
during the download asks first ("What is downloaded is kept"), as does Back.
`POST_NOTIFICATIONS` is asked for when the download starts; refused for good,
the screen says that Android may stop the download in the background.

While the boot screen shows work (the download, an unpack after an update), it
offers **Cancel**. The download stops with its chunks kept on disk; the unpack
stops between files. The screen then says where it stopped and offers
**Resume**, which carries on without asking again, and **Quit**. A cancelled
download's threads are closed down before the screen says so, so a Resume never
has two writers on one part. The boot screen's text and buttons are sized for
a phone held sideways (56 dp buttons, 18 sp text, one column at most 640 dp
wide) and it scrolls on a short screen; it still shows only when there is
something to wait for or decide. The download notification uses
`FOREGROUND_SERVICE_IMMEDIATE` (Android 12+), and a notification says when the
game is ready if the download finished while the app was in the background.

## Updating

A sideloaded app has no store behind it, so it watches its own release feed.

**The update cache.** The ABI-matching APK is downloaded into
`noBackupFilesDir/updates/` before anything is installed, as
`<asset>.apk.part` until it is complete. A download that stops (no signal, the
app closed, the phone off) carries on from those bytes next time with an HTTP
`Range` request instead of starting the 40 MB over. A finished one is checked
against the size and the SHA-256 GitHub lists for the asset (`digest`, on
assets uploaded since mid-2025), then renamed to `<asset>.apk`. A file that
fails the checksum is deleted and never reaches the installer. Every start
deletes whatever is not the release the last check found.

**Automatic updates** (the default) never hold the player at the boot screen:

1. While the game runs, `XonoticActivity` downloads the release the last check
   found into the cache, only on Wi-Fi or another unmetered connection.
   Otherwise the Updates screen says what the connection is -- mobile data,
   a Wi-Fi without internet access (so Android is on mobile data: what the
   tablet showed while it said "waiting for Wi-Fi" on Wi-Fi), or a Wi-Fi
   marked as metered -- and Download goes ahead on it.
2. Once it is complete the state is `ready`: "installs the next time you
   start".
3. On the next start `BootActivity` installs it from the cache before the
   engine starts, with no network needed. **Install now** on the Updates screen
   does it straight away instead.

**With automatic updates off**, the game still checks in the background, and
a line on the main menu says when a release is out. Download in Settings →
Updates fetches it. The next start offers to install it:

* **Update** installs it.
* **Not now** goes straight into the game and asks again next launch.
* **Skip this version** suppresses that one release.

**The boot screen only shows when there is something to wait for.** Unpacking
after an app update and the first-run asset download show it only once they
have run for 1.5 s. Installing a downloaded update, an install prompt or an
error show it at once. Otherwise the game starts straight away. An update
unpacks only the files that changed: `.bundle-manifest` keeps each file's CRC
and size from the last unpack, so a new build writes its few MB of game code
rather than the whole 94 MB bundle. The check for a new release no longer runs at boot (it could
hold the launch up to 25 s on a bad connection). `XonoticActivity` runs it on
a background thread from its first `onResume()`.

Android confirms the first in-app install itself. On Android 12 and newer the
app then becomes the package's *installer of record*, and every later update
goes through `PackageInstaller` with `USER_ACTION_NOT_REQUIRED` (the manifest
declares `UPDATE_PACKAGES_WITHOUT_USER_ACTION` for it), so the player is not
asked again. A sideloaded first install still asks once, because its installer
of record is the file manager or browser that placed it. There is no path
where a failed or slow check keeps anyone out of the game — any error just
falls through to launching.

`XonoticActivity` checks when the game starts, and again from `onResume()` once
the game has been in the background for more than an hour,
so a phone that is never cold-started still picks up releases. With automatic
updates on, each resume also retries a download that was waiting for Wi-Fi.

Releases carry `arm64-v8a`, `armeabi-v7a` and `x86_64` APKs; the last one is
for Chromebooks, Waydroid and the emulator (`scripts/android-build.sh --abi
x86_64`).

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

### The in-game Updates screen

`BootActivity` can only ask its question once, before the engine exists, in a
dialog that is gone by the time anyone is playing. Everything a player wants
afterwards — which build am I on, how far behind, check again, stop asking —
lives in the menu instead, under **Settings → Updates**. It shows:

* the installed build, the newest release, and the state: checking,
  downloading with a percentage, paused (with how much is cached), downloaded
  and installing next start, needs permission, failed;
* the automatic setting;
* Check now, Download, Install now and Skip version;
* links to the releases on GitHub (the `openurl` engine command, which only
  opens the project's own GitHub and Pages addresses);
* **What's new**: `changelog.txt` from the project's Pages site, which CI
  builds from the commits between release tags
  (`scripts/release-changelog.sh`).

The same text is the "Changes" section of each GitHub release. While a newer
build is known, a line along the bottom of the main menu points there.

The menu is QuakeC: no JNI, no way to call `AppUpdater`. So the two halves talk
through two files in the gamedir, the same handshake the asset downloader
already uses and for the same reason. `UpdateBridge` owns `update-status.txt`
and only writes it; the menu owns `update-request.txt` and only writes that.
Neither reads its own file back, so there is no shared state to race over — a
torn read costs one stale second, and the menu re-reads every second anyway.

Both files live in `<basedir>/userdata/data/data/touch/`, which is `-userdir`
plus the gamedir plus the `data/` that `VM_fopen` prefixes on its own — the same
doubling behind Xonotic's "saved in data/data/" messages. That is the only
directory a QuakeC `FILE_WRITE` can create a file in, and the first one a read
searches, so it is where the Java side has to meet the menu. `GameData`'s
`engineWriteDir` works it out once.

The format is documented once, in `qcsrc/menu/xonotic/touch_update_util.qh`;
`UpdateBridge` points at it rather than repeating it. Requests are `check`,
`download` (into the cache, then `ready`), `install` (download if needed, then
install now), `skip`, `auto-on` and `auto-off`, served by a poll thread in
`XonoticActivity`. Without one, every button on that screen would write a file
nothing ever reads.

Auto-update is opt-out, so a player who never opens the screen still gets fixes.
Turning it off does not stop the check — the screen still has to say how far
behind you are — it stops the install happening without being asked for. Android
confirms every package install either way, so even "automatic" is one tap rather
than none. Flipping the checkbox publishes nothing: the preference is the only
thing that changed, and a fresh record written around it could only guess at the
state above it and would take a found release off the screen with it.

A check that could not reach the feed is published as such, not as "up to date".
`findUpdate()` answers "nothing newer" and "could not ask" with the same null,
and treating them alike would tell a player on a flaky connection that they are
current — and drop the release the boot check had already found, which is the
one thing on that screen they might have been about to install. That release is
kept in the same preferences as the skipped tag, so Skip and Install still know
what they mean in a process that never ran the check itself.

On Linux there is nothing to install: Flatpak and the OpenStore own updates, so
`packaging/start.sh` publishes the `managed` state. The screen still shows the
version, What's new and the GitHub links, and it says when the changelog names
a newer build than the one installed. Download, Install and Skip are disabled
rather than offered when they cannot work.

### What survives an update

Everything the player has. An update is an ordinary same-signature upgrade, so
Android keeps the app's storage as-is, and:

* saved configs and keybinds live in `<basedir>/userdata`, which the update
  path never touches;
* downloaded maps, textures and music are `.pk3` files in `<basedir>/data` and
  are not re-fetched;
* the bundled slim payload is re-extracted on a version change — it overwrites
  its own files and deletes nothing else.

Keeping the files is only half of it: the next launch also has to read them.
An update always means a cold start, because Android kills the old process to
install the new APK, and two things used to lose settings there:

* `config.cfg` (mouse sensitivity, binds, video, name, and the
  `touch_perf_version` that keeps the first-start performance profile from
  going on again) was only written by a clean quit, and an Android app never
  gets one. The engine now saves it when
  SDL reports the app going to the background (`vid_sdl.c`), so a kill from an
  update, from recents or from the low-memory killer costs nothing.
* the touch layout (look speed and every other touch setting) is saved with
  QuakeC `fopen`, which puts it at `userdata/data/data/touch.layout.cfg`.
  `startup.cfg` execs it as `data/touch.layout.cfg`; the bare
  `touch.layout.cfg` it used before never found it, and the CSQC's schema
  check then saved the defaults over it. The menu's Touch tab saves the layout
  whenever the menu closes, so its sliders need no Save press. A schema bump
  (`TOUCH_LAYOUT_VERSION`) still resets positions, but keeps look speed,
  sensitivity, handedness and the chosen preset (`TOUCH_MIGRATE_KEEP`).

A layout lost to the old behaviour cannot be recovered: the defaults were
written over both `touch.layout.cfg` and `touch.layout.bak`.

This depends on both builds being signed with the same key. Android refuses an
update whose signature changed, and the only way out of that is uninstalling,
which takes the data with it. See **Signing** below.

## Performance on ARM phones

- **Per-frame vertex data goes through an unsynchronized map.** Every
  animated model, particle and HUD shape is streamed each frame into one
  buffer (`R_BufferData_Store`), with draws from it between the writes. With
  `glBufferSubData`, a tile-based GPU (Mali above all, which runs every draw
  at the end of the frame) copies the whole buffer before each write: the
  Galaxy Tab Active5 (Exynos 1380, Mali-G68) ran the benchmark demo at 7 fps,
  with the game thread mostly idle and the driver's worker thread busy
  copying. The writes now use `glMapBufferRange` with
  `GL_MAP_UNSYNCHRONIZED_BIT` (ES 3 and desktop GL 3, looked up at run time;
  `gl_mapbufferunsync 0` or `-nomapbufferrange` goes back), and the same demo
  runs at 125 fps. The console says "Dynamic buffers: unsynchronized
  glMapBufferRange".
- **DDS textures are decoded properly on GPUs without S3TC**, which is most
  phone GPUs. Xonotic ships most textures, the particle atlas among them, only
  as DXT. The engine's old fallback coloured each 4x4 block with the average
  of its two endpoints, so the soft-edged sprites behind every explosion,
  flame and flash came out as half-opaque boxes, and solid textures washed
  out. `gl_textures.c` now decodes DXT1/3/5 texel by texel, at the same size
  as before (two mip levels below full, or the picmip level if smaller).
  The DXT textures the phone packs keep (ASTC was bigger only because KTX
  entries are stored without zstd) are still decoded that way: on stormkeep
  132 of them, 22 ms of decoding and 19 MB uploaded; at full size
  (`r_texture_dds_swdecode_reduce 0`) they would take about 300 MB.
  `r_texture_astc_stats` prints both counts.
- **What a game loaded late is loaded under the next loading screen**
  (`touch_preload`, `engine/darkplaces/touch_preload.h`). Lazy loading
  (`mod_lazyload`, `snd_lazyload`) took the stormkeep load on the Tab Active5
  from 11.4 s to 3.7 s but left the frame where the bots join at 240-270 ms
  (their player models, LODs and first weapons load through `setmodel`).
  Every model, HUD picture, sound and GLSL permutation loaded on first use in
  a game is written to `<userdir>/data/touch_preload/<map>.txt` and loaded
  before the first frame of the next game on that map (a built-in list on a
  map's first game): the load then takes 4.6-5.0 s and no frame in the first
  10 s goes over about 65 ms except the bots' arrival (server code and the
  first draw of their models, as with lazy loading off). `touch_preload 0`
  turns it off, `touch_preload_budget` caps it (ms), `touch_preload_keep`
  says after how many games an unused entry goes, `developer_loading 1`
  prints each late load.
- **The particle atlas is sized from the texture that was made**, not from
  `image_width`, which the DDS loader never sets.
- **`touch_perf_version 5`** puts `cl_particles` back on and `r_buffermegs_*`
  back to their defaults, once per install. Players turned particles off
  while the renderer was slow, and the engine doubles and archives
  `r_buffermegs_*` whenever a frame overflows them, which made each copy
  above bigger still.

- **The first start picks a profile for the phone** (`DeviceProfile.java`).
  It reads the SoC (`Build.SOC_MANUFACTURER`/`SOC_MODEL` on Android 12+,
  `Build.HARDWARE`), total memory and the fastest core's clock from the
  system, with no input involved, and sorts the phone into a tier:
  - low: under 3.5 GB, a fastest core under 1.9 GHz, or fewer than 4 cores.
    Gets the 30 fps saver, because a phone that cannot hold 60 stutters
    instead of pacing evenly.
  - high: 7.5 GB and 2.8 GHz or more. Gets balanced with full-size textures.
  - mid: everything else. Gets balanced.

  The facts go to the engine as `_android_soc`, `_android_hardware`,
  `_android_ram_mb`, `_android_cpu_mhz`, `_android_cores`, `_android_exynos`
  and `_android_tier`, and to logcat ("Device: ..."). After the first start
  the player's choices stand.
- **Performance hint (ADPF), off by default.** With `sys_perfhint 1`, on
  Android 13+ the engine opens a performance hint session for its main thread
  and reports each frame's work, without the vsync wait inside the buffer
  swap, against the frame budget (`cl_maxfps`, else the panel's refresh). Off
  until it is measured on the target devices; the console says "Performance
  hint: session open" when it is on.
- **Sustained performance mode is not requested.** `XonoticActivity` no longer
  asks for it, pending the same measurement.
- **The game thread runs at high priority** (`SDL_SetThreadPriority`).
- **libmain.so is built** with hidden visibility, no semantic interposition,
  ThinLTO, gc-sections and ICF (`android/app/src/main/cpp/CMakeLists.txt`).
  Only `SDL_main` and the JNI entry points are exported.

## Debugging on a device

A phone has no launcher command line, so `XonoticActivity` appends anything in
`<basedir>/xonotic.args` to the engine's arguments — whitespace separated, `#`
for comments. `-developer` routes the engine's console to logcat (tagged
`app_process`, because that is SDL's `argv[0]`), and `-condebug` writes
`<basedir>/userdata/data/qconsole.log`, which `adb shell cat` can read whole:

```bash
adb shell "echo '-xonotic -condebug' > /storage/emulated/0/Android/data/io.github.dixonsolutions.godnotic/files/xonotic.args"
```

## Known gaps

* **Player IDs** (d0_blind_id): `scripts/android-deps.sh` builds
  `libd0_blind_id.so` and `libd0_rijndael.so` from `engine/d0_blind_id` with
  GMP 6.3.0 linked statically (portable C, its symbols hidden; about 0.4 MB per
  ABI), `crypto.c` dlopen()s them, and Xonotic's public key `key_0.d0pk` ships
  at the root of the APK's data bundle (the basedir). The GMP source (2 MB,
  checked against its SHA-256) comes from ftp.gnu.org or gmplib.org, and its
  configure needs `m4` on the build host.
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
keytool -genkeypair -v -keystore release.keystore -alias godnotic \
  -keyalg RSA -keysize 2048 -validity 10000
```

## The phone pack set

`scripts/repack-phone-assets.py`, `scripts/pack-phone-archive.py` and
Godnotic-data's `slim-data.yml` publish an ASTC/zstd/Opus repack of the
pinned full data to that repository's stable `data-slim` release
([PHONE_ASSETS.md](PHONE_ASSETS.md)): faster map loads, sharper textures on
GPUs without S3TC, and a much smaller download.

`android/app/src/main/assets/godnotic-data-url.txt` pins the archive:

```
https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim/Godnotic-data.zip
```

Its entries are `Xonotic/data/<official pack name>.pk3` -- the phone pack under
the name it installs as -- so `PackFetch` (the same range reader the autobuild
zip uses) installs them unchanged and `assetsMissing()` still finds
`-data.pk3` and `-maps.pk3`. No manifest parsing is needed at runtime; the
release carries `phone-manifest.json` for size/sha inspection only.

A GPU without `GL_KHR_texture_compression_astc_ldr` is the one case the set
does not serve: the engine then falls back to the DDS textures, which these
packs do not carry. The boot screen falls back to the autobuild archive when
the pinned one cannot be planned (HTTP error, no range support).

