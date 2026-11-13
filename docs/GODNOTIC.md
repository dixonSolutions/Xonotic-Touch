# Godnotic client architecture and delivery record

Godnotic uses a DarkPlaces client, QuakeC game code and a bounded Lua plugin
runtime. This document distinguishes implemented work, measured evidence and
remaining work. It is not a claim that downloads or map loads take milliseconds.
Network throughput, pack size, storage latency and shader compilation set hard
lower bounds. Warm cache checks can be very short; a first 1 GB download cannot.

## Components and ownership

| Repository | Owns | Consumers |
| --- | --- | --- |
| `dixonSolutions/Godnotic` | Packaging, native frontends, web loader, release/update channels | Android, Flatpak, Ubuntu Touch, Pages, Vercel |
| `dixonSolutions/Godnotic-darkplaces` | Godnotic engine, renderer hooks, Lua sandbox | Main repository via a pinned component revision |
| `dixonSolutions/Godnotic-data` | Game code, configs, reproducible pack tools and manifests | Main repository and runtime content downloads |
| `dixonSolutions/Godnotic` release `data-slim` | The slim pk3 archive the app streams | Android, through `godnotic-data-url.txt` |

Imported upstream code keeps its license notices and source attribution. Stable
Android/Flatpak/OpenStore identifiers and AndySU keys are compatibility contracts,
not display branding. Renaming those in place would create a different application
and strand installed users. Public names, wordmarks, icons and repository URLs use
Godnotic; compatibility aliases remain until a separately tested migration retires
them. In particular existing live Android clients select an exact legacy APK name.

Engine and game code remain vendored in the main checkout while the split is
established. Component imports must use full commit IDs, preserve local work and
pass tests before release. Direct upstream Xonotic refreshes and pack rebuilds are
manual, rather than scheduled release producers. AndySU signature renewal remains
scheduled because installed clients reject expired manifests.

## Native UI

Android now has a Kotlin native console and client-plugin panel over the SDL game
surface. JNI places commands into a bounded resident queue; only the engine thread
pumps that queue into the engine command buffer. Native dialogs never access live
engine structures from Android's UI thread. The resident bridge survives an AndySU
engine replacement. New Android Application wiring needs a normal APK update on an
old frozen AndySU host before that host can create the native overlay.

Linux already used Kotlin/Native with GTK 4 and libadwaita, not a Rust loader.
Its mod switches and settings now save in order with unique temporary files,
retain enabled entries for unavailable folders, restore failed toggles, and reject
stale background scans. Keeping this working launcher avoids a second state machine.
A Rust service can be introduced behind a versioned protocol if a real need remains;
there is no Rust launcher to repair in the current source tree.

The Android console currently shows submitted-command history, not a live engine
log. The native panels cover console and plugins, not every QuakeC game widget.
Converting every menu, hosting, downloads, update screen and in-game widget requires
further view-model and event-schema work; it must not be described as completed.

## Plugins

The existing Lua 5.4 runtime supplies separate client and server realms, event
subscriptions and namespaced console commands. Memory, instruction and wall-time
limits disable runaway loops; errors disable that plugin while the engine continues.
This is bounded execution, not an absolute guarantee against every native engine bug.

Versioned manifest permissions and persisted user grants sit below the existing
realm allowlists. Importing a manifest never grants it privileges. See
[MODDING.md](MODDING.md) for the exact syntax, commands, limits and compatibility
behavior. Executable native plugins are not part of the Lua sandbox standard;
launching an arbitrary executable would require a separate process protocol and OS
isolation. File import already exists on Android. A cross-platform remote installer,
confirmation UI, uninstall and executable-plugin protocol remain work to complete.

## Browser choice and caching

The deployable browser client uses HTML/CSS native browser controls for loading and
QuakeC for in-game UI. GTK Broadway is a remote-display protocol using a running
GTK process, `gtk4-broadwayd` and WebSockets. It is not a client-only libadwaita
runtime that can be uploaded to static Pages. The browser retains its existing
cache namespace during rebranding; changing a cache name needlessly reinstalls
assets. A new web origin cannot directly read the old origin's private OPFS/IDB
storage, so keeping the old host alias is necessary for existing browser installs.

Downloads must identify immutable content independently of an app build. A failed
or canceled transfer may retain verified complete chunks, never a partially written
file under a completed cache key. A warm restart must issue no requests for cached
pack payloads. Data pruning runs only after all required files have mounted.

## Pack optimization

The previous repacker could expand already-compressed files and ASTC conversions.
The new default compares each candidate's compressed payload and filename overhead
with the original. If the candidate grows, it keeps the original compressed bytes,
name and content. DDS symlinks follow only conversions that were actually accepted.
This permits a mixed DDS/ASTC pack without dangling references. `--allow-growth`
explicitly selects the old conversion preference for GPU-load experiments.

Lossless zstd recompression can improve transfer and decode time without lowering
quality. ASTC, reduced mip sizes and Opus are separate quality/compatibility choices;
the web and devices without ASTC or Opus support must retain compatible formats.
Do not ship a phone pack as a universal pack without testing those decoders.

`--profile slim` is the set phones download: 10x10 world textures at most 512 px,
8x6 model and 6x6 normal-map blocks, 4x4 for the 2D graphics, 64 kbit/s Opus music,
and no nexcompat pack. `--profile full` keeps the desktop-sized defaults. The size
table in [PHONE_ASSETS.md](PHONE_ASSETS.md) records what the published set measures.

## Research references

- [Insanotic source](https://github.com/luohoa97/insanotic): disk-backed resumable
  downloads, bounded native UI state and pack verification are useful concepts;
  no upstream files are copied into Godnotic by hand.
- [Android JNI guidance](https://developer.android.com/ndk/guides/jni-tips): keep
  UI work on managed threads, minimize cross-thread shared state and distinguish
  JNI's modified UTF-8 from engine UTF-8.
- [GTK Broadway](https://docs.gtk.org/gtk4/broadway.html): server process and
  WebSocket requirement rules out static Pages hosting of this GTK backend.
- [GitHub repository renames](https://docs.github.com/en/repositories/creating-and-managing-repositories/renaming-a-repository): Git URLs redirect, project Pages
  URLs are an explicit exception. Update feeds and existing Flatpak remotes need
  validation during the rename.

## Verification and remaining acceptance criteria

| Area | Evidence / acceptance |
| --- | --- |
| Launcher core | JVM tests pass, including ordered toggles, failed saves and missing-folder entries |
| GTK launcher | x86_64 native build passes; ARM Linux runtime was unavailable locally |
| Android JNI | x86_64 APK builds; emulator enabled and disabled an actual Lua plugin through native controls |
| ARM Android | Must compile and exercise on the physical tablet; not inferred from an x86 emulator |
| Lua isolation | Actual engine harness exercises escapes, runaway loops and permission denial/revocation |
| Web downloads | Behavioral tests must cover cache reuse, canceled transfers and resuming verified chunks |
| Pack format | Round-trip bytes, checksums, symlinks, accepted and rejected conversions |
| Server join/rendering | A local server/client smoke and public-server/device joins are required; no unmeasured rendering-speed claim |
| Battery/performance | Compare identical maps, settings and frame caps; collect cold/warm time, RSS and power/thermal evidence |
| Ubuntu Touch | Confined launcher regression tests; actual UT hardware validation remains separate |

Full replacement of every widget, a Rust download supervisor, universal remote
plugin installation, executable plugin isolation, public server rendering A/B and
long-running battery measurements are not implied by the foundational changes.
