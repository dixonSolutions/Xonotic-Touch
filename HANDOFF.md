# HANDOFF: work/srvlist2, the server list's first open beats Insanotic

Branch `work/srvlist2` (also pushed as `ci/srvlist2`), based on `origin/main`
f617cab. Clone: `~/.cache/godnotic-work/srvlist2`. Do not run component-push
from here; the coordinator merges and releases. Full write-up:
`docs/NETWORK.md`, "The server list" (the seed, the first wave, Measured).

## What Insanotic does differently (read from `~/Projects/insanotic/darkplaces/netconn.c`)

- `net_slist_querywait 0`: servers are asked from the first master answer on.
- `net_slist_burst 1`: up to `net_slist_queriesperframe` (100) queries every
  frame, in the masters' order, no pacing on the clock, no spacing per host.
- A silent server is asked again after `net_slist_timeout` (4 s), 3 times:
  it settles after about 12 s.
- No saved list, no seed; same masters (`xonotic-common.cfg`) and game name.

Its cold "half answered" beat ours (0.34 against 0.38 s in today's series)
because our list paced 1500 queries a second over ~540 addresses (300 dead,
interleaved with the live ones) and spaced the ports of one host 50 ms apart
(the busiest host runs 14 servers on IPv4 and 14 on IPv6: its last port went
out 0.7 s in), and its per-host table had 256 slots, so unrelated hosts
collided. Master choice, parallel masters and IPv4/IPv6 order were not the
difference: both ask the same six masters at once, and the nearest one
answers in 5 ms from here.

## Done and verified

- **Seed** (`engine/darkplaces/touch_srvlist_seed.h`, 243 addresses, written
  by `scripts/srvlist-seed.py`): with nothing saved, asked in the frame the
  list opens, first, before the masters answer; not shown until they answer.
  Not used when no master is set (tests, LAN-only) or once a saved list
  exists. `net_slist_seed 0` turns it off; a file name instead of `1` uses
  that list. The first wave goes to one address per network (/16, /48) before
  a second of any (no location is known).
- **CI refresh**: `.github/workflows/srvlist-seed.yml`, monthly + by hand;
  rewrites the header when more than 10 % of servers changed, pushes
  `bot/srvlist-seed-<date>` and opens a PR (the header is in the darkplaces
  component: merge with `component-push.sh darkplaces`). On a `ci/srvlist*`
  push touching the script it runs `--check` only. Keeps the last seed's IPv6
  servers when the runner has no IPv6.
- **First wave**: 48 in the frame the list opens (a long burst keeps the
  Wi-Fi radio from hearing answers: a 10 ms server asked first in a burst of
  220 answered after 42 ms, first of 49 after 15 ms), then 4000/s, at most
  320 a frame; per host 16 at once then 100/s (4096-slot table); EWOULDBLOCK
  stops the frame's sends instead of losing them; sockets ask for 2 MB / 1 MB
  buffers.
- **Insanotic model**: `net_slist_fast 2` runs Insanotic's scheduler in this
  engine for comparison; `tests/srvlist/bench_live.py` times sessions against
  the real masters (cold or warm, `--set` cvars, per-socket drop counts).

## Before / after (real masters, 2026-11-14 afternoon, medians of 5, cold rows interleaved)

| | rows in first frame | first row | t50 | t95 | settled |
|---|---|---|---|---|---|
| stock (`net_slist_fast 0`), first open | 0 | 0.49 (0.45-0.63) | 2.15 (1.92-2.36) | 3.47 (3.42-3.58) | 13.02 (12.76-13.19) |
| Insanotic model, first open | 0 | 0.07 (0.04-0.09) | 0.34 (0.34-0.35) | 0.48 (0.43-0.50) | 12.28 (12.28-12.29) |
| before (f617cab), first open | 0 | 0.04 (0.02-0.06) | 0.38 (0.36-0.38) | 0.84 (0.84-0.85) | 3.12 (2.93-3.13) |
| **after, first open** | 0 | **0.05 (0.04-0.05)** | **0.33 (0.33-0.33)** | **0.39 (0.38-0.39)** | **1.89 (1.89-1.90)** |
| before, later opens | 235 | 0.00 | 0.33 (0.33-0.34) | 0.79 (0.79-0.82) | 2.84 (2.75-3.11) |
| **after, later opens** | 237 | **0.00** | **0.33 (0.32-0.34)** | **0.38 (0.38-0.40)** | **2.55 (1.89-2.75)** |

After beats the Insanotic model on all four cold columns. Against the old
list the cold first row is 0.01 s later (overlapping ranges; see docs).
The morning numbers in the brief (0.12 / 0.47 / 0.95 / 3.36) came from a
slower network (masters ~0.1 s away); compare rows of one series only.

## Tests (all on `taskset -c 0-5`, basedir `.suite-base` = this clone's pk3dir)

- `scripts/test-srvlist.sh`: logic test + 6 end-to-end scenarios (cold, warm,
  changed, stock, seed with a 0.6 s master, Insanotic model): all 43 checks pass.
- `scripts/test-joinfail.sh` pass; `test-lan-logic.sh` pass; `test-lan.sh`
  pass; `test-lan-private.sh` PASS.
- `scripts/test-uibridge.sh` and `scripts/test-uibridge.sh --engine`: all passed.
- `cargo test --offline -p godnotic-loader-core`: ok, 0 failed.
- `scripts/test-old-name.sh`: PASS.
- Builds: SDL (`make sdl-release`), `make sv-release`, `scripts/build-web.sh
  --engine-only` (XT_WEB_DEPS from the integration clone): all rc=0, no
  warnings from touch_srvlist.
- CI on eec0bd9 (the last code commit), all success: `work/srvlist2` Tests
  43560427247, Android 43560427288; `ci/srvlist2` Tests 43560429680, Android
  43560429684, Server list seed 43560429816. The seed check ran on a GitHub
  runner: UDP to the masters and servers works there (3 IPv4 masters, 553
  listed, 169 IPv4 servers answered, no IPv6 on the runner, the seed's 75
  IPv6 servers kept).
- The other suites were run once against 2b34622 and again against the final
  engine (joinfail, lan-logic, lan, lan-private, uibridge, uibridge --engine).

## Not done / not verified

- Nothing measured on the tablet, on mobile data or against Android's socket
  buffer limits (`rmem_max`); only this desktop on Wi-Fi in Australia.
- Insanotic was not run (no build here): its row is a model of its scheduler
  in this engine (the docs say exactly what is and is not modelled).
- The seed workflow's write-and-PR path (monthly or by hand) has not run:
  GitHub only schedules or dispatches a workflow once it is on `main`, and
  `gh pr create` needs "Allow GitHub Actions to create pull requests" (if it
  is off the job still pushes the `bot/srvlist-seed-<date>` branch).
- The first row cannot be steered toward near servers on a first open: the
  seed carries no location.

## Known issues

- The e2e cold scenario's t95 (1.17 s) sits in the retry window because the
  fake network's fixed random seed loses 7 of the first 100 answers; the bound
  is 2.5 s.
