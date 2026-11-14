# Progressive data download

How every client (Android, the Linux loader, the web page) gets the split data
set of [PHONE_ASSETS.md](PHONE_ASSETS.md) ("Split set"), and how the running
game hears about it. One contract, three implementations.

## The manifest

`godnotic-data-<profile>.json` (`astc`, or `dxt` for a GPU without ASTC) sits
beside today's archive on the moving release: the client takes the directory
of its pinned `Godnotic-data.zip` URL and appends the manifest name.

A client uses the manifest only when all of this holds; otherwise it does
exactly what it did before (the single archive, by role):

- HTTP 200 and it parses as a JSON object with `"format": 1`,
- `set` is a non-empty string, `archive.url` is an http(s) URL, `archive.size`
  is positive,
- every pack has `name`, `install_as` matching `^godnotic-[A-Za-z0-9._-]+\.pk3$`,
  `size` > 0, an 8-digit hex `crc32`, a 64-digit hex `sha256`, a `priority`,
  and `offset + size <= archive.size`,
- every name in `first_play`, in each map's `needs` and in each pack's
  `depends` is a pack.

The accepted manifest is kept as `<data>/.godnotic-data.json` (written by
rename), so a later start needs no network to know what is installed and what
is left. It is fetched again at each start when a network is there; a failed
fetch keeps the stored one.

## Fetching a pack

The bytes of a pack are `archive.url`, `Range: bytes=<offset>-<offset+size-1>`
(always explicit offsets: GitHub's release storage answers suffix ranges with
501). They go to `<data>/<install_as>.part`; big packs in 8 MiB chunks over
several connections with the chunk state beside the part
(`<install_as>.part.state`, the `XTPC` format the single-archive path uses), so
a stop, a lost connection or a killed process carries on from the bytes on
disk. A whole part is checked against `size`, `crc32` and `sha256`, then
renamed to `<install_as>`. A part that fails the check is deleted and fetched
again (three times, then the run reports an error and carries on with the
other packs).

The archive is immutable (a new set is a new release and a new URL), so no
`If-Range` is needed; a `200` instead of `206`, or a `Content-Range` that does
not match, is an error for that attempt.

## Order

1. `first_play`, in list order. The game may start when every one of them is
   installed. This is the only part the boot screen waits for.
2. Everything else by `priority`, without asking: music, the other maps,
   `detail`, `extras`.
3. A *wanted* map (the player picked it, or a server runs it) jumps the queue:
   its `needs` that are missing go to the front, in order.

**A map's own pack is installed last**: it is renamed into place only when
every other pack in that map's `needs` is installed. So for a map of the set,
"`maps/<map>.bsp` exists" means "playable", which is what the menu and the
engine test.

## What is installed

Files are the truth; `<data>/.godnotic-set` is a cache of what was verified:

```
set 20261107-4c53098165d2
legacy 20261107-4c53098165d2
pack godnotic-20261107-core.pk3 64225800 c49b9836
...
```

A pack counts as installed when its file exists with the manifest's size and a
`pack` line with the manifest's size and crc32. A file of the right size with
no line (a kill between the rename and the save) is hashed once and adopted or
deleted.

### An install from before the split

The old layout is the official names: `xonotic-<date>-data.pk3`, `-maps.pk3`,
`-music.pk3`, `-xoncompat.pk3` (and `-nexcompat`, `font-*` from the stock
archive). It is *complete* when data, maps and music are all there.

- **Complete old install, first time a manifest is seen:** nothing is fetched.
  `legacy <set>` records the set id that install stands for. As long as the
  published `set` equals it, the client does nothing.
- **Complete old install, the published `set` differs from `legacy`:** the new
  set is fetched pack by pack in the background (order as above) while the old
  packs keep the game whole. When *every* pack of the new set is installed the
  old packs are deleted and the `legacy` line dropped.
- **Incomplete old install** (a first download that never finished): the
  parts and state files of the old layout are deleted at once; whole old packs
  are deleted when `first_play` is installed.

Never both gone: old packs are only deleted after the new packs that replace
them are verified and renamed.

### A new set

When the manifest's `set` differs from the `set` line: packs whose
`install_as`, size and crc32 are unchanged stay; changed ones are fetched to
`.part` and renamed over the old file; `godnotic-*.pk3` files (and their
parts) the new manifest does not list are deleted once `first_play` of the new
set is installed.

## Talking to the running game

The engine side is `engine/darkplaces/touch_datafetch.c`.

### Host to game

