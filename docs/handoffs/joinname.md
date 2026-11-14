# Handoff: `work/joinname` (the join card names the server)

Owner's report: "the joined card for servers says joined with IP address, we need the name
and then in brackets the ip address, so user knows the name."

Base: `origin/work/uifix` (6d65836, which contains `origin/main` 7a8d3ee). uifix's
`UIB_Join` records the list's name in `_uib_server_name`; this branch keeps that as the
fallback and adds the engine side. Clone `~/.cache/godnotic-work/joinname`.

Both components changed: `engine/darkplaces` (a new Touch file plus hooks) and
`engine/data/xonotic-data.pk3dir` (menu QuakeC). The merge needs
`scripts/component-push.sh all` and the pins. `component-push.sh` was not run here.

## What changed

Every screen of a join names the server **`<name> (<address>)`**, or the address alone
while no name is known. Colour codes (`^1`, `^x0f0`) are stripped, the name is one line,
and a name longer than 32 characters is cut to 31 plus `…`.

- **`engine/darkplaces/touch_joinname.c/.h` (new, Touch-owned).** Keeps the name of the
  server being joined or played on:
  1. the server list's name (`serverlist_cache`, matched on the address), so a join
     from Multiplayer is named from the first frame;
  2. otherwise the server's own `hostname`: a `getinfo` with a challenge of our own to
     the join address (4 tries, 1.25 s apart), answered with an `infoResponse`. A typed
     address or a link gets its name as soon as the server answers, during
     "connecting", so a refused or busy join is named too.
  The answer is ours alone: `netconn.c` hands it to `TouchJoinName_Connectionless` and
  drops it, so a typed server never lands in the server list. The name stays until
  another server is joined (Could not join, a reconnect, the pause page). A local game
  or a demo clears it. Published as `_touch_join_address`, `_touch_join_name` and
  `_touch_join_label` (read-only cvars).
  Hooks: one line in `netconn.c` (`// Touch:`, next to the existing
  `TouchLAN_ClientConnectionless` hook) plus its include, one line in `makefile.inc`.
  Frame and Init come from `touch_joinfail.c`, so there is no other upstream hook.
- **Connect topic** (`touch_frontend.c`): new `name` and `label` keys
  (docs/FRONTEND_BRIDGE.md).
- **Could not join** (`touch_joinfail.c`): the sentences use the label ("No answer from
  …", "… answered but did not let you in", "… refused the join"). `_join_fail_address`
  and `join_retry` still carry the plain address. `cl_connect_status` keeps the plain
  address (it is 128 bytes and the front ends match its prefix).
- **Reconnect bar** (`touch_net.c`): "Reconnecting to <name> (<addr>)…", "(attempt n of
  m)", "Could not reconnect to …" and "Disconnected by …" use the label.
