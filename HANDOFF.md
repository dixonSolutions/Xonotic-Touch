# Handoff: work/net2 (away policy after an automatic reconnect, connect back-off timing)

Branch `work/net2`, from origin/main `e7c8e9d`. Scratch clone `~/.cache/godnotic-work/net2`.
Nothing here is merged. Not pushed to main; no component-push.

## The bug (tablet report) and its cause

After an automatic reconnect following a **server** freeze (36 s), 11 minutes
away neither moved the player to the spectators nor left the server.

Cause: the client timed out and joined again from the same IP:port while the
server still held its slot (the server, frozen, had not timed it out). Stock
DarkPlaces' "client crashed and is coming back" branch (netconn.c) keeps the
slot **and its connection's sequence numbers**, while the client starts a new
connection at sequence 0. Every reliable message is then a "duplicate" in
both directions: the client looks connected (unreliable entity updates flow,
it keeps playing) but none of its reliable commands reach the server, so
`cmd spectate` was never received; and the client's clean disconnect was
never processed, so the server only timed it out later. A client-side freeze
(the earlier tests) did not show it, because there the server had already
timed the slot out and the rejoin got a fresh one.

## What changed (uncommitted before this handoff; now in the branch)

- `touch_net.c` `TouchNet_ServerRejoin` + one hook line in netconn.c's
  begun-client branch (`if (!TouchNet_ServerRejoin(...)) // Touch:` before the
  stock `accept`): the server starts that connection over (sequences, buffers,
  windowed state) and answers as for a new connection (with `rwin` if offered).
  Also fixes the same stock bug for stock clients that rejoin from one port.
- Client: when it schedules an automatic reconnect after a lost connection it
  first sends `clc_disconnect` three times (CL_DisconnectEx sends it only when
  the player leaves), so a server that still hears it frees the slot.
- Away state is re-armed whenever the client is (back) at signon 4 — a new
  connection or a new map: `spectated` is cleared so `cmd spectate` is sent
  again while away; the away time still counts from when the app left.
- The net_away_limit message reads "after 30 s" below 2 minutes (it said
  "after 0 minutes"), and is printed to the console too.
- `scripts/test-net-away.sh`: new cases `reconnect-away`, `svfreeze-away`,
  `away-freeze`, `mapchange-away`, `manual-away`; client and server poll a cfg
  file so the test can send commands (touch_away, changelevel, connect) at
  the right moment.
- `scripts/test-net-connect.sh` + `tests/net/drop_connects.py`: a fake server
  that ignores the first N connects; join time with `net_connect_backoff` 0/1.

## Verified (commands and results)

All in network namespaces, local only. Binaries copied to
`~/.cache/godnotic-work/netdata/bin2/` (pre-* = origin/main, fix-* = this branch);
basedir `~/.cache/godnotic-work/netdata/base2`, libs `~/.cache/godnotic-slimtest/lib`.

```
G=~/.cache/godnotic-work/netdata
ENGINE=$G/bin2/pre-sdl SERVER=$G/bin2/pre-dedicated BASEDIR=$G/base2 LIBS=~/.cache/godnotic-slimtest/lib \
  scripts/test-net-away.sh reconnect-away svfreeze-away
```
origin/main (reproduces the tablet bug):
```
ok   reconnect-away: rejoined by itself after a 40 s freeze
ok   reconnect-away: away after the reconnect: spectator after net_away_spectate
ok   reconnect-away: ... left at net_away_limit, the server freed the slot ...
ok   svfreeze-away: rejoined by itself after the server froze 36 s
FAIL svfreeze-away: away after the reconnect: not moved to the spectators
FAIL svfreeze-away: away after the reconnect: no clean disconnect at net_away_limit (Client "Bench" dropped (Timed out))
```
This branch (fix-*), same command:
```
FAIL svfreeze-away: did not get back into the game      <- test criterion, fixed since (see below)
ok   svfreeze-away: away after the reconnect: spectator after net_away_spectate
ok   svfreeze-away: away after the reconnect: left at net_away_limit, the server freed the slot, the client says why (Left 10.88.0.2:26000 after 30 s in the background)
ok   reconnect-away: rejoined by itself after a 40 s freeze
ok   reconnect-away: away after the reconnect: spectator after net_away_spectate
ok   reconnect-away: away after the reconnect: left at net_away_limit, ... (Left 10.88.0.2:26000 after 30 s in the background)
```
The one FAIL was the test's own criterion: a server keeps a returning player
in the game, so there is no new "is now playing" line (the server log shows
the bot hitting Bench right after the rejoin). The criterion now checks the
rejoin in the server log; not re-run since the change.

Connect back-off (`scripts/test-net-connect.sh 0 1 2 3 4`, fix-sdl):

| connects dropped | stock timing (backoff 0) | Godnotic (backoff 1) |
|---|---|---|
| 0 | 0.003 s | 0.001 s |
| 1 | 1.005 s | 1.007 s |
| 2 | 2.006 s | 2.006 s |
| 3 | 3.007 s | 8.562 s |
| 4 | 4.011 s | 9.567 s |

First retry and the 1–2 lost-connect case are as fast as stock. From the
third unanswered connect the back-off pauses 5.6 s (by design, against a
stock server's renewing 5 s connect flood block), so a host that drops
exactly 3 connects for other reasons is 5.5 s slower than stock. Decision
for the srvlist owner: keep, or back off only after 4.

Also passed: `scripts/test-old-name.sh` (PASS), `scripts/test-lan-logic.sh`.
Builds: client (`xonotic_compile_engine_only`) and `make sv-release` both
build on this branch.

## Not done

- `away-freeze`, `mapchange-away`, `manual-away` were written but not run
  (the run was stopped for this handoff), nor the re-run of `svfreeze-away`
  with the corrected criterion. Run:
  `ENGINE=$G/bin2/fix-sdl SERVER=$G/bin2/fix-dedicated BASEDIR=$G/base2 LIBS=~/.cache/godnotic-slimtest/lib scripts/test-net-away.sh`
  (all cases, about 25 minutes; needs a machine load average below ~30).
- `scripts/build-web.sh --engine-only` not run. It needs a cached opus build:
  `XT_WEB_DEPS=~/.cache/godnotic-work/srvlist/build/web-deps scripts/build-web.sh --engine-only`
  (otherwise it downloads opus sources, ~7 MB over the metered link).
- CI on work/net2: check `gh run list --repo dixonSolutions/Godnotic --branch work/net2`.
- The duplicate-accept branch (a client still in its signon that restarts
  its connection) has the same stock sequence problem; not changed.
- On-device check of the tablet case (server frozen, rejoin, then Home for
  11 minutes): expected now "is now spectating" after ~20 s and "Disconnect by
  user" at 10 minutes in the server log.

## Merge notes

- Upstream hook added: netconn.c, one line in the begun-client branch of the
  connect handler. Everything else is in touch_net.c/.h and scripts/tests.
- `docs/NETWORK.md` does not describe the rejoin reset yet (one paragraph
  under "Staying connected" would do).
