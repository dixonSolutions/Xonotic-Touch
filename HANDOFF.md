# Handoff: work/net2 (away policy after a reconnect, rejoin of a held slot, connect back-off)

Branch `work/net2`, scratch clone `~/.cache/godnotic-work/net2`, merged with
`gh/main` f617cab (which contains work/integration 664916d). Nothing here is
on main; no component-push.

## What the branch does

- **Server, rejoin of a held slot** (`TouchNet_ServerRejoin`, one `// Touch:`
  hook in netconn.c's begun-client branch). A client that timed out while the
  server was frozen joins again from the same IP:port; stock DarkPlaces keeps
  the slot *and its connection's sequence numbers* while the client starts at
  0, so every reliable message is a "duplicate" both ways (no `cmd spectate`,
  no clean disconnect gets through). The connection now starts over.
- **Server, restart during the signon** (`TouchNet_ServerDuplicate`, the
  duplicate-accept hook): a client that has already sent packets on its
  connection and connects again gets a fresh connection and signon, not a
  duplicate accept on the stale one.
- **Client**: sends `clc_disconnect` before an automatic reconnect;
  `connect` to the server it is already on leaves first
  (`TouchNet_ConnectAgain`, one hook line in cl_main.c).
- **Connect back-off** only after four ignored connects (as fast as stock up
  to three lost connects), and the pause counts from the last connect sent.
- **Away policy** (this round, commit b6dfaff):
  - the short silence limit after a freeze (3 s for 10 s) runs on the wall
    clock. A freeze usually lands inside a frame whose `host.realtime` was
    taken before it, so the window counted from realtime had already ended
    by the next frame, and the loss was judged by the 30 s limit. Away, the
    away limit came first, and its clean disconnect went to a server that had
    dropped the player long before;
  - while away the client asks for the spectators whenever the scoreboard
    says it plays (frags other than -666): at most every 5 s, at most three
    refused requests in a row, checked again at once after a rejoin or a map
    change. The old single request at signon 4 did nothing on a fresh slot
    (an observer) and was lost to anything that joined the player later.
    An observer by choice is left alone and is not joined on return.

## The three failures from the last round, and what they were

| case | failure | cause | fix |
|---|---|---|---|
| away-freeze | no clean disconnect (server: Timed out) | product: the after-freeze window was computed from the stale frame time (SIGSTOP inside a frame), so the client only noticed the loss after the 30 s limit; at net_away_limit 75 its disconnect went to a server that had dropped it | quick window on `Sys_DirtyTime()` (touch_net.c); traced with a debug build: `quick=29.06` while `now=59..88`, the loss now seen 3 s after the freeze |
| mapchange-away | not moved to the spectators | test: `$JOIN`'s `defer 20 "cmd join"` fired after the map change and put the player back (the "is now playing" in the log was the test's own join). Also the polled `changelevel` ran twice (two map changes 0.5 s apart). Product weakness found on the way: the one spectate request at signon 4 is sent while still an observer and does nothing | `playing()` calls off the remaining joins; `once` runs changelevel/connect exactly once; the client re-checks the scoreboard while away |
| manual-away | not moved; no clean disconnect (Disconnect by user ×2) | test: the same stray `defer 20` join; the clean-leave check matched the manual reconnect's *own* "Disconnect by user" and then looked for "Left" 30 s too early; `connect` could run twice | same test fixes; the clean-leave check waits for the client's "Left ..." and the drop after the last join |

Test criteria now: the spectator state comes from the server's `status`
(frags, with `sv_status_show_qcstatus 0`), not from log lines (a kept slot
prints no new "is now playing"); after each rejoin the test also joins the
player while away (as an auto-join or a kept slot would) and expects it moved
again. `away-freeze` uses net_away_limit 75, so it fails again if the loss
is noticed late. The download case started `range_http.py` through a shell
function, so `kill $HP` killed the subshell and left the server running
(pid 994088 from the last round; killed); it now starts it with `nsenter`
directly.

## Verified (2026-11-14, merged tree 1d18aad, built here)

Builds: `xonotic_compile_qc_only && xonotic_compile_engine_only` rc=0
(`v1.2.202-33-g1d18aad`), `make sv-release` rc=0,
`XT_WEB_DEPS=~/.cache/godnotic-infra/main/build/web-deps scripts/build-web.sh --engine-only` rc=0.

Suites (BASEDIR `~/.cache/godnotic-work/netdata/base2` = stock pk3s + this
clone's pk3dir, LIBS `~/.cache/godnotic-slimtest/lib`, ENGINE build/bin/xonotic,
SERVER engine/darkplaces/darkplaces-dedicated, `taskset -c 4-7` / `8-11`;
logs in `~/.cache/godnotic-work/netdata/logs5/`):

