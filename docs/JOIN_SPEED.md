# Join speed: from the tap to playable

Where joining a server and starting a local game spend their time, phase by
phase, what Godnotic changed to make them faster, and how to measure it.
The network side (windowed reliable channel, fast signon, parallel HTTP
ranges, reconnects) is in [NETWORK.md](NETWORK.md); local load work (lazy
loading, the menu warm-up, the per-map preload) in
[PERFORMANCE.md](PERFORMANCE.md). This page adds the timeline that ties them
together and the changes of `work/joinfast`.

## The timeline (`touch_jointime 1`)

`engine/darkplaces/touch_jointime.c` stamps each phase of a join the first
time it happens, in milliseconds from the start:

| event | when |
|---|---|
| `tap` | the front end's `fe play` (local game) or `fe connect` (server list) |
| `spawn_begin`, `spawn_end` | a local server loads its map (`SV_SpawnServer`) |
| `challenge_tx`, `challenge_rx` | `getchallenge` sent, `challenge` received |
| `connect_tx`, `accept` | `connect` sent, `accept` received: connected |
| `signon1` | the server's first message (serverinfo, precache lists) is parsed |
| `curl_begin`, `curl_end` | the server's own packs over HTTP (`curl --forthismap`) |
| `csprogs_begin`, `csprogs_end` | the server's CSQC, when it is not here: UDP download or HTTP (below) |
| `csqc_begin`, `csqc_end` | `CL_VM_Init`: CSQC loads and runs `CSQC_Init` |
| `precache_end` | models and sounds loaded, `prespawn` sent |
| `signon2`, `signon3`, `signon4` | the last signon steps (baselines, spawn, begin) |
| `first_frame` | the first game frame is drawn |
| `playable` | the first frame after which ten frames in a row took under 50 ms |

Without a tap, a join starts at the `connect` command or a local server's
spawn. The engine prints the table at the first frame and once playable
(`JOINTIME ...` lines) and keeps the last one in `_touch_jointime`
(`event=ms ... csprogs=local|cache|http|udp`); `_touch_jointime_serial`
counts finished joins. One clock read per event; nothing is printed unless
asked.

`scripts/bench-join.sh` runs a scenario N times on fresh userdirs (a
returning player: one game on the map played before) and prints, per event,
the median and the spread:

```sh
BASEDIR=<client basedir> LIBS=<dir with libjpeg.so.62> scripts/bench-join.sh -n 5 \
  local:stormkeep local:stormkeep:0.3 join:gn:100 join:stock:100 join:stock:100:csprogs=<dir>
```

