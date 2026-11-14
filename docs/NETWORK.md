# Joining over a real network

What a join costs over Wi-Fi, a hotspot or the internet, what Godnotic changes
so that it costs about as many round trips as the protocol really needs, and
how that is measured and tested. Engine code: `engine/darkplaces/touch_net.c`.

## Where a join spent its time

A join is a short conversation between client and server. Each step waits for
the peer, so on a real network the round trip time (RTT) multiplies:

| phase | client → server | server → client |
|---|---|---|
| connect | `getchallenge`, then `connect` with the challenge | `challenge`, then `accept` |
| serverinfo | | serverinfo, precache lists (models, sounds), stufftexts, `signonnum 1`: about 22 KB |
| precache | player info (`name`, colours, ...), after loading every precached model and sound: `prespawn` | |
| signon 2 | | baselines and static entities, `signonnum 2` |
| spawn | `spawn` | client data, light styles, stats, `signonnum 3` |
| begin | `begin` | the first entity frame (signon 4: the game is on screen) |

All of it except the first two packets travels on DarkPlaces' **reliable
channel**, which is stop-and-wait: a message is cut into fragments of 1 KB and
the next fragment (and the next message) goes out only when the last one is
acknowledged; a lost fragment is sent again after a fixed second. 22 KB of
serverinfo is therefore 22 round trips, and every 2% of loss adds a second
here and there. On top of that each side answers a request only at its next
frame: the server at its next tick (`sys_ticrate` 1/64 s, after sleeping
blind, with the packet waiting in the socket), the client at `cl_netfps` and
behind its frame limiter.

Measured on one desktop, stock behaviour (both sides with
`net_reliable_window 0` and `net_signon_fast 0`), stormkeep, a dedicated
server and a client in a network namespace with `tc netem` (Results below):
at 100 ms RTT the serverinfo phase alone took 2.3 s, 22.7 round trips of
waiting; the whole join waited 27 round trips. At 0 ms RTT the same phases
are a few milliseconds each: what is left there is local work (loading
models and textures, CSQC init, the first frame), which no network change
can shorten.

## What changed

### 1. A windowed reliable channel between Godnotic peers

Up to `net_reliable_window` fragments (default 32, i.e. 32 KB) are in
flight at once, across message boundaries. The bytes inside each fragment,
the fragment header (length and flags, sequence number) and the message
boundaries (`NETFLAG_EOM`) are exactly the stock ones; only the timing and
the meaning of an acknowledgement change:

- **Acknowledgements are cumulative.** An ACK for sequence *n* says "I have
  everything up to and including *n*". A receiver acknowledges every data
  packet it gets with the last in-order sequence it holds, also for a
  duplicate or a fragment that arrived ahead of a gap. (A stop-and-wait
  sender never gets ahead, so for it a cumulative ACK is the stock ACK.)
- **The receiver keeps fragments that arrive ahead of a gap**, up to the
  window, and hands complete messages to the game in order and exactly once.
  When a late fragment fills a gap and so completes several messages at
  once, they are parsed one after the other in the same packet's processing
  (`TouchNet_ClientAfterParse`, `TouchNet_ServerAfterRead`), so the game
  sees the same message sequence as on a stock connection.