`scripts/test-net-away.sh` (all cases, one run): **all away tests passed**
```
ok   freeze5: stopped 5 s (match), still connected, no reconnect
ok   freeze-signon: stopped 5 s (signon), still connected, no reconnect
ok   freeze-signon: the join completed
ok   away: spectator after 5 s in the background
ok   away: back into the game on return
ok   newaddress: the client saw its address change
ok   newaddress: joined again from 10.88.0.9 by itself
ok   timeout: the server dropped the frozen client after net_messagetimeout
ok   timeout: back, the client noticed (Lost connection to server (nothing from it for 3 s)) and joined again by itself
ok   download: stopped 10 s midway, completed, identical
ok   freeze60: stopped 60 s, the server timed it out, it joined again by itself
ok   loss-freeze60: stopped 60 s, the server timed it out, it joined again by itself
ok   reconnect-away: rejoined by itself after a 40 s freeze
ok   reconnect-away: away after the reconnect: not a player after net_away_spectate (moved to the spectators)
ok   reconnect-away: away after the reconnect: put back in the game while away, moved to the spectators again
ok   reconnect-away: away after the reconnect: left at net_away_limit, the server freed the slot, the client says why (Left 10.88.0.2:26000 after 30 s in the background)
ok   svfreeze-away: rejoined by itself after the server froze 36 s
ok   svfreeze-away: away after the reconnect: not a player after net_away_spectate (moved to the spectators)
ok   svfreeze-away: away after the reconnect: put back in the game while away, moved to the spectators again
ok   svfreeze-away: away after the reconnect: left at net_away_limit, the server freed the slot, the client says why (Left 10.88.0.2:26000 after 30 s in the background)
ok   away-freeze: spectator
ok   away-freeze: reconnected while away: not a player after net_away_spectate (an observer since the rejoin)
ok   away-freeze: reconnected while away: put back in the game while away, moved to the spectators again
ok   away-freeze: reconnected while away: left at net_away_limit, the server freed the slot, the client says why (Left 10.88.0.2:26000 after 75 s in the background)
ok   mapchange-away: spectator
ok   mapchange-away: after a map change: not a player after net_away_spectate (an observer since the rejoin)
ok   mapchange-away: after a map change: put back in the game while away, moved to the spectators again
ok   mapchange-away: after a map change: left at net_away_limit, the server freed the slot, the client says why (Left 10.88.0.2:26000 after 45 s in the background)
ok   manual-away: after a manual reconnect: not a player after net_away_spectate (an observer since the rejoin)
ok   manual-away: after a manual reconnect: put back in the game while away, moved to the spectators again
ok   manual-away: after a manual reconnect: left at net_away_limit, the server freed the slot, the client says why (Left 10.88.0.2:26000 after 40 s in the background)
ok   connect-again: back in the game 0.6 s after the connect
ok   connect-again: no timeout
ok   restart-signon: the server saw the restart and started the connection over
ok   restart-signon: the join completed after the restart
all away tests passed
```

Other suites, one run each:
```
old-name rc=0 PASS: no old project name in tracked files
lan-logic rc=0 LAN logic: all checks passed
joinfail rc=0 joinfail: all checks passed
net-connect rc=0 connect timing: ok
net rc=0 all net tests passed
lan rc=0 LAN test passed
lan-private rc=0 PASS
srvlist rc=0 srvlist end to end: all checks passed
```

Connect timing (`scripts/test-net-connect.sh`):

| connects dropped | stock timing (net_connect_backoff 0) | Godnotic (net_connect_backoff 1) |
|---|---|---|
| 0 | 0.001 s | 0.001 s |
| 1 | 1.007 s | 1.005 s |
| 2 | 2.005 s | 2.010 s |
| 3 | 3.004 s | 3.009 s |
| 4 | 4.010 s | 8.620 s |
| flood | none s | 8.621 s |

CI: `ci/net2` at 1d18aad: Android (3 APKs), Tests, Loader all success.

## Not done / known

- Nothing checked on the tablet (owned by the Android agent). Expected on the
  tablet case (server frozen, rejoin, Home 11 min): "is now spectating" after
  ~20 s, "Disconnect by user" at 10 min in the server log.
- A player moved to the spectators who then loses the connection comes back
  (automatic reconnect, fresh slot) as an observer and is not joined on
  return (`TouchNet_Disconnecting` clears `spectated`, as on main). Keeping
  it across an automatic reconnect would put them back in the game; not
  changed, not tested.
- Seen once in a debug run, not investigated: two `changelevel`s 0.5 s apart
  gave the client `Host_Error: CL_ParseTempEntity: bad type 100`. Only with
  the test's old double changelevel; likely stock behaviour for a map change
  during the previous map change's signon.
- Other clones (integ, integm, integ2, android3) have orphaned
  `range_http.py` processes from the same test bug; they belong to other
  agents and were left running.

## Merge notes

- Upstream hooks: netconn.c (two lines: rejoin accept, duplicate accept),
  cl_main.c (one line). Everything else is touch_net.c/.h,
  touch_joinfail*, scripts and tests.
- HANDOFF.md conflicted with main's (work/hometabs2's) on the merge; this
  branch keeps its own.
