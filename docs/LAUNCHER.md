# The Linux launcher

`launcher/` is Godnotic's GTK 4 / libadwaita launcher for the desktop Flatpak
(and any Linux package that ships `xt-launcher`). It is Kotlin Multiplatform:

| Part | What it is |
| --- | --- |
| `launcher/core` | Common Kotlin with no UI: settings, launch arguments, mods, profiles, pk3 map lists, and the **pack downloader** (`xt.core.fetch`). Targets `jvm()` (tests) and `linuxX64()` / `linuxArm64()`. |
| `launcher/app` | Kotlin/Native GTK app calling the C API through cinterop: pages Play, Data, Mods, Host, Settings in `app/src/gtk/kotlin/xt/ui/`. |

The launcher picks settings, then `exec`s `bin/start.sh` (which stays the one
place that prepares a game session) with the choices as environment and `+cvar`
arguments. It also downloads the game data itself (the **Data** page).

## Pages

- **Play**: the Godnotic banner, the data status, and PLAY. While the data is
  missing, PLAY is replaced by *Download Game Data*, which opens the Data page;
  `Ctx.play()` also sends the player there instead of starting a game without packs.
- **Data**: which archive, why, how much to fetch, free space, the data folder;
  progress (percent, MB done of total, speed, time left); Download / Pause /
  Resume / Cancel / Try Again / Play; *Discard partial download*. Cancel stops
  and then asks whether to keep the downloaded part (default) or discard it.
  Errors are shown as the plain sentence the downloader produced.
- **Mods**, **Host**, **Settings**: unchanged, except Settings' *Game data* row
  now leads to the Data page.

Narrow windows (under 640 sp, `AdwBreakpoint`) move the page switcher to a
bottom `AdwViewSwitcherBar` with 64 px buttons and grow rows to 64 px. Rows are
at least 56 px and buttons 40-56 px at any width. The banner and icon are built
into the binary (`embedBranding` in `app/build.gradle.kts` turns
`branding/godnotic-banner.png`, scaled to 800 px, and `godnotic-icon-256.png`
into a generated `Embedded.kt`), so every package shows them without installing
extra files. The window's icon name is the app id, which the Flatpak installs.

## The pack downloader (`launcher/core/.../fetch/`)

A port of the Android app's `PackFetch.java`, writing the same files, so one
data folder can be resumed by either.

The release zip is never stored. Its central directory comes from its last
bytes, and each `Xonotic/data/*.pk3` is fetched from its own byte range.

**Plan** (`PackFetch.plan`)
1. `Range: bytes=0-0` learns the size (`Content-Range`) and the `ETag`.
2. The tail (`max(0, size-65557)..size-1`) is fetched by explicit offsets.
   GitHub's release storage answers suffix ranges (`bytes=-N`) with 501.
3. The end-of-central-directory record gives the directory, read from the tail
   or with one more range. zip64 is refused with an error.