- **Retransmission.** The timeout comes from the measured round trip:
  smoothed RTT and its variance as in RFC 6298 (no sample from a fragment
  that was sent twice: Karn's rule), `max(2 x SRTT, SRTT + 4 x RTTVAR)` plus
  one server tick, doubled on every consecutive timeout, bounded by
  `net_reliable_rto_min` (0.2 s: a peer answers in its next frame, which on
  a busy listen server or a phone loading a model can take that long) and
  `net_reliable_rto_max` (1 s, the stock
  fixed value). The client seeds the estimate with the connect/accept
  exchange. On a timeout everything in flight is sent again (go-back-N); on
  three duplicate ACKs only the first missing fragment is (fast retransmit),
  and a partial ACK during recovery sends the next hole.

  Why not pure selective repeat: the wire has only cumulative ACKs (one
  32-bit sequence, as stock; no SACK blocks), so the sender cannot know which
  later fragments arrived. Go-back-N on a timeout recovers any loss pattern
  in one round trip, and the receiver's reorder buffer makes the next
  cumulative ACK jump over everything that did arrive, so only one round of
  duplicates is ever sent. Fast retransmit of a single fragment covers the
  common case of one lost packet with no duplicates at all. A join is 25 KB
  of reliable data; at 10% loss the duplicates cost a few KB.
- **Flow control.** The window is what the receiver said it can buffer, and
  never more than the peer's `rate` (bytes per second) can carry in two
  round trips (minimum 4 fragments), so a client on a slow link that asked
  for a low rate is not flooded. Fragments sent when an ACK arrives count
  against the connection's `cleartime` like any other packet, so the stock
  rate limiter (`NetConn_CanSend`, `sv_maxrate`, `rate`, `rate_burstsize`)
  still paces the unreliable traffic that follows.
- **Bounded memory, stock overflow.** Each windowed connection holds a
  send queue of 160 fragments (160 KB, allocated with the connection), a
  reorder buffer of 64 fragments and a buffer for messages a filled gap
  completed (130 KB, allocated on first use). A new reliable message is taken
  into the queue only while less than a window is queued; otherwise it waits
  in the connection's 64 KB message buffer, where messages pile up compactly
  exactly as behind a stock sender's one message in flight, and a peer that
  stops acknowledging overflows that buffer and is dropped exactly as by a
  stock server. Acknowledgements of sequences never sent, fragments outside
  the window and fragments longer than 1 KB are ignored (but acknowledged
  with the cumulative sequence).
- **Demos.** `sv_send.c` writes a client's pending reliable message into the
  server demo only when the stock sender is idle (`sendMessageLength == 0`),
  i.e. in the frame the message is taken. The windowed sender keeps
  `sendMessageLength` 0 exactly when it will take the next message (and sets
  a marker of 1 while it holds them, keeping the stock one-second resend from
  firing on it), so each message is written to the demo exactly once, in
  order. Client demos record parsed messages, which are the same.
- **Downloads.** The DarkPlaces download protocol (`svc_downloaddata`,
  csprogs.dat when the client lacks the server's CSQC) runs on unreliable
  packets and is untouched; curl downloads (`sv_curl_defaulturl`) are HTTP.
  Their start commands travel on the reliable channel and arrive sooner.

#### Negotiation and wire format

Connectionless packets (`\377\377\377\377` + text), as DarkPlaces' own:

```
client -> server   connect\protocol\darkplaces 3\protocols\...\challenge\<c>...\rwin\<N>
server -> client   accept\rwin\<M>        (M = min(N, server's net_reliable_window), 2..64)
                   accept                  (stock server, or window off on the server)
```

`N` is the window the client can buffer (its `net_reliable_window`, at most
64). A server that does not know `rwin` ignores the unknown key, as
DarkPlaces ignores every key it does not read, and answers the stock
`accept`; the client then uses the stock channel. A server only answers
`accept\rwin\M` to a client that offered a window, so a stock client never
sees one. Both peers then send with window `M`; each side only buffers what
it offered. Anything else (`rwin` missing, not a number, below 2, a second
connect that no longer offers it) means the stock channel; a value above 64
is taken as 64.

The client turns `accept\rwin\M` into a plain `accept` before the rest of
the stack sees it, so the crypto layer's checks of a bare `accept` (host key
present, encryption level) run exactly as before.

**Insanotic**: the key, its meaning (window the client can buffer, server
answers the minimum, cumulative ACKs, fragments as stock) and the accept
form are Insanotic's convention, so a Godnotic client and an Insanotic
server (or the other way round) use the window with each other. Insanotic's
receiver drops fragments that arrive ahead of a gap instead of keeping them;
that only costs a Godnotic sender a few more resends to it. Insanotic offers
32 and its server caps at its `net_reliable_window` (16 by default).

### 2. A signon that does not wait for frames (`net_signon_fast`, default 1)

Timing only; the bytes on the wire are the stock ones, so this also speeds up
a stock client joining a Godnotic server:

- **Server**: while any client is in its signon, the server sleeps in
  `select()` on its sockets instead of a blind sleep until the next tick, so
  a client's packet is read when it arrives; and the answer to a signon
  request (the serverinfo, the signon data after `prespawn`, the client data
  after `spawn`) goes out as soon as the request is parsed, not at the end of
  the next tick. Once every client is in the game, the stock timing returns.
- **Client**: during its signon the frame limiter is off (the existing
  `touch_load_restless` path) and the client sleeps in `select()` for at
  most 5 ms between frames, waking up for the server's packets: no busy
  loop. A reliable message written during the signon (player info,
  `spawn`, `begin`, `sentcvar` answers) is sent at once instead of at the
  next `cl_netfps` slot. Once the game is on screen the stock timing returns.

### 3. CSQC

The client never asks for the server's csprogs.dat when it already has a file
with the same CRC and size: `cl_csqc_download 2` (touch/xonotic.cfg) keeps the
bundled CSQC on a server running the same data build, and a csprogs.dat
downloaded before is found in `dlcache/` by name, size and CRC. The traces
below show no download on any join to a Godnotic server.

### 4. HTTP downloads (a server's map packs, `sv_curl_defaulturl`)

- **Parallel ranges for one large pack** (`touch_curl.h`, included by
  `libcurl.c`). Half a second after the server answered a download, if it
  would still take more than 3 s, what is left is split into up to
  `curl_segments` (4) pieces of at least `curl_segment_size` (2 MB): the
  stock transfer keeps the first, extra connections fetch the others with
  `Range:` requests into `<file>.partN`. The stock transfer is cut short at
  its piece's end only once every extra connection has answered `206 Partial
  Content`; a server without ranges, an error or a redirect elsewhere leaves
  the stock download alone. When every piece is in they are appended in
  order and the stock code checks and mounts the pack (`FS_AddPack`). A
  failed piece is asked for once more from where it stopped; if that fails
  too, the download fails as a stock one would. Downloads with a speed limit
  (`curl_maxspeed`, `sv_curl_maxspeed`) are never split, so limits hold.
- **Resume**: DarkPlaces resumes an interrupted download from the partial
  file in `dlcache/` (`CURLOPT_RESUME_FROM`); a split download that is
  cancelled drops its pieces, so the next attempt resumes the first piece.
- `curl_maxdownloads 8` (touch/xonotic.cfg; the engine default is 3): a
  server with several small packs costs a round trip per pack, not
  bandwidth.
- The loading screen's download line shows the size, the speed and the time
  left, over all pieces:
  `Downloading foo.pk3 ...  3.2 MB of 12.0 MB (27%) @ 850.0 KiB/s, 0:10 left`.
- **Verification**: the pack is checked as a pack (`FS_AddPack` must open
  it, else it is deleted and fetched again: stock), and the pieces must add
  up to the size the server announced. DarkPlaces' `curl` requirement lines
  carry no size or CRC, so there is nothing more to compare with yet (see
  What is left).
- **When the download starts**: the server's `curl --pak --forthismap ...`
  lines are part of the first reliable message after `accept` (with the
  serverinfo), and the client runs them at once, so the download already
  starts about 1.5 round trips after the connect, before any precache is
  loaded. Starting earlier needs the server list to carry the pack's name
  and URL (What is left).

### 5. Staying connected: leaving the game and coming back

What goes wrong when a phone puts the game aside, and what the engine does:

| situation | what happened before | now |
|---|---|---|
| Android freezes the process (no foreground service, cached-apps freezer) for a short while | the connection survives up to the server's `net_messagetimeout` (DarkPlaces default 300 s; many servers set 30-120) | same; on return the client listens closely for 10 s (3 s silence limit) and joins again by itself if the server dropped it |
| frozen longer than the server's timeout | dropped; the player came back to a dead game that only noticed after the client's own 300 s timeout, then a menu | the client sees the frames stopped (more than 5 s between frames) and the server silent, and **joins again automatically** (same address, the LAN password typed this session from memory) with `_net_reconnect_state` saying "Reconnecting to ... (attempt n of 5)" for the UI, `net_reconnect_cancel` to stop |
| the device's address changes (Wi-Fi to mobile data, a new DHCP lease) | the server keys the connection on IP:port and ignores the new address: a dead connection until a timeout | the client checks its own address toward the server every 2 s (a connected UDP socket's local address); when it changes it **joins again at once** with the fast join (about 4 round trips) |
| a server that stops answering | client timeout after 300 s | after `net_reconnect_silence` (30 s) the connection counts as lost and is joined again |
| kicked, server full, shut down | "Server disconnected" | not retried; the reason stays in `_net_reconnect_state` ("Disconnected by ADDRESS: reason") for the UI to show |
| in the background in a match (Android's onStop, `TouchNet_Away`) | the player stood idle in the match; Xonotic moves idle players to the spectators after 60 s (`sv_maxidle_playertospectator`) and some servers kick idlers (`sv_maxidle`) | after `net_away_spectate` (20 s) the client sends `cmd spectate`, and `cmd join` on return; after `net_away_limit` (10 min) it disconnects cleanly with the note "Left ADDRESS after 10 minutes in the background" |
| an HTTP download while away | curl is pumped by the frame loop: a frozen process stalls it | when the loop runs (foreground service) it continues; after a freeze it carries on from where it stopped (tested: stopped 10 s midway, completed, identical) |

The engine side cannot keep a frozen process alive: on Android the
foreground service has to be running (see "Android requirements" below).
`touch_away 0|1` drives the away policy on desktop for tests;
`touch_android.c` calls it from SDL's background/foreground events, and its
`touch_netstate` reports "joining / Reconnecting to ..." during a
reconnect so the foreground service stays up through it.

#### Android side (XonoticActivity.watchNetState, KeepAliveService, LanLocks)

`watchNetState` reads `touch_netstate` every 2 s (line 1 the kind, line 2 the
text, line 3 a download's size/speed/time left):

1. **The foreground service outlives a short idle.** While the activity is
   not in front the service stays 60 s after the last busy state (joining,
   connected, downloading, hosting), so a map change, a reconnect or a
   finished download that reads `idle` for a poll does not lose it (it could
   not be started again from the background on Android 12+). In front, an
   idle state stops it at once.
2. **Wi-Fi lock while connected in the background**: from the first busy
   poll while away until idle or back in front, a
   `WIFI_MODE_FULL_LOW_LATENCY` lock (API 29+, else `FULL_HIGH_PERF`),
   "Godnotic:connected"; hosting keeps its own multicast and Wi-Fi locks.
3. **Service type** `specialUse` for a connection (Android 14+), `dataSync`
   only for the data download; the partial wake lock lives with the service.
4. **Doze**: not handled beyond the foreground service; the 10-minute away
   limit bounds it.
5. **Notification**: the state line, and under it (expanded and as the
   subtext) the download line from `TouchNet_CurlText`. A join that finishes
   (`joining` to `connected`) while the activity is not in front posts
   "Ready to play" (its own channel, tap returns to the game), cleared when
   the player comes back.
6. **Reconnect UI**: the front-end bridge's `app` topic carries `reconnect`
   (what the automatic reconnect is doing, "Reconnecting to ... (attempt n
   of 5)") and `reconnect_note` (why it gave up). The native layer shows a
   bar over the game or the loading screen with the text and Cancel
   (`fe reconnect_cancel` = `net_reconnect_cancel`; the loading screen's
   Cancel also stops it), and a toast for a "Disconnected by ..." note.
7. A network change needs nothing: the engine notices it.

Tablet run (Galaxy Tab Active5, Android 15, on the same Wi-Fi as a desktop
dedicated server, 2026-11-14): joined by a `godnotic://join` link; Home for
30 s put the player with the spectators and back in the game on return; the
service ran as `specialUse` with the notification "Connected to ... - map" and
the "Godnotic:connected" low-latency lock while away (released in front); the
server frozen for 36 s gave "Connection timed out", then the reconnect bar
("attempt 1 of 5", "attempt 2 of 5") and the join again once the server
answered; hosting a LAN game on the tablet and joining it from the desktop
worked (the LAN popup closed itself when the desktop joined). Not run: screen
off (the tablet's lock screen), switching networks (not allowed on that
tablet), a pack download under a speed cap. Found: after an automatic
reconnect, 11 minutes in the background neither moved the player to the
spectators nor left the server (the server shows them playing throughout);
a fresh join does move to the spectators after 20 s.

**Joining again into a slot the server still holds.** That bug was the
server's, and stock DarkPlaces has it. The server froze, so it had not timed
the player out when the client gave up and joined again from the same
IP:port. Stock keeps the slot ("client crashed and is coming back") *and its
connection's sequence numbers*, while the client starts a new connection at
sequence 0. From then on every reliable message is taken for a duplicate in
both directions. Entity updates (unreliable) still flow, so the game looks
fine, but no command of the client's reaches the server: `cmd spectate`
while away, the clean disconnect at the away limit. Now:

- the server starts that connection over (sequences, buffers, window) and
  answers as for a new one (`TouchNet_ServerRejoin`, one hook line in
  netconn.c). This also helps stock clients;
- a client still in its signon that connects again gets the same treatment.
  The server can tell it from a lost accept, which also brings a repeated
  connect, because the client has already sent packets on that connection
  (`TouchNet_ServerDuplicate`);
- a client that reconnects by itself first sends its disconnect on the lost
  connection, so a server that still hears it frees the slot;
- `connect` to the server the client is already on, in the game or while
  joining, leaves it first (`TouchNet_ConnectAgain`, one hook line in
  cl_main.c). Stock keeps the old connection until the new accept and only
  then sends its disconnect. The server sees the connect, then the
  disconnect, and drops the player who just joined again; the new
  connection then hangs until a timeout, about 30 s. A stock client still
  does this against any server;
- the away timers are re-armed whenever the client is back at signon 4 (a
  new connection or a new map). The player goes to the spectators again
  while away, and the away time still counts from when the app left.

`scripts/test-net-away.sh` covers these with `svfreeze-away`,
`reconnect-away`, `away-freeze`, `mapchange-away`, `manual-away`,
`connect-again` and `restart-signon`. On origin/main, `svfreeze-away`,
`connect-again` and `restart-signon` fail.

Battery: a backgrounded connected client runs its loop at `cl_maxidlefps`
(20 Hz) without rendering (`vid_hidden`), reading packets and sending a
move per frame. Not measured on a phone; the estimate is that the Wi-Fi
radio staying awake for 20 small packets a second each way, not the CPU, is
the cost, of the order of an idle VoIP call (a few percent of a battery an
hour), which the 10-minute away limit bounds.

#### On-device checklist (tablet joins a desktop-hosted server over the hotspot)

1. Desktop: `darkplaces-dedicated -dedicated +set sv_public 0 +set net_messagetimeout 60 +map stormkeep`; tablet on the desktop's hotspot, join by address.
2. In the match, press Home for 10 s, come back: same match, no reconnect, the notification "Connected to ..." was shown while away.
3. Home for 30 s: the server log says "is now spectating" after about 20 s; on return "is now playing".
4. Screen off for 2 minutes: as 3; no "Timed out" in the server log.
5. Home for 11 minutes: the server log shows "Disconnect by user"; on return the game says "Left ... after 10 minutes in the background".
6. In the match, switch the tablet from the hotspot to mobile data and back (or toggle Wi-Fi): within a few seconds "Reconnecting to ..." and the match again, no password prompt (a password-protected LAN game).
7. Join a server that downloads a pack (the desktop with `sv_curl_defaulturl` and a 30 MB pack under a 2 MB/s cap), press Home during the download: the notification keeps showing progress; come back after it finished: in the game.
8. Kill the server while the tablet is in the background: on return "Disconnected by ...: Server disconnected" (no endless retries).

## The server list

Opening Multiplayer used to mean waiting. The stock list asks the masters,
waits until every one has sent its end-of-list (or `net_slist_timeout`, 4 s,
when one is silent), then asks 128 servers a second and at most two a frame,
and asks every server three times whether it answered or not; a row appears
when its server answers. `net_slist_fast 1` (the default; `touch_srvlist.c`)
keeps the protocol packet for packet (`getserversExt` to the masters,
`getstatus` with the timestamp challenge to the servers) and changes when
things are sent and what is shown meanwhile:

- **The saved list.** Every server that answered is kept with what its row
  shows (`touch_serverlist_info.txt` in the user directory: address, name,
  map, mode, players, ping, when it last answered, and how many sessions in a
  row it stayed silent). Opening the list puts those rows up in the same
  frame, before any packet is answered, marked stale until their server
  answers (`stale` in the bridge's row; the QuakeC list simply shows them). A
  saved server that stays silent is taken out when its tries are used up and
  is not shown from the saved list again until it has answered once; after
  three such sessions, or a week unseen, it is forgotten.
- **No waiting for masters.** A server is asked in the frame a master names
  it. The saved servers and the favourites are asked at once, before the
  masters have answered.
- **A budget on the clock, not the frame.** `net_slist_fast_rate` queries a
  second (1500), `net_slist_fast_burst` at most in one frame (48): about 550
  servers are asked within 0.4 s at any frame rate.
- **Asking again only where it helps.** A silent server is asked again 1.1 s
  later (a DarkPlaces server ignores a second `getstatus` from one address
  for 1 s), up to `net_slist_maxtries` (3) times if it was up last time,
  twice if a master lists it and nothing else is known, once if it already
  failed. After the last ask it has `net_slist_maxping` (420 ms) plus 0.1 s.
- **The menu's periodic refresh** (the QuakeC list asks again every 10 s
  while it is open) runs at 120 queries a second, the stock pace, and leaves
  a server that answered in the last 20 s alone. Opening the list, a reset
  and the first refresh in a minute run at full rate.
- **Rows that stay put.** While a finger is on the menu the list does not
  reorder (`net_slist_hold 1`): the QuakeC list is paused the way its own
  Pause box does it and let go a quarter second after the finger lifts; the
  bridge's `servers` sources keep their rows and rev until then. A native
  renderer says so itself: `"touching": true|false` in its `visible` event
  (docs/UI_BRIDGE.md). Between those moments a row is inserted where its
  sort key puts it and an answer updates the row in place.
- **"N servers · M players".** `_slist_summary` (and `_slist_servers`,
  `_slist_players`, `_slist_refreshing`, `_slist_saved`) for the Multiplayer
  card: from the saved list at the first frame of the session, live from then
  on.

### Being a good citizen

| what | number |
|---|---|
| to a master | what the stock client sends: one `getserversExt` per refresh and socket; the menu's 10 s timer is upstream's |
| to one server | one `getstatus` (about 45 bytes; the answer is 300-1400) per refresh; a second only after 1.1 s without an answer; at most 3 per refresh, 2 for an unknown address, 1 for one that failed before (stock: always 3) |
| to one IP address | at least 50 ms between two queries, whatever the ports (hosts that run several servers) |
| overall | 1500 queries a second at most, 48 in one frame; 120 a second for the periodic refresh; a server that answered is not asked again within 20 s by the periodic refresh |
| while joining | nothing: the list stops asking from the first `getchallenge` to the last signon, so nothing counts against the join at a rate-limiting host |
| after a reset | the time each address was last asked survives a list reset: two refreshes in a second do not ask anyone twice within the server's 1 s |
| amplification | none: answers go to the address that asked, and the client answers nothing |

A full refresh of today's list (about 560 addresses, 240 alive) sends about
890 packets cold and 560-620 with the saved list; the stock client sends
three per address, about 1700.

### Measured

`net_slist_stats` prints the last refresh, fast or stock; `net_slist_open`
does what opening the list does. Times are seconds from the refresh:
`first_row` the first row on screen, `t50` / `t95` when half / 95 % of the
servers that answered had answered, `settled` when every server had answered
or been given up.

Real masters and servers, 2026-11-14, a desktop in Australia on home
broadband (median server ping about 270 ms), offscreen client pinned to 8
cores while the machine ran other work (load 15-20), medians of 5 sessions
each (range in brackets), one refresh per session (`net_slist_open` 8 s after
the start; the first refresh that settles is taken):

| | sessions | rows in the first frame | first row | t50 | t95 | settled | servers listed | queries sent |
|---|---|---|---|---|---|---|---|---|
| stock (`net_slist_fast 0`) | 5 | 0 | 0.80 in the one session where no LAN game answered first (0.00 otherwise: a test server on this machine answers the LAN broadcast) | 1.91 (1.62-2.71) | 4.80 (2.91-6.39) | 8.27 (7.53-9.27) | 236 | about 1700 (3 per address) |
| fast, nothing saved | 5 | 0 | 0.12 (0.05-0.14) | 0.47 (0.44-0.51) | 0.95 (0.91-1.60) | 3.36 (3.26-3.52) | 238 | 889 |
| fast, the next session | 5 | 238 | 0.00 | 0.50 (0.43-1.51) | 0.99 (0.91-1.73) | 3.08 (2.12-3.18) | 238 | 582 |

Half of the servers are in after half a second instead of two, 95 % after one
second instead of five, and from the second session on the whole list is on
screen in the first frame. A higher rate was tried and is not the default:
at 2500 a second and 96 a frame, five sessions listed fewer servers (216 and
172 against 238) and `t50` went up to 1.5 s in the saved-list case, because
answers were lost (240 answers of up to 1.4 kB inside a tenth of a second)
and had to be asked for again a second later.

The floor for `t50` from here is the median ping, about 0.27 s, plus the
master's answer when nothing is saved (0.12 s). A stock refresh in this
series did not hit the 4 s master wait (every master answered); when one is
silent, stock adds up to 4 s before the first server is asked.

**Insanotic** (`~/Projects/insanotic`, `darkplaces/netconn.c`; compared by
reading it: there is no runnable build of it on this machine) removed the
wait after the masters (`net_slist_querywait 0`) and asks up to
`net_slist_queriesperframe` (100) servers every frame (`net_slist_burst 1`),
so on a first open it sends everything within a few frames (faster than
this list's 0.4 s by design, at the burst size that lost answers here), and
is frame-rate bound. It has no saved list (its first row is a master round trip
plus the nearest server's ping; here it is the first frame), waits
`net_slist_timeout` (4 s) before asking a silent server again and asks three
times, so it settles after about 12 s where this settles in about 3; it asks
every server at the full burst on every refresh, has no spacing per host and
keeps asking during a join.

### In the tests

`scripts/test-srvlist.sh`: the rules (`tests/srvlist/srvlist_logic_test.c`)
and the engine against a fake master and 160 fake servers on loopback with
latency and 3 % loss (`tests/srvlist/fakenet.py`, `test_srvlist.py`). It
asserts the time to the first row (0.6 s), to half (1.0 s) and 95 % (2.5 s)
of the answers and to settled (6 s) on a fresh install; every live server
listed; the saved rows up in the frame the list opens in the next session,
before any answer, stale in the bridge's source and counted in the summary;
20 servers gone and 10 new: removed, listed, and not shown from the saved
list again; no server asked twice within a second over three refreshes in a
row, hosts with several ports spaced; the stock scheduler still works with
`net_slist_fast 0`.

## When a join fails

Reproducing a reported "connection failed" against real public servers
(12 servers, IPv4 and IPv6, game versions 802 to 806, stock 0.8.5 / 0.8.6,
git and modded, one client per host at a time): every server answered the
challenge and accepted the connect, with and without the `rwin` offer. What
did fail or look like failing:

- **Two clients behind one public address.** A DarkPlaces server ignores
  every `connect` from an IP address (any port) for
  `net_connectfloodblockingtimeout` (5 s) after a connect from that address,
  and each ignored connect renews the block. The stock client asks again
  every second, so it never gets in: ten challenges answered, no accept,
  "Connect: failed, no reply". Seen with two test clients joining one server
  at once (10 of 10 connects ignored, on two hosts) and reproduced on
  loopback (`scripts/test-joinfail.sh`). It applies to a second device at
  home, to players behind one carrier-grade NAT address, and to a join from a
  new port right after an abandoned one.
  Now (`net_connect_backoff 1`): after four challenges answered with a
  connect and no accept, the client stays silent until 5.6 s after its last
  connect, once, and asks again. By then the block has ended and the join
  goes through, about 8.6 s after it began. Up to three lost connects the
  join is exactly as fast as stock. Four in a row, with every challenge
  answered, is the block, not packet loss (`scripts/test-net-connect.sh`):

  | connects ignored | stock | Godnotic |
  |---|---|---|
  | 0-3 | 0 / 1.0 / 2.0 / 3.0 s | the same |
  | 4 | 4.0 s | 8.6 s |
  | flood block (a second device joined just before) | never | 8.6 s |
- **Long joins that look dead.** A 0.8.5 / 0.8.6 server without a pack URL
  sends its 3.3 MB CSQC over the stock UDP download: 45-60 s at 130-270 ms
  ping. Servers with their own packs need them over HTTP first: 36 MB for the
  busiest Deathmatch server that day, 135 MB for one modded CTF server. The
  loading screen shows the download; nothing fails, but it is minutes on
  mobile data. Not changed here.
- **No reason and no way back.** A failed join only wrote to the console and
  the loading screen went away.

Now `touch_joinfail.c` watches a join from the first `getchallenge` to the
last signon and, when it ends short of the game, says why:

| `_join_fail_kind` | when | `_join_fail_text` / `_join_fail_detail` |
|---|---|---|
| `timeout` | ten requests, no answer at all | No answer from ADDRESS / The server is offline or restarting, or this network cannot reach it. Waited 10 seconds. |
| `busy` | challenges answered, connects ignored (after the back-off too) | ADDRESS answered but did not let you in / It is busy, or it limits joins from your network (someone on the same connection just joined). Try again in a few seconds. |
| `refused` | the server's `reject` | ADDRESS refused the join / The server says: TEXT (full, banned, wrong version: its own words) |
| `network` | no socket for the address family | No network connection for ADDRESS / for an IPv6 address: this device has no IPv6 connection right now |
| `dropped` | disconnected during the signon | ADDRESS closed the connection while joining / The server says: TEXT |
| `files` | the map could not be loaded after its download failed | Missing files: could not get the map NAME / The download from the server did not finish... |
| `error` | another `Host_Error` during the signon | The join stopped with an error / TEXT |

For every front end: `_join_fail_serial` counts up with each failed join and
`_join_fail_kind`, `_join_fail_text`, `_join_fail_detail`,
`_join_fail_address` describe the last one; `join_retry` connects to that
address again. `cl_connect_status` keeps the prefixes the native front ends
key on ("Connect: failed", "Connect: rejected, ") and now ends in the reason
("Connect: failed, no answer from ADDRESS", "Connect: failed, ADDRESS
answered but did not accept the join"); "Connect: the server is busy, asking
again in a few seconds..." is shown during the back-off. The touch menu
opens a **Could not join** dialog with the two sentences, **Retry** and
**Close** (`dialog_touch_joinfail.qc`, opened like the password prompt with
`menu_cmd directmenu TouchJoinFailed KIND ADDRESS`); a front end that shows
its own sets `touch_joinfail_dialog 0`. The password prompt keeps wrong and
missing passwords; the player's own cancel is not a failure; a drop that
`net_reconnect` answers by joining again is not reported.

Not verified: the tablet itself (the report came from the released Android
build; these runs were a desktop build of the same network code).

## Cvars and commands

| name | default | |
|---|---|---|
| `net_reliable_window` | 32 | fragments in flight to a peer that supports it; 0 or 1 = stock stop-and-wait (and no `rwin` offered or accepted) |
| `net_reliable_rto_min` | 0.2 | shortest retransmission timeout, seconds |
| `net_reliable_rto_max` | 1 | longest retransmission timeout, seconds |
| `net_signon_fast` | 1 | wake up for packets and answer at once during a signon; 0 = stock timing |
| `touch_net_trace` | 0 | print every packet of a join (`NETTRACE` lines); 2 = every packet, in the game too |
| `net_mtu` | 0 | path MTU for clients' datagrams; 0 = the route's MTU to each client, asked from the system every 10 s (see LAN.md, Playing over a VPN) |
| `curl_segments` | 4 | connections one large HTTP download may use; 1 = stock |
| `curl_segment_size` | 2097152 | smallest piece worth its own connection |
| `net_reliable_status` | | windowed connections: window, in flight, SRTT, RTO, retransmits |
| `net_reconnect` | 1 | join the same server again when the connection is lost |
| `net_reconnect_tries` | 5 | attempts |
| `net_reconnect_silence` | 30 | seconds without a packet before the connection counts as lost (3 after a freeze or an address change) |
| `_net_reconnect_state`, `net_reconnect_cancel` | | what the reconnect does or why it gave up; stop it |
| `net_away_spectate` | 20 | seconds in the background in a match before going to the spectators |
| `net_away_limit` | 600 | seconds connected in the background before a clean disconnect |
| `net_slist_fast` | 1 | the fast server list; 0 = the stock DarkPlaces scheduler |
| `net_slist_fast_rate`, `net_slist_fast_burst` | 1500, 48 | server queries a second at most, and in one frame, for a list that was opened or reset |
| `net_slist_saved` | 1 | keep the list between sessions and show it at once |
| `net_slist_hold` | 1 | no reordering while a finger is on the menu |
| `net_slist_stats`, `net_slist_stats_print`, `net_slist_open` | | timings of the last refresh; print them when a refresh settles; refresh as opening the list does |
| `_slist_summary`, `_slist_servers`, `_slist_players`, `_slist_refreshing`, `_slist_saved` | | "N servers · M players" and its parts; a refresh is running; the numbers still include saved rows that have not answered |
| `net_connect_backoff` | 1 | after four ignored connects, wait out a server's connect flood block once instead of renewing it every second |
| `_join_fail_serial`, `_join_fail_kind`, `_join_fail_text`, `_join_fail_detail`, `_join_fail_address`, `join_retry` | | the last failed join, for every front end; join it again |
| `touch_joinfail_dialog` | 1 | open the menu's Could not join dialog (a native front end that shows its own sets 0) |

Both features are on by default: the loss, reordering and duplication tests
below are clean, and a stock peer never sees either.

## Measuring

```sh
BASEDIR=<basedir> SV_ENGINE=engine/darkplaces/darkplaces-dedicated \
  scripts/bench-net.sh -n 3 0 25 50 100 50:2      # one-way delay[:loss %]
scripts/net-trace-report.py --rtt 100 <dir>/join-*.log
```

`bench-net.sh` runs `bench-load.sh`'s join scenario (a dedicated server and a
client in an `unshare -rn` namespace with `tc netem` on loopback) once with
both features off on both sides (`stock`) and once with the defaults
(`godnotic`), with `touch_net_trace 1`. `net-trace-report.py` splits each
join into the phases above and gives, per phase, the wall time, the client's
frame work (loading, rendering the loading screen), the rest (waiting for the
peer), that wait in round trips, packets, bytes and resends.

## Results

One desktop (16 threads, Mesa offscreen), stormkeep, a dedicated server
(`make sv-release`) and a client in an `unshare -rn` namespace, `tc netem`
on loopback (delay each way = RTT / 2), 3 joins per point, medians.
"stock" is both sides with `net_reliable_window 0` and `net_signon_fast 0`
(the stock code paths), "Godnotic" is the defaults. Join = the bench's
`game_ready.load`: from the `connect` command to the first game frame.
Phases from `net-trace-report.py` (wall ms; "wait" = wall minus the
client's frame work).

**Full data** (`xonotic-20261107` packs, so loading the map's models and
textures is ~0.7 s and the first frame's lazy loads ~1.7 s):

| RTT | loss | stock join | Godnotic join | stock serverinfo | Godnotic serverinfo | stock wait (RTTs) | Godnotic wait (RTTs) |
|---|---|---|---|---|---|---|---|
| 0 | | 3124 ms | 2640 ms | 378 | 44 | | |
| 50 ms | | 4153 ms | 3268 ms | 1355 | 47 | 31.8 | 4.5 |
| 100 ms | | 5823 ms | 2966 ms | 2345 | 43 | 27.1 | 4.2 |
| 200 ms | | 8446 ms | 3481 ms | 4315 | 45 | 27.2 | 4.0 |
| 100 ms | 2% | 6799 ms | 3504 ms | 3360 | 273 | 37.1 | 6.6 |
| 100 ms | 10% (2 runs) | 27836 ms | 4203 ms | 8886 | 345 | 250 | 12.0 |

Phase breakdown at 100 ms (ms; stock / Godnotic): connect 215 / 218,
serverinfo 2345 / 43, precache (local load) 843 / 678, signon 2 118 / 109,
spawn 126 / 111, begin and first frame 1949 / 1631 (local work: 1945 /
1631). The serverinfo's 22 fragments took 22.6 round trips stock and now
arrive in one burst behind the accept.

**Slim data** (the phone packs, `xonotic-20261107-*-phone`; LAN, 50 ms and
100 ms; measured while other jobs loaded the machine to a load average of
17-30, so the local-work phases are slower and noisier than on an idle
machine, the waits are not affected):

| RTT | stock join | Godnotic join | connect | serverinfo (stock / Godnotic) | precache (local) | signon 2 | spawn | begin + first frame | wait, RTTs (stock / Godnotic) |
|---|---|---|---|---|---|---|---|---|---|
| LAN (0 ms) | 2691 ms | 2371 ms | 23 | 423 / 93 | 1288 | 22 | 22 | 762 | - |
| 50 ms | 4049 ms | 2682 ms | 128 | 1391 / 79 | 1327 | 70 | 71 | 790 | 31.5 / 4.8 |
| 100 ms | 6846 ms | 4011 ms | 222 | 2383 / 106 | 1928 | 123 | 122 | 884 | 27.3 / 4.3 |

(Godnotic's phase columns; stock's precache, signon 2, spawn and begin are
within noise of the same local work and 1 round trip each.)

**What is left, round trip by round trip** (Godnotic, 100 ms): 4.2 round
trips of waiting.

| round trip | why it is needed |
|---|---|
| getchallenge → challenge | the server proves the client's address before it allocates anything (anti-spoofing); stock servers require it |
| connect → accept (+ serverinfo in the same burst) | the server allocates the slot and sends everything the client needs to load |
| prespawn → signon 2 data | the client asks for the baselines only once its models are loaded (its parser needs them) |
| spawn → signon 3 data | the server runs ClientConnect/PutClientInServer on spawn |
| begin → first entity frame | hidden behind the client's first-frame loading (1.6 s of local work), so it costs nothing here |

The floor with this protocol is therefore 4 round trips plus local work;
`connect`, `prespawn` and `spawn` could each be folded into the previous
step between Godnotic peers (What is left), for a floor of 2 round trips
(challenge, then connect with everything pipelined) plus local work.

**Downloads** (`scripts/bench-download.sh -s 40`, a valid 40 MB pack of
incompressible data, `curl --pak` from `tests/net/range_http.py`, timed from
the transfer's start to the pack mounted, sha256 compared; 1 run each, the
LAN row is noise-dominated: 0.3-4.9 s in repeated runs for both):

| link | per-connection cap | stock (1 connection) | Godnotic (`curl_segments 4`) |
|---|---|---|---|
| LAN (loopback, no shaping) | none | 0.4 s | 1.6 s (repeat: 0.3-4.9 s both) |
| Wi-Fi-like: 20 Mbit/s, 40 ms RTT | none | 19.9 s | 19.9 s (the link is full either way) |
| mobile-like: 30 Mbit/s, 80 ms RTT, 0.5% loss | none | 89.5 s | 30.6 s |
| lossy: 100 Mbit/s, 100 ms RTT, 1% loss | none | 183.6 s | 57.8 s |
| a host serving 1 MB/s per connection, 50 ms RTT | 1024 KiB/s | 40.3 s | 10.8 s |

Parallel ranges help exactly where one TCP connection cannot fill the link
(loss and latency, or a per-connection cap on the host), and cost nothing
where the link itself is the limit.


## Testing

- `scripts/test-net.sh`: join, a 60 s match with 4 bots and a map change
  with the client connected, at 0, 100 and 200 ms RTT, with 2% and 10% loss,
  with reordering and with duplication (Godnotic on both sides, window
  negotiated to 32), both features off on both sides, a Godnotic client
  against a stock server, a stock client against a Godnotic server and stock
  against stock, with a packet capture of the handshake. Stock engines:
  `upstream/vendor`'s `engine/darkplaces` built as is (`make sdl-release`,
  `make sv-release`), with upstream Xonotic data on both sides (`STOCKBASE`).
- `scripts/test-net-fuzz.sh`: hostile peers (`tests/net/hostile_peer.py`)
  against an AddressSanitizer + UBSan build of the server and of the client.
- `scripts/test-lan.sh`, `scripts/test-lan-logic.sh`: unchanged, still pass.

### Test results (this branch, desktop, network namespaces)

- `scripts/test-net.sh` (60 s matches with 4 bots and a map change each):
  38 checks passed. Godnotic on both sides at 0, 100 and 200 ms RTT, 100 ms
  with 2% and with 10% loss, 60 ms with 25% reordering, 60 ms with 10%
  duplication: joined both maps, no protocol errors, window 32. Captured
  handshakes: Godnotic to Godnotic `rwin\32` / `accept\rwin\32`; both
  features off: `accept`; Godnotic client to stock server: the client's
  `rwin\32` and the stock `accept` (window 0, joined both maps); stock
  client to Godnotic server: no `rwin`, the stock `accept`, joined both
  maps. The stock-to-stock reference run joined both maps once (with the
  stock client timing out at the end of the second map) and failed to start
  in a second run while `/tmp` was out of quota; it exercises no Godnotic code.
- `scripts/test-net-fuzz.sh` (AddressSanitizer + UBSan builds, 60 s each):
  a hostile client (171 rounds, 90 accepted with bogus or giant windows,
  79871 packets) against the server and a hostile server (4.6 million
  packets, 5 joins with bogus `accept\rwin\...`) against a reconnecting
  client: no sanitizer report from the network code, server alive and
  answering, memory bounded. The UBSan reports printed are upstream's
  (`prvm_edict.c:2788` at program load). A first version that sent random
  game messages to the client stopped it in the stock entity parser
  (`CL_ExpandEntities: cl.entities not initialized`, Sys_Error): stock
  behaviour on garbage entity data, outside this work.
- `scripts/test-lan.sh`: passed, VPN part included (labelled VPN addresses,
  found by the wg0 sweep and as a remembered /32, joined over the 1280 MTU
  link with the password: 2538 packets, largest UDP payload 1252 bytes, no
  IP fragments, remembered afterwards). `scripts/test-lan-logic.sh` passed
  (CI too).
- `scripts/test-net-away.sh`: all passed. Stopped (SIGSTOP) 5 s in a
  match and 5 s in the signon: still connected, no reconnect, the join
  completed. Stopped 60 s (and 60 s with 2% loss): Xonotic's servers set
  `net_messagetimeout 30`, so the server timed the client out; on return it
  joined again by itself. Stopped past a 40 s server timeout: same. Address
  changed in a match (10.88.0.1 to 10.88.0.9): seen within 2 s, joined
  again. `touch_away 1`: spectator after 5 s, `touch_away 0`: back in the
  game. An HTTP download (2 MB/s cap) stopped 10 s midway: completed,
  byte-identical. These runs needed the machine below a load average of
  about 30 (other agents' jobs): at 40 the client did not finish its signon
  in time for the scripted `cmd join`, and the script now says so instead of
  failing on a wrong reason.
- CI on `work/net`: Tests and Android green (the Android build compiles
  touch_android.c and the rest for arm64).

## Engine hooks

Logic lives in `touch_net.c`; upstream files carry single `// Touch:` lines:

| file | hook |
|---|---|
| `netconn.c` | include; `NetConn_Read`/`NetConn_Write`: `TouchNet_TracePacket`; `NetConn_SendUnreliableMessage`: `TouchNet_SendReliable`; `NetConn_Close`: `TouchNet_Close`; `NetConn_ReceivedMessage`: `TouchNet_Handles`/`TouchNet_Received`; challenge reply: `TouchNet_AppendConnect`; connectionless parse: `TouchNet_ClientConnectionless`; accept: `TouchNet_Established`; end of `NetConn_ClientParsePacket`: `TouchNet_ClientAfterParse`; server connect: new `if (!TouchNet_ServerAccept(...))`, repeated during the signon `if (!TouchNet_ServerDuplicate(...))`, rejoin of a begun slot `if (!TouchNet_ServerRejoin(...))`, each before the stock `accept`; after `SV_ReadClientMessage`: `TouchNet_ServerAfterRead` |
| `cl_input.c` | include; `CL_SendMove`: `important = important \|\| TouchNet_ClientUrgent();` |
| `cl_main.c` | include; `CL_DisconnectEx`: `TouchNet_Disconnecting(kicked, reason)`; `CL_EstablishConnection`: `TouchNet_ConnectAgain(address)` |
| `libcurl.c` | `#include "touch_net.h"`, `#include "touch_curl.h"`; `CURL_fwrite`: `TouchCurl_Clip`; `Curl_EndDownload`: `TouchCurl_End`; `Curl_Frame`: `TouchCurl_Frame`, `TouchCurl_Done`, `TouchCurl_Finished`; `Curl_GetDownloadInfo`: `TouchCurl_Info` |
| `cl_screen.c` | include; `SCR_DrawCurlDownload`: `TouchNet_CurlText` |
| `sv_send.c` | include; `SV_SendClientDatagram`: `TouchNet_ClampDatagram` |
| `makefile.inc` | `touch_net.o` in `OBJ_COMMON`; `OBJ_SV+= touch_null.o` |

The server list (`touch_srvlist.c`) adds to `netconn.c`: its include;
`ServerList_QueryList`: `TouchSrvList_Refresh(resetcache)` at the end;
`NetConn_QueryQueueFrame`: `if (TouchSrvList_QueryFrame()) return;`;
`..._ServerList_ProcessReply`: `TouchSrvList_Replied(n)`; and three one-line
accessors to static helpers (`TouchSrvList_ViewInsert`, `_ViewRemove`,
`_AddServer`). The join failure watch (`touch_joinfail.c`) has no hook of
its own: `touch_lan.c` calls it from `TouchLAN_Frame` and `TouchLAN_Init`.

`touch_null.c` gives the dedicated server build (`make sv-release`) the
client-only symbols Touch code refers to, and `touch_loadtime.c` no longer
uses the menu VM in that build: the dedicated server compiles again.

## What is left

- **Pipelined signon between Godnotic peers** (2 round trips less): the
  client could send `prespawn` before loading its models if it held the
  server's answer until the load is done (its parser needs the models for
  static entities), and `spawn` + `begin` could go in one request when the
  server says it accepts that (a key in `accept`). Floor then: challenge +
  connect, 2 round trips, plus local work.
- **Compressed signon payloads**: the serverinfo and precache lists (22 KB
  for stormkeep, highly repetitive) could be deflated between peers that
  advertise it (an `accept` key and a fragment flag bit); with the window
  they already cost one burst, so this only matters on links slower than
  about 2 Mbit/s. Not done: the engine's zlib wrapper inflates without an
  output bound, and a bounded inflate needs zlib calls outside fs.c.
- **Pack size and CRC from the server**: DarkPlaces' `curl --pak` lines carry
  no size or checksum. A Godnotic server could send them in a Godnotic-only
  stufftext (only to clients that offered `rwin`), e.g.
  `touch_curl_expect <pack> <size> <crc32>`, and the client compare after
  the download (the pieces already have to add up to the announced HTTP
  length).
- **Start the download before connecting**: a Godnotic server could list its
  map pack in its `infoResponse` (`\d_pak\<name>:<size>:<url>`); the client
  could start fetching when the player picks the server, before the
  challenge, saving the connect and serverinfo round trips and letting the
  download overlap the player's decision.
- **Slim map variants between Godnotic peers** (specification only): next to
  `<map>.pk3` a server may offer `<map>-slim.pk3` (textures recompressed by
  `scripts/repack-phone-assets.py`: zstd-compressed entries, method 93, ASTC
  or ETC2 textures and smaller mip levels; same BSP, same entity data, so
  gameplay is identical). It announces it only to Godnotic clients with a
  stufftext `touch_curl_slim <pack> <slimpack> <size> <url>`; a client that
  can read method-93 entries (every Godnotic build) fetches the slim one
  instead and mounts it under the original name's role (it provides the same
  `maps/<map>.bsp`). A stock client never sees the stufftext and downloads
  the full pack. Typical saving for a custom map: 40-60% of the bytes.
- **Insanotic interop** is by construction (the same keys and meaning), not
  tested against a running Insanotic engine (none built here).
- **Android, on a device**: the foreground-service and Wi-Fi-lock changes
  above (Android agent), and the on-device checklist.

### LAN joins: where the time goes, and what is not the network

On a LAN (sub-millisecond RTT) the network is not the bound: of the 2.6 s
full-data join at 0 ms, 38 ms is waiting for the peer (Godnotic; 410 ms
stock, mostly the server's tick and the client's frame limiter between
signon steps). The rest is local work in two lumps: loading the precached
models and their textures between signon 1 and `prespawn` (0.67 s), and the
lazily loaded models, sounds and shaders that the first game frame pulls in
(1.6-1.7 s, "begin" above). With the slim packs the same join is 2.4 s
on the loaded machine: 1.3 s of model and texture loading before
`prespawn`, 0.76 s of first-frame loading, 93 ms of serverinfo (the client
parsing 22 KB of precache lists and starting CSQC), and under 70 ms in all
the other steps together; on an idle machine the brief's 1.41 s has the
same shape.

For the agent on `work/perf3` (local load time; nothing of theirs was
edited here): the client could start loading the map's BSP as soon as the
serverinfo names it (signon 1), in parallel with the remaining signon steps
and the server's own work, instead of after `prespawn` is due; and the
first-frame lazy loads (`touch_preload`) could run during the signon's
waits, which on a real network are 3-4 round trips of idle client time
(0.3-0.8 s at 100-200 ms RTT) that are pure waiting today.