- `fs_mountpack <install_as>` for each pack that lands while the engine runs,
  where the host has a command channel (Android: `NativeBridge.enqueue`; web:
  `xt_web_command`; the loader: the UI-bridge backend's stdin).
- The status file `<engine write dir>/touch/data-status.txt` (the directory
  QuakeC's `fopen` reaches: `<userdir>/data/data/touch/`; where
  `update-status.txt` already lives), written to a temporary name and renamed.
  The engine reads it about once a second while it exists:

  ```
  format 1
  set 20261107-4c53098165d2
  state fetching
  done_kb 184320
  total_kb 363868
  want boil
  want_done_kb 2048
  want_total_kb 9216
  landed godnotic-20261107-map-boil.pk3
  landed godnotic-20261107-music.pk3
  ```

  `state`: `fetching`, `paused` (the player stopped it), `waiting` (no
  network, or waiting for Wi-Fi), `error` (with `message <text>`), `complete`,
  `legacy` (an old complete install; nothing to do). `done_kb`/`total_kb`
  count the whole set. `want*` describe the wanted map's missing `needs` and
  are absent when nothing is wanted. `landed` lists every pack installed since
  the host process started; the engine mounts those it has not mounted, so a
  host without a command channel (or a full queue) needs nothing else.
- A host with a command channel may send the same fields as
  `touch_data_status state fetching done_kb 184320 total_kb 363868 ...`
  instead of the file (the web page does).

The engine publishes them as read-only cvars for QuakeC and the native
interfaces: `touch_data_state`, `touch_data_done_kb`, `touch_data_total_kb`,
`touch_data_want`, `touch_data_want_done_kb`, `touch_data_want_total_kb`,
`touch_data_message`, and `fs_mountpack_count`.

### Game to host

`touch_data_want <map>` (the menu calls it when the player picks a map that is
still downloading; the engine calls it itself, below) writes
`<engine write dir>/touch/data-request.txt`:

```
want boil 3
```

The number grows with every request, so a host that polls the file (twice a
second) sees a repeated request for the same map. The host never deletes the
file; a request older than the host's own start is ignored. On the web the
engine calls `Module.xt.dataWant(map)` instead.

### Maps that are not here yet

A map of the set whose pack is not installed has its `.mapinfo` (in the core)
and no `.bsp`. While `touch_data_state` is `fetching`, `paused`, `waiting` or
`error`:

- `map <m>` / `changelevel <m>` for such a map does not start: the engine asks
  for it (`touch_data_want`), keeps the command in `touch_data_pending` and
  runs it when `maps/<m>.bsp` appears. `touch_data_cancel` drops it.
- A server whose map is such a map: instead of the "cannot load map" error the
  client disconnects, asks for the map, keeps `connect <address>` in
  `touch_data_pending` and reconnects when the map is there.
- The map list is the engine's `maps` list source (`touch_uib_src.c`), built
  from `maps/*.mapinfo`. Each row has `state` (`ready`, `downloading`, or
  `missing` when nothing is fetching it) and `progress` (0..1 for the wanted
  map, -1 while it is only queued); the source rebuilds when a pack is mounted,
  the downloader starts or stops, or the wanted map moves on. Choosing a
  downloading map asks for it (`touch_data_want`); starting one says that it
  is next instead of starting another map. `TouchData_MapState()` in
  `touch_datafetch.h` is the same for a native front end, and
  `qcsrc/menu/xonotic/touch_data.qh` for QuakeC (`TouchData_StatusLine()`
  gives "Getting the rest of the maps: 180 of 300 MB").
- Every renderer of the UI bridge (QuakeC, GTK, Kotlin) draws a map tile that
  is not `ready` dimmed with a label on its picture: `Downloading`, the wanted
  map's percent with a bar along the picture's bottom, or `Not downloaded`.
- While `touch_data_pending` holds a command, the menu shows the dialog
  `data.hold` (`touch_data.qc`): "Getting the map boil", then joining or
  starting, the wanted map's MB and bar, and Cancel (action `data.cancel`,
  which runs `touch_data_cancel`; the map keeps downloading with the rest).
  Back or a tap outside only hides it: the command still runs when the map
  is here.
- A join that waits for its map is not a failed join: `touch_joinfail.c` stays
  quiet while `touch_data_pending` holds the `connect`.

`fs_mountpack` also runs the music pack's `cdtracks.cfg` when the pack that
carries it lands (the configs exec it once at start-up, before the pack is
there), and `fs_rescan` counts in `fs_mountpack_count`.

On the web the page's own `needMap` hook does the first of these (it existed
before the split); it mounts with `fs_mountpack` instead of `fs_rescan`.

## Tests

- `scripts/test-datafetch-engine.sh SPLITDIR`: a headless engine on the
  `first_play` packs only; a second map landing while it runs; a join to a
  server on a third. Missing-file lines are compared with the whole set's.
- `scripts/test-datafetch.sh SPLITDIR [PHONEDIR]`: the loader's downloader
  against `tests/datafetch/linksrv.py` (explicit ranges, 501 for suffix ranges,
  one shared link of N Mbit/s): exact first-play bytes, SIGKILL and resume, a
  pack served damaged once, a whole install asking for nothing, a new set id,
  an old install left alone, the engine playing a late map while the rest
  comes in; `TIMINGS="5 20 100"` adds time to first play per link speed.
- `loader/crates/core/tests/fetch_manifest.rs` and
  `android/app/src/test/java/.../DataFetchTest.java`: the same rules on small
  made-up sets, in Rust and on the JVM.
- `scripts/test-web-cache.sh`: the web stager and page (docs/WEB.md).