- **Android notification state** (`touch_android.c`): "Joining …" and "Connected to …".
- **Menu QuakeC** (`touch_uib_screens.qc/.qh`): `UIB_ServerLabel(addr)` and
  `UIB_JoinAddress()`. They drive the loading card's title ("Joining <label>", which the
  GTK and QuakeC renderers draw), the pause page's card for someone else's server (title
  `<label>`; the subtitle is now just "Online game" or "LAN game", since the address is
  in the title), the `mp.password` popup ("<label> needs a password."), and the
  map-download hold dialog ("Then you join <label>."). Before the join starts (a locked
  server's password prompt) the list name that `UIB_Join` recorded stands in. The
  classic password dialog (`dialog_touch_lan.qc`) uses the engine's name too.
- **Kotlin** (`FrontendHost.kt`): the loading strip's title is
  `Joining ${label}` from the connect topic. The last address and label are kept for the
  native password prompt and the Could not join dialog, whose body no longer repeats an
  address the sentence already names.

## Tests

- `scripts/test-native-menu-live.sh joinname` (new case, in the default list) and
  `tests/native-menu/check_joinname.py`. A server named
  `^1Red^7 ^x0f0Green^7 Test Server with a very long name indeed` sits behind
  `tests/native-menu/udp_delay.py`, a relay that holds its `challenge` back 4 s and its
  `infoResponse` 2 s. A client with a native front end attached joins it twice:
  - from the server list (the relay port is a favourite; no master server): the loading
    card reads `Joining Red Green Test Server with a ve… (127.0.0.1:P1)` from its first
    frame, the connect topic's label is the same at once, and so is the pause card;
  - by an address typed into Multiplayer: the card reads `Joining 127.0.0.1:P2`, then
    `Joining Red Green Test Server with a ve… (127.0.0.1:P2)` once the server answers.
    The label follows, and the pause card names it.
  - no `^` reaches any title, and the name is 32 characters ending in `…`.
- `joinmid`: the server is now named `^3Joinmid^7 test`, so its pause card must read
  `Joinmid test (127.0.0.1:P)`. That is the name of a typed connect, learned from the
  server.
- `scripts/test-joinfail.sh`: the no-back-off server is named `^2Busy^x0f0 test`. The busy
  report must read `Busy test (127.0.0.1:P) answered but did not let you in`.
- Fixtures: `tests/uibridge/fixtures/seq-download.json` (seeded, not engine-made) and
  `seed_fixtures.py` now title the card `Joining Clan Wars #12 (203.0.113.12:26000)`. Its
  `scripts/test-gtk-screens.sh` check follows. No engine-made fixture changed:
  `uib-fixtures.py --check` passes, because no fixture joins a server.

## Results

All on CPUs 10-11 (`taskset -c 10-11`), basedir `<clone>/.suite-base` (the stock pk3s
plus this clone's pk3dir), engine `build/bin/xonotic` built from this branch.

Builds: QuakeC + SDL `rc=0`, `make sv-release` `rc=0`, `scripts/build-web.sh --engine-only`
with `XT_WEB_DEPS=~/.cache/godnotic-infra/main/build/web-deps` `rc=0` (5.3M wasm). There are
no warnings from the new or changed files.

| suite | result |
| --- | --- |
| `scripts/test-old-name.sh` | PASS |
| `scripts/test-uibridge.sh` | all passed |
| `scripts/test-uibridge.sh --engine` | all passed (engine_test, input_test, firstrun, `uib-fixtures.py --check`) |
| `cargo test --offline -p godnotic-loader-core` | 23 passed, 0 failed |
| `cargo test --offline -p godnotic-loader-android --no-default-features` | 9 passed, 0 failed |
| `scripts/test-joinfail.sh` (end to end) | all checks passed |
| `scripts/test-native-menu-live.sh` (uid2name, joinmid, joinname; STOCKBASE=`~/.cache/godnotic-work/controls-stock`) | all checks passed |
| `scripts/test-frontend.sh` | all checks passed |
| `scripts/test-net.sh gn-0 gn-100` | all net tests passed |
| `scripts/test-lan-private.sh` | PASS |
| `scripts/test-gtk-screens.sh` | not run: the only built loader (`~/.cache/godnotic-work/cargo-target/release/godnotic-loader`, 14:29) predates the script's `labels` command, so every check there comes back empty. The one changed line is its title expectation. |

CI on `ci/joinname` (536c12e): Tests success, Android success (the Kotlin compiles), Loader
success. Loader's first attempt failed in `crates/core/tests/fetch.rs`
(`a_stop_resumes_without_fetching_done_bytes_again`, `done > 0`, aarch64). That is a timing
test in code this branch does not touch, and it passed on a re-run of the failed job.

Screenshot: `~/.cache/godnotic-work/joinname-logs/shot/out/1280x800-shot.png`. It is the GTK
renderer drawing a real engine snapshot taken from the joinname run (typed join, name
known): "Joining Red Green Test Server with a ve… (127.0.0.1:27221)", "Connecting", Cancel.

## Not done or not verified

- Nothing was checked on the tablet or an Android device. The Kotlin compiles only in CI
  (`ci/joinname`). The Kotlin card was not seen on screen, though its source (the
  connect topic's `label`) is checked by the live test.
- The name is asked for during the join only. A server that blocks direct queries
  (`sv_public -1`) and is not in the list stays named by its address.
- A name that keeps a literal `^` after decolouring (written `^^` on the server) shows as
  `^`, which the QuakeC renderer may read as a colour code. Kotlin and GTK show it as is.
