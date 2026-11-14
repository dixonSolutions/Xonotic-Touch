# work/uibfix: QuakeC bridge menu "ignores" scripted taps and keys

Base: `gh/work/integration` 44e0595 (release candidate 1: androidui2 + screens3 + controls to ee0e7b4).

## Cause

Not the real-time `UIB_Tick` (a5e4da2), not Home (c359a71), not the front-end bridge.
The guilty commit is **e7c8e9d** "Welcome: every new player picks a name" (androidui2).

It adds a `welcome` dialog that `UIB_Context` opens over Home whenever
`TouchStartup_nameNeeded()` is true (no `touch_profile_done`, `_cl_name` empty or
"player"). The dialog cannot be dismissed and refuses back, and as a modal it takes every
tap and key. `tests/uibridge/input_test.py` starts the engine on a fresh userdir with no
name and without `touch_profile_done 1`, so from its first step the welcome sat over Home:
taps on Home's cards and Down+Enter reached the dialog, not Home, and the stack stayed
`['home']`. e7c8e9d pinned `touch_profile_done 1` in `scripts/uib-fixtures.py`, but missed
input_test.py. The checks that still passed (Escape goes back, Escape closes it) passed only
because the stack never left Home.

Evidence: every snapshot in the failing run has `"modal":["welcome"]`. The same engine and
QC with `+set touch_profile_done 1` added passes all 20 original checks.
`git log -S TouchStartup_nameNeeded` lists only e7c8e9d.

The product behaviour is as intended (the owner wants every new player to pick a name), so
there is no engine or QuakeC change. Native front ends are untouched.

## Fix

`tests/uibridge/input_test.py` now runs the welcome first, through the same scripted input,
on the QuakeC renderer: the fresh install opens on the welcome with Continue off; Escape does
not close it; a tap on the name field and typing turn Continue on; Enter saves `_cl_name` and
`touch_profile_done 1` to config.cfg and closes it. The existing steps follow unchanged.
(`uib_input text` types a single word, which is why the name is `InputTester`.)

## A second failure the first one hid

`test-uibridge.sh --engine` runs under `set -e`, so the fixture drift check after
input_test never ran on the androidui2 merge. With input_test passing, it reported two
stale fixtures. Neither is a bug: in both cases a merge carried a fixture recorded before
another branch changed the screen.

- `welcome.json` (androidui2 e7c8e9d) has Home under the dialog with `minItem 240`. screens3
  (add0cdb) changed Home's grid to `minItem 200` and re-recorded home.json, but not the
  welcome fixture from the other branch.
- `servers.json` (re-recorded by screens3 add0cdb) has no `subtitle`. On integration the
  servers screen opens while the refresh runs and says "Looking for servers…"
  (`_slist_refreshing`, srvlist b6c1e12).

Both were re-recorded with `scripts/uib-fixtures.py --write`. The diff is those two lines
and nothing else.

## Tests

Environment: `~/.cache/godnotic-work/uibfix-env.sh`, i.e. run-suites.sh's env with a private
basedir `~/.cache/godnotic-work/uibfix-base`. That basedir has the stock pk3s plus this
clone's pk3dir. `~/.cache/godnotic-infra/base` links the integration clone's pk3dir, so it
would test someone else's QuakeC. Every run was pinned with `taskset -c 8-11`.

- Before the fix, on 44e0595: `input_test.py 1280x720 mouse` failed 14 checks, and every
  snapshot had `"modal":["welcome"]`. The same run with `+set touch_profile_done 1` passed
  all checks.
- After the fix:
  - `scripts/test-uibridge.sh --engine`, twice: `test-uibridge: all passed` both times, with
    0 FAIL, 0 DRIFT and 102 ok each. That covers engine_test, input_test with 37/37 steps,
    and fixtures --check.
  - `scripts/test-uibridge.sh`: all passed.
  - `scripts/test-frontend.sh`: "front-end bridge: all checks passed".
  - `scripts/test-touch-hudmode.sh`: rc=0.
  - `scripts/test-touch-spectate.sh`: "all checks passed".
  - `scripts/test-joinfail.sh`: "all checks passed".
  - `cargo test --offline -p godnotic-loader-core`: ok, 0 failed (rerun after the
    fixture update).
  - `cargo test --offline -p godnotic-loader-android --no-default-features`: ok, 0 failed.
- hudmode, spectate and joinfail ran before the fixture commit, which they do not read.

## Not done / known

- I did not build and run each androidui2 commit. The cause was pinned from the snapshots,
  the A/B with `touch_profile_done 1`, and `git log -S`.
- Nothing was checked on the tablet or in a browser. The welcome on the web build is covered
  by `tests/web-mods/browser.sh` (e7c8e9d), which I did not rerun.
