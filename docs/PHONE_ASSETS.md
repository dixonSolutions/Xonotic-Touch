# Phone assets

The official Xonotic packs are made for desktop GPUs: every world and model
texture is DXT (S3TC) in `dds/*.dds`, entries are deflated, sound is Ogg
Vorbis. Phone GPUs have no S3TC, so the engine decodes every DXT texture in
software at map load and keeps it two mip levels down (docs/ANDROID.md):
blurry, and CPU time during a map load that takes ~25 s on a Galaxy Tab
Active5.

The **phone pack set** is the same data repacked for phones, made by
`scripts/repack-phone-assets.py` and published by the `slim-data.yml` workflow of
[Godnotic-data](https://github.com/dixonSolutions/Godnotic-data). The engine reads both: a phone pack is a
pk3 like any other, and everything it adds falls back to the official format.
The ideas come from [Insanotic](https://github.com/luohoa97/insanotic)'s
`repack-assets.sh`; the code here is written fresh (theirs is GPL-3.0+ on an
older DarkPlaces) and the libraries are vendored from upstream.

## What changes

| | official | phone pack | engine side |
| --- | --- | --- | --- |
| textures | `dds/foo.dds`, DXT1/3/5, deflated | `dds/foo.ktx`: ASTC in KTX 1 with the whole mip chain | `touch_assets_ktx.h`, uploaded as it is when the GPU has `GL_KHR_texture_compression_astc_ldr` |
| other entries | deflate | zstd (zip method 93) when it saves 2 %, else stored | `touch_assets_fs.h` + `touch_assets_zstd.c` (zstd 1.5.7 decoder in `touch_vendor/zstd/`) |
| music | Ogg Vorbis | Ogg Opus 96 kbit/s, same `.ogg` name | `touch_assets_opus.c` (libopusfile) |
| sound effects | Ogg Vorbis | Ogg Opus at a measured bitrate, or Vorbis (`--sfx opus`) | the same |

ASTC block sizes: 8x8 for world textures, 6x6 for `dds/models/`, 5x5 for
normal maps, 4x4 for the 2D graphics in `dds/gfx/`. World and model textures
over 1024 px start a level down (the 2048 px weapon and player skins); 2D
graphics keep their size, which is the size the HUD draws them at.
Everything is a script option.

Each hook into upstream code is one line marked `// Touch:`:

- `gl_textures.c`: includes `touch_assets_ktx.h`, registers its cvars, and at
  the top of `R_LoadTextureDDSFile` tries `dds/foo.ktx` first.
- `fs.c`: includes `touch_assets_fs.h`, flags method-93 entries in
  `PK3_BuildFileList`, and opens them in `FS_OpenPackedFile` by decoding the
  whole entry into a data file. Nearly everything is loaded whole anyway
  (`FS_LoadFile`), and what streams (music) is stored, so `FS_Read`, `FS_Seek`
  and `FS_Close` stay as they are. `FS_OpenPackedFile` also calls
  `Touch_FS_Trace` (`fs_opentrace`), and `FS_Init_Commands` includes
  `touch_fs_mount.h` and calls `TouchFS_Init_Commands` (`fs_mountpack`).
- `snd_ogg.c`: includes `touch_assets.h` and lets `Touch_Opus_LoadFile` take
  an `.ogg` whose first page is Opus (or a `foo.opus` when `foo.ogg` is
  missing).

Opus keeps the `.ogg` name because the QuakeC checks that a sound exists by
its `.ogg`/`.wav` name before it precaches it ("Missing sound"); a renamed
`.opus` would never be asked for.

Cvars and commands: `r_texture_astc_load` (1; 0 or `-noastc` loads the DDS
files), `r_texture_astc_log` (print every ASTC texture and every refused
`.ktx`), `r_texture_astc_stats`, `-noopus`.

## Measured

Official 0.8.6 packs (`xonotic-20230620-*`), repacked with the defaults:

| pack | official | phone | of which textures | zstd only (`--textures keep`) |
| --- | ---: | ---: | --- | ---: |
| data | 317.5 MB | 251.4 MB | 208.6 -> 151.8 MB | 293.4 MB |
| maps | 626.2 MB | 652.3 MB | 413.6 -> 446.5 MB | 594.5 MB |
| music | 110.7 MB | 71.2 MB | (Opus) | |
| nexcompat | 125.4 MB | 100.1 MB | 105.4 -> 80.3 MB | 121.7 MB |
| xoncompat, fonts | 5.3 MB | 4.6 MB | | |
| **total** | **1185 MB** | **1080 MB** | | |

The maps grow a little: their DXT1 deflates unusually well (2:1) and ASTC,
like any block format without rate-distortion tuning, hardly compresses.
`--world-blocks 10x10` or `--max-size 512` shrink them at a cost in
sharpness. A full repack takes 11 minutes on 16 threads.

`--profile slim` is what the app downloads, from the same source build
(`xonotic-20261107-*`), without nexcompat:

| pack | official | slim | textures | JPEGs | left out |
| --- | ---: | ---: | --- | --- | ---: |
| data | 312.2 MB | 108.2 MB | 204.8 -> 19.4 MB | 18.5 -> 9.9 MB | 6.1 MB |
| maps | 618.4 MB | 231.1 MB | 404.3 -> 85.8 MB | 126.9 -> 73.4 MB | 9.1 MB |
| music | 110.7 MB | 47.1 MB | (Opus 64 kbit/s) | | |
| xoncompat, fonts | 5.3 MB | 3.4 MB | | | |
| **total** | **1046 MB** | **390 MB** | | | |

ASTC entries are zstd-compressed like everything else (they shrink by about an
eighth). Before that they were stored raw, and because the size policy compares
each converted entry with its deflated DXT original, 986 textures that deflate
well stayed DXT: on a phone without S3TC those were decoded in software at a
quarter of their width and height. Compressed, the ASTC version wins for all but
272 near-flat textures and `particlefont.dds` (DXT2, which the encoder does not
take), and the set is 43 MB smaller.

A link follows its target to `.ktx`, including a link to a link
(`dds/gak.dds -> dds/textures/gak.dds -> gakfullbright.dds`).

### JPEGs and what is left out

The official lightmaps (`maps/<map>/lm_*.jpg`) are saved at quality ~99 and were a
third of the slim maps pack. The slim profile re-encodes JPEGs (`--jpeg-quality 86`,
`--lightmap-quality 90`), under three rules that came out of a failed first attempt:

- **The source's chroma sampling is kept.** Pillow's default halves the colour
  resolution, which bleeds neighbouring charts of a lightmap atlas into each
  other: seams and discoloured patches in game.
- **Deluxemaps are not lightmaps.** Under the odd numbers (`lm_0001`, ...) a
  deluxemapped map stores the light's direction as a colour. Those start at
  quality 95 with a stricter floor. They are recognised by their pixels being
  mostly unit vectors; five sparsely filled ones in the 20261107 set fall under
  that test and take the lightmap path (40.7 dB or better on their vector pixels).
- **Every re-encode is decoded and compared with its source** and used only at or
  above a PSNR floor (38 dB; deluxemaps 41 dB), trying qualities 93, 96 and 97
  first; otherwise the original entry stays. A file already at or below the
  target quality, or not a tenth smaller, stays too.

Left out, because a running game never opens them: map and model sources
(`maps/*.map`, `models/*.map`, `*.ase`), QuakeC line tables (`*.lno`, only used to
number lines in a QC error trace) and the two demos (`demos/*.dem`; Media > Demos
is empty with this set).

Checked in the engine against the full and the previous slim set: eight maps from
four cameras each with deluxemapping on, 44-51 dB against the previous slim set on
every still shot, no new log lines, all 10,005 entries decoding to their CRC.

The streamed archive measures the same 508 MB (its entries are stored), where
the previous download was the 1.17 GB autobuild archive: about 57 % less to
fetch and store. Textures are what is left; `--world-blocks 12x12` or
`--max-size 256` would cut further, at a clearly visible cost.

The tables above were measured on this source build and the machine that built
the release; a repack on the 24-core workstation took about 12 minutes with
`-j 8`.

Map load (client side: the Stormkeep benchmark demo, wall time minus engine
start and playback), desktop GLES2 build on Mesa Intel UHD 770, 1280x720,
`r_texture_dds_swdecode 2` to take the phone path for DDS:

| packs | load, two runs |
| --- | --- |
| official (software DXT decode) | 10.7 s, 10.9 s |
| zstd entries, DDS textures | 7.2 s, 6.9 s |
| zstd entries, ASTC textures (the phone set) | 5.7 s, 5.7 s |
| zstd, DDS, Opus for *all* sound | 10.5 s, 9.2 s |

Opus sound effects cost ~2 s of load (they are decoded to PCM at map load,
and Opus decodes at 48 kHz, which also made resident sound memory 10 %
bigger), so only the music, which streams, is Opus by default (`--audio all`
does the rest). The timedemo frame rate did not move beyond run-to-run noise
(330-400 fps here, CPU-bound).

Image quality, same demo frame (left: desktop DXT, middle: the current phone
path, right: ASTC phone pack):

![Desktop DXT, software-decoded DXT two levels down, ASTC](media/phone-assets-textures.jpg)

Whole-frame PSNR against the desktop frame: 30.3 dB for the software decode,
32.9 dB for ASTC. Per texture (77 sampled from data and maps, mean PSNR
against the full-size DXT decode): today's phone path 29.0-29.5 dB, ASTC 8x8
at full size 39.9-41.4 dB, ASTC 6x6 44.6-47.5 dB.

Not measured yet: a real phone. The desktop Mesa driver has no S3TC-less
mode and emulates ASTC; the BlissOS VM passes GL through to the same host
GPU. The numbers that matter are the Mali/Adreno load time and memory.

## Profiles

`--profile full` (the default) is the desktop-sized set above. `--profile astc`
(alias `slim`) is what the app downloads; `--profile dxt` is the same set for
GPUs without ASTC (desktop NVIDIA/AMD: the web build and desktop Linux choose
it by GPU), with the official DXT textures kept:

| option | full | astc (slim) | dxt |
| --- | --- | --- | --- |
| `--textures` | astc | astc | keep (DXT) |
| `--world-blocks` | 8x8 | 10x10 | - |
| `--model-blocks` | 6x6 | 8x6 | - |
| `--norm-blocks` | 5x5 | 6x6 | - |
| `--gfx-blocks` | 4x4 | 4x4 | - |
| `--max-size` | 1024 | 512 | - |
| `--music-rate` | 96k | 64k | 64k |
| `--sfx` | keep | opus | opus |
| `--bsp-zstd-level` | (19) | 22 | 22 |
| JPEG / lightmap re-encode | - | 86 / 90 | 86 / 90 |
| left out | - | editor files, `*.mp3` | editor files, `*.mp3` |

Any flag still overrides its preset value. 2D graphics, which the HUD draws at
1:1, keep 4x4 blocks and their size in both profiles; the sacrifices are in
the world, model and normal-map textures a small screen shows at arm's length.

Sizes from the pinned `20261107` source (13 min for astc, 11 min for dxt on 24
cores):

| set | data | maps | music | rest | total |
| --- | ---: | ---: | ---: | ---: | ---: |
| official (no nexcompat) | 312.2 | 618.4 | 110.7 | 5.3 | 1046 MB |
| astc, published 2026-11-13 | 108.2 | 231.1 | 47.1 | 3.4 | 390 MB |
| astc, with Opus effects | 97.9 | 222.0 | 46.9 | 3.4 | **370 MB** |
| dxt | 270.5 | 521.3 | 46.9 | 4.6 | **843 MB** |

### Sound effects (`--sfx opus`)

Every Vorbis effect (802 in data, maps and xoncompat) is tried as Opus at a
ladder of bitrates, mono 32-64 kbit/s and stereo 48-80, and the smallest rung
is kept that is as close to the source as Opus at a generous rate (96 mono,
128 stereo) within these bounds, all measured on the decoded PCM against the
source as libvorbisfile decodes it:

- mean band error over 25 critical bands +0.45 dB at most,
- 2 ms energy envelope +0.35 dB, total energy above 100 Hz 0.25 dB,
- no more than 1.5 dB extra energy before the first onset (pre-echo),
- a loop's seam no worse than 3 dB over the error inside it.

A sound no rung passes, or where Opus is not a tenth smaller, stays Vorbis.
On the 20261107 set 655 effects became Opus (29.7 -> 14.9 MB), 147 stayed
Vorbis; `phone-sfx.tsv` lists every file with its rung and margins. Stereo
whose side channel is 30 dB under the mid becomes mono (32 files): the mixer
plays an L == R stereo sound exactly as the mono one.

The Opus file is built from libvorbisfile's PCM resampled to 48 kHz (soxr),
and its Ogg stream is rewritten so it decodes to exactly round(n * 48000 /
rate) samples. ffmpeg's own Vorbis-to-Opus path came out up to 674 samples
short or 1411 long. LOOP_* comments are rescaled to 48 kHz samples and the
OpusHead input rate set to 48000. A whole-file loop (LOOP_START=0, or a
looped `target_speaker` in a map) is encoded with 100 ms of its end before it
and of its start after it, which pre-skip and end trim then drop, so the codec
sees the loop as continuous.

The music goes through the same exact path now. The old ffmpeg path kept the
LOOP_* comments in source samples but wrote 48000 as the input rate, so every
44.1 kHz track with loop points looped at the wrong place in the published
set (nanite.ogg: LOOP_END=8404942 read as 48 kHz samples cut its last 15.5 s).

Decode cost (desktop, one core, min of 3; a tablet core is ~4-5x slower):
libopusfile decodes the effects at about half the speed of libvorbisfile
(all 795 effects that are decoded whole, i.e. shorter than
`snd_streaming_length` 40 s: 5.2 s Vorbis, 9.7 s with the chosen mix; the
largest single one 85 -> 208 ms). The engine decodes small files on first
play and big Vorbis files on worker threads, but an Opus file over
`snd_lazyload_maxsize` (48 KiB) is decoded on the main thread at precache.
With every effect as Opus that put 0.7-1.4 s of decoding into a stormkeep or
erbium load on a desktop (4 cores; sound phase 70-190 ms with the published
set), several seconds on a tablet. So `--sfx-precache-kib 48` keeps Vorbis
for an effect whose Opus file would be over 48 KiB and that is short enough
to be decoded whole (30 files, 2.5 MB more); set it to 0 once the engine
decodes Opus on its worker threads too.

### Split set (`scripts/split-data-packs.py`)

The download is cut so a client can play after a small part of it:

| pack | astc | dxt | what |
| --- | ---: | ---: | --- |
| core | 63.3 MB | 203.3 MB | what the menu and any match opened (all but ~0.5 MB is opened by every match: the QuakeC precaches every registered model and sound), all 2D pictures of the menu and HUD skins in use (`--skins luma`; drawn on demand, so a trace misses rare ones), every mapinfo, level shot and shader script, fonts, xoncompat |
| map-stormkeep + its shared packs | 10.3 MB | 24.6 MB | the first map |
| 30 other maps | 0.7-15.5 MB own | | bsp, lightmaps, waypoints, what only that map opened |
| shared-N | ~40 packs | | files several maps opened, grouped by which maps use them |
| music | 46.9 MB | 46.9 MB | the music pack, whole; fetched right after the first map |
| detail | 7.5 MB | 7.5 MB | player LOD meshes (the QuakeC uses them only if they exist) |
| extras | 83.7 MB | 265 MB | what no traced run opened: stock texture sets no stock map uses (community maps do), other HUD/menu skins, overkill models, the official progs (the apps ship their own) |

First play (core + stormkeep and its shared packs) is 73.6 MB of 370 MB for
astc, 227.9 MB of 843 MB for dxt. No entry is in two packs and the union
is the unsplit set (the one name two official packs share,
`scripts/shaderlist.txt`, keeps the copy the engine used).

Which map opens what comes from traces, not guesses: `fs_opentrace <file>`
logs every pack entry the engine opens, and `scripts/test-data-split.sh
--trace` runs the menu and a bot match on every map (and the other gametypes
and mutators on a few) to build the usage table
`scripts/data-usage-<date>.tsv.gz`. A file several maps opened goes to a
shared pack; shared groups merge while that makes maps fetch less than 3 % of
the shared bytes they do not need. Maps are listed by the menu from their
mapinfo in the core whether or not their pack is in.

`scripts/test-data-split.sh SPLIT PHONE` checks the manifest, archive byte
ranges, union and duplicates; with `ENGINE=` it also loads every map from the
core and that map's needs only, and fails if anything the same match opened on
the whole set is missing, or a "not found" line appears that the whole set
does not print.

A pack that arrives while the game runs is mounted with `fs_mountpack
<name.pk3>`: it goes to the end of the search path, so it adds files and never
shadows one (the app's own progs and configs in `xonotic-data.pk3dir` stay in
charge). `fs_mountpack_count` counts mounts so the menu can refresh its map
list.

`godnotic-data-<profile>.json`:

```json
{
 "format": 1, "set": "20261107-<hash>", "meta": {...},
 "default_map": "stormkeep",
 "first_play": ["core", "shared-1", ..., "map-stormkeep"],
 "maps": {"stormkeep": {"pack": "map-stormkeep", "needs": ["core", "shared-1", ..., "map-stormkeep"]}},
 "packs": [
  {"name": "core", "file": "godnotic-20261107-core.pk3", "install_as": "godnotic-20261107-core.pk3",
   "kind": "core", "size": 57880000, "sha256": "...", "crc32": "...", "priority": 0, "entries": 2362,
   "depends": [], "offset": 39},
  {"name": "map-stormkeep", "kind": "map", "maps": ["stormkeep"], "depends": ["shared-1", ...], ...}
 ],
 "archive": {"file": "Godnotic-data-astc.zip", "size": ..., "sha256": "...", "folder": "Xonotic/data/",
             "url": "<immutable release URL, set when published>"},
 "left_out": ["data:scripts/shaderlist.txt"]
}
```

`priority` is the fetch order (lower first); `offset` and `size` are the
pack's bytes in the archive, so one Range request fetches a pack without
reading the zip directory. A map is playable when every pack in its `needs` is
installed.

## Building and publishing

    scripts/repack-phone-assets.py --profile slim -o out data/*.pk3   # 3.14, numpy, ffmpeg, astcenc
    scripts/pack-phone-archive.py -o Godnotic-data.zip out/*-phone.pk3
    scripts/test-phone-assets.sh                                     # self-checks, run by tests.yml
    scripts/test-phone-archive.sh

`slim-data.yml` in Godnotic-data runs on demand (publishing data is a decision,
not a schedule). It takes that repository's pinned full release (`data-<date>`,
never a moving upstream build), repacks every pk3 except nexcompat with
`--profile slim` using the scripts here, and uploads to that repository's stable
`data-slim` release (prerelease, never "latest"):

- `Godnotic-data.zip`: the archive the app streams, entries stored under
  `Xonotic/data/` with the official pack names,
- `Godnotic-data.zip.sha256`,
- `phone-manifest.json`,
- `phone-sizes.md`.

The tag stays `data-slim` and its assets are replaced, so
`android/app/src/main/assets/godnotic-data-url.txt` never changes. A new
archive has a new ETag and length, which is how `PackFetch` notices and drops
the parts of the old one.

`phone-manifest.json`:

```json
{
 "format": 1,
 "meta": {"source_url": "...Xonotic-latest.zip", "source_etag": "...", "built_from": "<sha>",
          "base_url": "https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim/"},
 "packs": [
  {"file": "xonotic-20261107-data-phone.pk3", "size": 251424941, "sha256": "...",
   "install_as": "xonotic-20261107-data.pk3",
   "source": {"file": "xonotic-20261107-data.pk3", "size": 320746799, "sha256": "..."},
   "textures": "astc", "audio": "music", "profile": "slim"}
 ]
}
```

The Android app installs each archive entry under the name it carries: the
engine loads whatever pk3 has that name, and the existing "is
`xonotic-*-data.pk3` there" checks keep working. The set replaces the whole
download; mixing a phone data pack with an official maps pack works too, it is
just bigger.

**Only a GPU with ASTC can use the set**: it carries no DDS files, so without
`GL_KHR_texture_compression_astc_ldr` world and model textures would be
missing. Every Mali since the T6xx, every Adreno since the 4xx and PowerVR
Series6 have it; check the GLES extension string before choosing the set.
The Opus music needs libopusfile, which the Android build links; desktop
builds load it at run time if the system has it.

The workflow that builds and publishes the sets (profiles astc and dxt, build
artifacts first, publish a chosen run second, immutable
`data-<profile>-<date>-rN` releases plus the moving `data-slim` aliases) is
`.github/workflows/slim-data.yml` in Godnotic-data; its header says how.

## Left to do

- Measure the slim set on the Galaxy Tab (load time, memory, fps) and tune the
  block sizes against how it actually looks.
- Opus effects on the worker-thread decode path, and the lazy-load threshold
  by decoded length instead of file size (an Opus file holds about twice the
  audio per KiB that Vorbis does).
- Not adopted, measured: music at 56 or 48 kbit/s (-5.8 / -11.6 MB, band error
  +0.08-0.13 / +0.15-0.33 dB over 64k on every track: no listening test, so
  64k stays); zstd 22 and long mode for models and textures (0.0-0.01 MB);
  dropping deluxemaps (-40.9 MB, every stock map is deluxemapped: visible
  wherever `r_glsl_deluxemapping 1`, and the engine would need a lightmap
  renumbering hook); ASTC lightmaps (not smaller than the re-encoded JPEGs at
  ~1.7 bit/pixel, would save VRAM only, needs a q3bsp loader change).
- An old install that already downloaded the official packs keeps them:
  `PackFetch` matches them by role, and the names are the same.

## Godnotic size policy

The default repacker retains the original compressed entry whenever a converted
entry would be larger, including its name and symlink targets. This fixes stored
Ogg expansion and keeps unsuitable ASTC conversions as DDS. `--allow-growth` is
an explicit GPU-load tradeoff for experiments; it is not the default distribution
policy. Manifests record `size_policy` and `astc-mixed` when applicable.