4. Only `Xonotic/data/<name>.pk3`, one level deep, never a name starting with
   `.` or holding `/` or `\`.

**Run** (`PackFetch.run`)
- Stored entries (the big packs) are cut into 8 MB chunks that 8 connections
  fetch at once, each writing at its own offset into `<name>.part`.
- Each chunk's progress is saved to `<name>.part.state` about once a second and
  on stop, so Pause, a dropped connection, closing the launcher or `kill -9`
  resumes from the bytes on disk. The format matches the Android app's
  `DataOutputStream` (big-endian): int `0x58545043` ("XTPC"), long compressed
  size, int chunk count, one int per chunk.
- Deflated entries (the small font packs) are inflated as one stream, which
  starts over after a break.
- Before a part is renamed into place, its size and CRC32 must match the
  directory. A mismatch removes the part and its state and ends the run with
  "Damaged download of X; Retry fetches it again".
- `.packs-setup` pins the archive while a setup runs. The first two lines are
  the ETag and the size, then `want <role>` and `done <file>` lines. A plan
  with another identity discards all `*.pk3.part*` files and every pack that
  setup finished whose name and size the new archive does not have.
- Every range request carries `If-Range: <etag>`. An HTTP 200 (the server sent
  the whole new file) or another ETag means `ArchiveChanged`. The UI drops the
  plan, and Try Again plans the new build.
- Each chunk or stream gets 10 attempts, with a back-off of 2 s times the
  attempt number, capped at 15 s. 4xx errors other than 408/429, damage, no
  space and a changed archive are not retried.
- Free space must cover what is left to unpack plus 64 MB, or the run stops with
  `NoSpace` before fetching anything.
- Roles: a build date in a name is a wildcard
  (`xonotic-20261107-maps.pk3` and `xonotic-20261029_2-maps.pk3` both play
  `xonotic-*-maps.pk3`), so an installed pack of any date counts.

**Platform interfaces** (`Platform.kt`): `RangeHttp` (one GET with headers;
head and body callbacks; must return within about a second of a cancel),
`FileSystem` (+ `RandomFile` with positional read/write), `RawInflater`, and
`Threads` (lock, thread, sleep, clock). CRC32 is common Kotlin.

| | JVM (`jvmMain`, tests) | GTK app (`app/.../NativeFetch.kt`) |
| --- | --- | --- |
| HTTP | `HttpURLConnection`, closed on cancel | libcurl (follows redirects, HTTPS, low-speed timeout 30 s, xferinfo callback for cancel); CA bundle from the usual paths |
| Files | `RandomAccessFile` / NIO | POSIX `open`/`pread`/`pwrite`/`fsync`, `statvfs` |
| Inflate | `java.util.zip.Inflater(nowrap)` | zlib `inflateInit2(-15)` |
| Threads | `Thread`, `ReentrantLock` | pthreads, `pthread_mutex` |

libcurl, zlib and libepoxy come from the GNOME runtime (`xtnative.def`; the SDK
headers are included with `-idirafter` so its glibc headers do not shadow the
Kotlin/Native sysroot).

## Which archive: the GPU probe

| Set | URL | When |
| --- | --- | --- |
| Slim (ASTC textures) | `.../releases/download/data-slim/Godnotic-data.zip` | the GPU has `GL_KHR_texture_compression_astc_ldr` |
| Full | `.../releases/download/data-20261107/Godnotic-data-20261107.zip` | anything else, or unknown |

The engine loads a pack's `.ktx` ASTC textures only when its own GL context
has that extension (`touch_assets_ktx.h`). The launcher asks the same question
before the engine runs (`Gpu.kt`): it creates a `GdkGLContext` from the launcher
window's own surface, so the display, driver and GPU are the ones the game will
get. It tries desktop OpenGL first, because the desktop engine (`darkplaces-sdl`)
uses desktop GL and a driver's GL and GLES extension lists can differ, then
OpenGL ES. It makes the context current and asks libepoxy
(`epoxy_has_gl_extension`, `glGetString(GL_RENDERER)`).

Why not the alternatives:
- An EGL pbuffer would need a cinterop for EGL and could choose a different
  GPU than the window on hybrid laptops.
- Reading `/sys` or a PCI id list cannot tell what the driver exposes.

No GL at all (Broadway, a broken driver) leaves ASTC unknown, and the full set
is used. The result is logged (`xt-launcher: GPU: ...`) and shown on the Data
page (*Graphics* row, and the *Data set* subtitle gives the reason). llvmpipe
reports ASTC (Mesa decodes it in software), and so would the engine, so the slim
set also draws correctly there.

Overrides:
- The Data page's *Data set* row: Automatic, Slim or Full (`data_set` in
  `~/.config/xonotic-touch/launcher.conf`).
- `XT_DATA_SET=slim|full|<archive URL>` in the environment wins over the
  setting (used by the tests and screenshots).

An automatic slim choice whose archive answers 404 falls back to the full set
and says so. When this was written, the `data-slim` release had no
`Godnotic-data.zip` asset yet. A slim set chosen by hand reports the 404
instead.

## Where the data goes and the shell fallback

Packs go to `PathResolver`'s `userData` = `<USER_BASE>/data`, the same folder
`packaging/start.sh` computes, and one of the engine's data dirs:
- Flatpak: `$XDG_DATA_HOME/xonotic-touch/data`
- otherwise: `~/.local/share/xonotic-touch/data`
- or `XONOTIC_TOUCH_USER_BASE`, or the launcher's custom folder setting

`packaging/start.sh`, `scripts/lib/asset-fetch.sh` and
`scripts/fetch-assets-posix.sh` are unchanged. They remain the downloader for
anything started without the launcher: one curl of the whole archive, then
unzip. To make the two agree:

- After a complete run the launcher writes `<data>/.assets-ready`, which every
  shell path checks first. It also writes `<data>/touch/music-ready.txt` when
  the music pack is installed. The bash check would otherwise also want a
  `nexcompat` pack.
- While it downloads, the launcher holds the same exclusive `flock` on
  `<data>/touch/fetch.lock` that start.sh's downloader holds. If the shell
  downloader is running, the launcher says so and does not start.
- The shell's partial archive (`<data>/.fetch-tmp/godnotic-data.zip`) is not
  reused by the launcher, and the launcher's `.part` files are not reused by the
  shell. Each resumes only its own.

## Command line

```
xt-launcher                 the launcher (or the game at once if "Open the launcher" is off)
xt-launcher --page=data     open on the Data page
xt-launcher --download      open the Data page and start downloading
xt-launcher --fetch         download without a window, progress on stdout; Ctrl-C/SIGTERM
                            stop and keep the parts; exit 0 done, 1 failed, 130 stopped,
                            3 shell downloader running. No window means no GPU probe:
                            full set unless XT_DATA_SET or data_set says otherwise.
