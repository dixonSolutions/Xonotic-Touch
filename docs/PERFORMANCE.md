# Performance: start-up and map loads

What makes Godnotic start and load quickly, how it was measured, and the switch
that turns each piece off. The first pass (lazy fonts, pictures, sounds and models,
the recorded per-map preload, cached loose-directory listings) is summarised in
[GODNOTIC.md](GODNOTIC.md) "Start-up and loading"; this page records the second.
`touch_loadtime 1` prints the phase tables a load spends its time in.

## Numbers

Desktop: Intel, Mesa, offscreen 1280x720, slim data, warm page cache, a returning
player (one earlier game on the map, so its preload list exists), stormkeep with 4
bots, the menu shown for 6 s before Play. Medians of 5 runs, spread in brackets.
"Pinned" runs the engine on two cores (`taskset -c 2,3`) to come closer to a
phone's few slow cores. The machine was shared with other builds while measuring:
single runs up to 5 s and worst frames of 100-350 ms appear in both arms of every
A/B, so compare medians.

| Scenario | First pass | This pass | |
|---|---|---|---|
| Tap Play to first game frame, menu idle first (warm Play) | 2398 ms (2071-3118) | 788 ms (688-3124) | unpinned |
| ... pinned | 4025 ms (3967-6469) | 2127 ms (1905-2493) | two cores |
| Tap Play straight away (cold Play), this pass's other switches on | | 1443 ms (1334-2106) | unpinned |
| ... pinned | | 3413 ms (3152-5575); warm 2449 ms (1394-3072) | two cores |
| Process start to menu | 616 ms (490-1123) | 644 ms (518-1463) | unpinned, no change |
| Worst frame in the first 5 s of play | 21.7 ms (19-131) | 23.9 ms (18-81) | unpinned |

The tablet (Galaxy Tab Active5) was not available for this pass; its numbers are
those in GODNOTIC.md.

## The switches

| Cvar (default) | What it does | A/B (desktop, unpinned) |
|---|---|---|
| `touch_warm 1` | While the menu has had no input for `touch_warm_idle` (2) s, loads the next local game's world model, its `touch_preload` list and the slow persistent pictures (`_touch_warm_pics`), a piece per frame within `touch_warm_budget` (12) ms. The map is `touch_warm_map`, else the last local map played (`_touch_warm_last`). | Play after idling: 1443 -> 847 ms; model load in the load 490 -> 73 ms |
| `touch_edicts_growth 1` | QuakeC edict storage grows to the size the VM needed before (`_touch_edicts_peak`), or by half, instead of 256 at a time. | server spawn 1035 -> 448 ms; load 1805 -> 1250 ms |
| `fs_zstd_lazy 1` | A zstd pack entry decodes on its first read, not at open: QuakeC's `fexists()` (open + close) of ~900 models and sounds per load no longer decodes them. | server spawn 759 -> 337 ms, CSQC init 438 -> 219 ms; load 1689 -> 958 ms |
| `touch_preload_early 1` | Joining a remote server, the per-map preload starts when the precache is done and runs `touch_preload_slice` (15) ms a frame while the server answers prespawn and spawn, instead of all at signon 3. Local games unchanged. | see [JOIN_SPEED.md](JOIN_SPEED.md) |
| `touch_warm_join 1` | While a join to a remote server waits on the network (a stock server's stop-and-wait first message, a CSQC download), loads the map's preload list (never its world model). | see [JOIN_SPEED.md](JOIN_SPEED.md) |
| `touch_load_fontslices 1` | Joining a remote server, the pending font sizes render 10 ms a frame during the signon's network waits instead of all in the frame after the accept. | see [JOIN_SPEED.md](JOIN_SPEED.md) |
| `touch_crc_fast 1` | CRC-16 of progs and csprogs (about 20 MB per map load) eight bytes at a time; identical result (`touch_crc_selftest`). | 4 MB: 31.5 -> 4.1 ms, so about 0.13 s less per load at that rate |

### touch_warm in detail

- Runs only on the menu with nothing connected and no local server, the window
  visible and focused, no console, not on battery saver (`touch_device_powersave`)
  and below `touch_warm_maxthermal` (2) of `touch_device_thermal`. The platform
  layer sets those two (Android: `PowerManager.isPowerSaveMode()`,
  `getCurrentThermalStatus()`); until it does they read 0.
- Any key, button or tap (Key_Event) stops it until the menu is idle again; one
  model is never split, so the world model is one long frame (about 0.5-1 s on
  a desktop) taken only after the idle time.
- Prepares a map once, then does nothing until the target changes or a game is
  played. `touch_warm_status` shows what it did.
- Memory: the prepared map is what a game on it holds anyway. Desktop RSS on the
  menu 227 MB without, 305 MB with; after any game the menu holds 502 MB, because
  the engine keeps the last map's assets loaded. Four games in a row return to the
  same 502 MB: no growth across map changes.
- Low memory: `SDL_APP_LOWMEMORY` (Android `onLowMemory`) or `touch_warm_drop`
  frees every model and sound no game uses and stops the warm-up until the next
  game.
- A different map or settings only means the prepared assets go unused: the end
  of that load frees them as it frees an earlier game's. Off by default on the web
  build (maps there are fetched on demand).

### Hooks in upstream files

Each is one line marked `// Touch:`.

- `cl_screen.c`: `TouchWarm_Init()` in `CL_Screen_Init`, `TouchWarm_Frame()` after
  each drawn frame, and the include.
- `keys.c`: `TouchWarm_Input()` at the top of `Key_Event`, and the include.
- `com_crc16.c`: `CRC_Block` returns `TouchCRC_Block`'s result when it computed one,
  and the include.
- `prvm_edict.c`: `PRVM_MEM_IncreaseEdicts` takes `TouchPRVM_EdictGrowth`'s size,
  and the include.
- `fs.c`: `Touch_FS_ZstdEnsure(file)` at the top of `FS_Read`'s data-file path,
  `Touch_FS_ZstdClose(file)` in `FS_Close`.
- `makefile.inc`: `touch_warm.o`, `touch_crc.o`, `touch_prvm.o`.

## Measuring

- `scripts/bench-load.sh` (first pass): start to menu, menu to map, joins.
- `scripts/perf-guard.sh`: menu, cold Play and warm Play for a returning player,
  medians against a baseline recorded on the same machine (`-r`), 15 % plus 40 ms
  allowed (`-t`, `-s`), `-p 2,3` to pin. Exit status 1 when something got slower.
  Run it before a release on a quiet machine; it is not a CI job, because a shared
  runner varies by more than the tolerance (see the spreads above). The worst-frame
  figures are the noisiest: another process taking the CPU for a moment shows up
  there first, so check an OVER worst frame with more runs (`-n 9`) before
  believing it.
- QuakeC: `prvm_timeprofiling 1`, then `prvm_profile server 40` and
  `prvm_profile client 25` right at the first game frame show which builtins a
  load spends its time in (that is how `_spawn` and `fopen` were found).