`join:stock:...` uses a real Xonotic 0.8.6 dedicated server (the Flatpak's
`xonotic-dedicated` on its own data); `join:gn:...` this checkout's
dedicated server. RTT is `tc netem` delay on loopback in a user and network
namespace. Options: `cached` (the server's CSQC already in dlcache),
`pack=MB` (the server needs a pack over HTTP at `HTTP_KBPS`), `csprogs=DIR`
(DIR stands in for the CSQC release), `cskbps=N`.

## What a public server costs

Public servers are stock Xonotic: 243 answered on 2026-11-14, of them 82 on
0.8.6, 22 on 0.8.6 autobuilds, 54 on git builds, 6 on 0.8.5 (`qcstatus`).
A Godnotic client joining one of them pays for:

1. **The server's first message, stop-and-wait.** A stock server has no
   reliable window: the 22 KB of serverinfo, precache lists and stufftexts go
   one 1 KB fragment per round trip. 2.5 s at 100 ms RTT, 5 s at 200 ms.
   Nothing on the client can change that; it can only use the time.
2. **The server's CSQC.** Godnotic's bundled CSQC is its own build, so a
   stock server's must come from somewhere. DarkPlaces downloads it over
   UDP, one datagram (about 1.4 KB) per server tick; a 0.8.6 server ticks at
   30 Hz (`sys_ticrate 0.0333333` in its xonotic-server.cfg), so 40 KB/s
   whatever the link: the 0.78 MB deflated csprogs.dat takes 19-20 s on a
   perfect network and the 45-60 s seen on real ones once packets are lost
   (the server goes back to the first gap). The client's `rate` does not
   change it (measured: `rate 1000000` gives the same 19.4 s).
3. **Packs over HTTP** for servers with their own maps or mods
   (`sv_curl_defaulturl`), then the CSQC download after them, not alongside.
4. Local work: precache (the world model, CSQC init), the per-map preload,
   the first frame.

## What changed

### A stock server's CSQC over HTTP (`touch_csprogs`, `touch_csprogs.c`)

The CSQC of an official Xonotic release is a known file. When a server
announces one (`csqc_progsize` / `csqc_progcrc` stufftexts), the client
fetches `csprogs.dat.SIZE.CRC.deflate` from `touch_csprogs_url` (the
prerelease `csprogs` of this repository, published by
`.github/workflows/csprogs.yml` from the official release archive) instead
of asking the server:

- **From the first fragment.** The csqc stufftexts are the first bytes of
  the server's first message, and a stock server sends that message one
  fragment per round trip. A hook on the stock reliable channel
  (`TouchCSProgs_Peek`, one line in `netconn.c`) reads them from the
  partial message, so the fetch starts one round trip after `accept` and
  runs during the 20-odd round trips the rest of the message takes. At
  100 ms RTT the CSQC is in before the message is.
- **Trust nothing but the compiled-in hash.** The file must inflate (raw
  deflate, bounded to exactly SIZE bytes) to the SHA-256 compiled into
  `tcp_known`, and to the CRC. Anything else (404, a wrong or truncated
  file, no HTTP, slower than `touch_csprogs_timeout` (15 s) once the load
  waits for it) is thrown away and the server is asked, exactly as before.
  A build that failed is not tried again in the session.
- **Where the stock code looks.** The file goes to
  `dlcache/csprogs.dat.SIZE.CRC`, the name the server's own download would
  have written; `CL_BeginDownloads` and `CL_VM_Init` then find it and check
  size and CRC as for any download. One hook line in `CL_BeginDownloads`
  (`TouchCSProgs_Wait`) holds the load while a fetch runs.
- Known builds: Xonotic 0.8.6 (4021257 bytes, CRC 18349). Adding one:
  `scripts/csprogs-catalog.py OUT "label=<csprogs.dat or pk3>"` prints its
  `tcp_known` line and writes its file; the workflow refuses to publish a
  file whose hash the engine does not list. Autobuild and git servers each
  run their own build: those still download from the server.
- Off: `touch_csprogs 0`. `touch_csprogs_status` prints what it did.
  Web builds (no linked zlib) never fetch.

### The preload overlaps the last signon steps (`touch_preload_early`)

The per-map preload (models, pictures, shaders and sounds the last games on
the map loaded late) used to run at signon 3, in one piece, before the
first frame. Joining a remote server it now starts when the precache is
done (`prespawn` sent) and runs `touch_preload_slice` (15) ms per frame
while the server answers `prespawn` and `spawn`; whatever is left runs at
signon 3 as before. A local game is unchanged (its server answers in the
same frame).

### The join warm-up (`touch_warm_join`)

The menu's warm-up (`touch_warm`, PERFORMANCE.md) prepares the next local
game while the menu is idle. Joining a remote server, the client is idle
too while it waits: during a stock server's stop-and-wait first message it
already knows the map (the peek reads `maps/NAME.bsp` from the same
fragments), and during a CSQC download it has the precache list. The
warm-up then loads that map's preload list, a piece per frame: at signon 0
only in the first 40 % of each round trip after a fragment arrived (the next
fragment's acknowledgement is never held up), during a download within
`touch_warm_budget`. Never the world model (the precache's job: loading it
any earlier crashed in the lightmap upload and stalled the join in tests),
and never while the server's own packs download (they may replace what the
list names). The replay then finds the entries loaded.

## Results

(See the before/after tables below; desktop, SDL offscreen, `taskset -c
4-9`, split data set G3 plus this checkout's game code, a returning player,
median of 5.)

## What is still slow, and why

- **The stop-and-wait first message of a stock server**: 22 round trips. A
  stock server cannot be made to send faster; a Godnotic server sends it in
  one burst (NETWORK.md).
- **Servers on builds we do not have** (autobuilds, git): their CSQC still
  comes over UDP at 40 KB/s, 20 s at best. Their files could be added to the
  release only by fetching them from those servers (and each server updates
  its build at will).
- **Servers with their own packs**: 36 MB for the busiest Deathmatch server
  seen, 135 MB for a modded CTF one: that is the link (parallel ranges help
  on lossy links, NETWORK.md). The CSQC download still follows the packs
  instead of running alongside: a stock server has no way for the client to
  cancel a download, so starting it early would cost the 20 s again whenever
  a pack brings the CSQC itself (as `csprogs-*.pk3` servers do).
- **Local work on a phone**: CSQC init, the world model and the first frame
  are single-threaded loads; on the tablet the warm local start is 2.8-2.9 s.