xt-launcher --snapshot=FILE --size=WxH [--snapshot-delay=MS]   render the window to a PNG
```

## Build and test

```sh
# Core tests (what CI runs first): JVM, against a local Range/If-Range/ETag server
cd launcher && ./gradlew --no-daemon :core:jvmTest

# The binary (finds org.gnome.Sdk under ~/.local/share/flatpak)
bash launcher/build.sh        # -> launcher/dist/xt-launcher-x86_64 (and -aarch64 with that runtime)
```

`core/src/jvmTest/.../RangeServer.kt` stands in for GitHub's release storage.
It serves a generated zip of stored and deflated pk3s plus entries the fetcher
must skip, answers suffix ranges with 501, honours `If-Range`, counts served
bytes, and can slow down, corrupt a byte, fail with 503, or switch to a new
build mid-run. `PackFetchTest` covers:
- plan and filtering
- a full download
- music optional
- a stop with resume that does not refetch done bytes
- a kill with stale state
- an archive changed mid-run (`ArchiveChanged`, old parts discarded)
- a finished pack of the old build being replaced
- a CRC failure (part removed, the retry fetches it again)
- 503 retries
- 404, and the slim-to-full fallback
- no space
- roles
- the chunk-state format
- CRC32 against `java.util.zip.CRC32`

Without a display, the GTK binary runs in the GNOME runtime:

```sh
flatpak run --filesystem=host --share=network --command=$PWD/launcher/dist/xt-launcher-x86_64 \
    org.gnome.Platform//48 --fetch
xvfb-run -a -s '-screen 0 1600x1000x24' sh -c 'flatpak run --filesystem=host --share=network \
    --socket=x11 --env=GDK_BACKEND=x11 --env=DISPLAY=$DISPLAY \
    --command=$PWD/launcher/dist/xt-launcher-x86_64 org.gnome.Platform//48 \
    --launcher --size=400x800 --snapshot=/abs/path/shot.png'
```

The Flatpak sandbox's `/tmp` is private, so give `XONOTIC_TOUCH_USER_BASE` a
folder under `$HOME` when testing.

## What remains

- Pause, Resume, Cancel's keep-or-discard dialog and *Discard partial download*
  were built and compiled but not clicked in a test. The headless screenshots
  cannot click. Stopping by closing the window (the same cancel path) and
  `--fetch` with SIGINT and `kill -9` were exercised.
- The aarch64 build (`linuxArm64`, cross-compiled with the aarch64 GNOME
  runtime) was not built here: no aarch64 runtime is installed on the dev
  machine. CI builds it. `xtnative.def` uses only arch-neutral headers.
- The slim archive (`data-slim/Godnotic-data.zip`) was not published yet, so a
  real slim download is untested. Its `phone-manifest.json` installs packs
  under the full set's names, so roles match.
- Ubuntu Touch's click starts `start.sh` directly. Using the launcher there
  would need a launcher binary in the click and a GLES-first probe order.
- The menu's in-game download wizard and the launcher's Data page are separate
  UIs. The wizard only runs when start.sh finds no `.assets-ready`.
