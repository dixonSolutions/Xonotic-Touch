# LAN games

Play with people on the same Wi-Fi, hotspot or wired network: one player hosts,
the others find the game in their server list and join. No master server, no
typed address, no Xonotic server scripts. Works the same on Linux (Flatpak),
Android and Ubuntu Touch, and between them. Not in the browser build: a web page
has no UDP, so there the Host screen says so and its buttons are off.

## Host

**Multiplayer → Servers → Host LAN...** (or, in a match on a touchscreen, the
game menu's **LAN game** button). The Host screen:

| Setting | Default | Notes |
|---|---|---|
| Server name | Godnotic server | what others see in their list |
| Game mode | Deathmatch | DM, TDM, CTF, Clan Arena, Freeze Tag, Duel, LMS, Keepaway, Key Hunt, Domination |
| Map | stormkeep | or **Random**; a map without the chosen mode is swapped for a random one that has it, and the screen says so. After the first map the server's normal rotation for the mode follows |
| Bots | 4 | they take player slots too; slots are raised to bots + 2 so at least one friend fits |
| Slots | 8 | |
| Port | 26000 | UDP, 1024-65535. If it is taken (another game or app on this device), **Start** says so, puts the next free port in the box and asks you to press Start again |
| Password | none | optional, see below. **Remember on this device** keeps it for next time |
| Touch assists | All | what `sv_touch_assist` allows players' touch controls: all, no auto-shoot, aim assist only, or none |

These are the launcher's Host page defaults too (name, port, slots, bots, map),
so both start the same game.

**Start hosting** starts a listen server in the game itself: you play on it as
well. The screen then shows the port, every address others can use (one per
network interface, e.g. `Join at: 192.168.1.23:26000 10.42.0.1:26000`) and who
is in. Only interfaces others on the local network can reach are listed, Wi-Fi,
Ethernet and hotspot ones first (`wlan*`, `swlan*` when the phone is the
hotspot, `eth*`, ...); mobile data (`rmnet*`, `ccmni*`, ...), point-to-point
links and VPN tunnels are left out, and the LAN search sends no broadcast on
them either. **Stop hosting** (or leaving the match) ends the game for
everyone. On Android a game hosted from this screen keeps running while the
app is in the background; a bot match started from Create or Singleplayer
pauses ([ANDROID.md](ANDROID.md)).

The LAN filter on the Join screen (`net_slist_lan_only 1`) asks no master
server: no master address is resolved and nothing is sent to one (checked by
logging every `sendto`/`connect`/`getaddrinfo` of a search), and `net_slist`
then prints "Searching the local network for games".

What the Host screen sets, and why:

- `sv_public 0`: the game answers the LAN search (`getinfo`/`getstatus`) and is
  never registered with the public master servers. `-1` would hide it from the
  LAN search as well (the engine then ignores those queries), and `1` would
  list it publicly.
- `sv_status_privacy 1`: `status` does not show players' IP addresses to others.
- `maxplayers`, `port`, `hostname`, `bot_number`, `sv_touch_assist`, the
  `g_<mode>` cvar of the mode, then `map`.
- No `rcon_password`: no remote console unless you set one yourself.
- Network rates, `sv_curl_defaulturl` and voting stay at Xonotic's defaults
  (`xonotic-server.cfg`), which suit a LAN.

On Android the game keeps running while you switch apps or turn the screen off,
like an online match (a notification says "Hosting a LAN game on port ...").
While hosting the app also holds a Wi-Fi multicast lock, without which many
phones drop the broadcasts other players search with, and a low-latency Wi-Fi
lock against power-save lag. Both are released when hosting stops
([ANDROID.md](ANDROID.md)).

A dedicated server works too: the launcher's Host page, or
`darkplaces -dedicated +sv_public 0 +port 26000 +maxplayers 8 +map stormkeep`,
plus `+sv_touch_lan_password <secret>` for a password.

## Join

**Multiplayer → Servers**, tick **LAN**. The list then asks only the local
network, without the master servers, and LAN games show within about a second
under the **LAN** category, first in the list, with their ping. A game that needs
a password has a padlock in the icons column. Without the LAN box, LAN games are
listed too, under LAN ahead of the internet ones (with **Categories** on).

Pick the game and **Join!**. A locked game asks for the password first, in a
dialog with a large field (the on-screen keyboard opens on a touchscreen); a
wrong password brings the same dialog back with "Wrong password ... Try again"
instead of a dropped connection.

**Join by address**: type `192.168.1.23` or `192.168.1.23:26001` in the
**Address** box under the list (the Host screen shows the exact address) and
press Join. Use it when the search finds nothing, e.g. across subnets.

How the search works (engine `touch_lan.c`): DarkPlaces on its own sends one
query to 255.255.255.255, port 26000. Godnotic also sends a subnet-directed
broadcast on every network interface (e.g. 192.168.1.255), which reaches the
right Wi-Fi even when the phone's default route is mobile data or there is no
internet at all (where 255.255.255.255 fails outright with "network
unreachable"); asks ports 26000-26004 (`net_slist_lan_ports`), so a game moved
to the next free port is found; and repeats the query twice 0.35 s apart
(`net_slist_lan_repeats`, `net_slist_lan_repeatdelay`), since Wi-Fi drops
broadcasts. The servers that answered last time are asked directly at once
(`net_slist_cache`, kept in `data/touch_serverlist_cache.txt`), and the wait
before the list's own pings is 1.5 s instead of 4 (`net_slist_timeout`). IPv6
multicast is not used: the engine's address strings drop a link-local
address's scope, so a game found at `fe80::...` could not be joined; every LAN
seen so far that carries IPv6 carries IPv4 too.

## Password

Optional, set on the Host screen (`sv_touch_lan_password` on the server).

Xonotic has no join password at all (its `g_password`-style cvars do not exist
in DarkPlaces; only `rcon_password` does, for the remote console), so Godnotic
adds one. It is a **shared secret for a trusted LAN**, meant to keep strangers
on the same café or school Wi-Fi out of your game, not to protect anything
valuable:

- The client never sends the password. The server already sends each joining
  client a random challenge tied to its address; the client answers with
  HMAC-MD4(key = password, challenge), as DarkPlaces' `rcon_secure 2` does,
  and the server compares it with its own. Someone watching the network cannot
  replay that answer from another address, but could try to guess a short
  password offline from one captured exchange: use a few words, not a PIN.
- The proof only goes to the server the password was typed for
  (`_cl_touch_lan_password_server`); another server never gets one.
- Typed passwords live in memory only (`_cl_touch_lan_password`, never written
  to `config.cfg`). With **Remember on this device** they are kept in
  `data/touch_lan_passwords.txt` in the game's data folder, **as plain text**,
  one `address<TAB>password` per line (the host's own as `(host)`).
  `touch_lan_forget [address]` deletes them.
- Neither password is ever printed to the console or its log, and both cvars are
  `CF_PRIVATE`: a server cannot read them through `$`-expansion in a stuffcmd.
- A stock Xonotic client is refused with the readable reason
  "[pw-required] This game needs a password."; it cannot send the proof.
- The game itself is plain UDP, like any Xonotic game. If both sides have
  `d0_blind_id` (Linux builds that ship `libd0_blind_id`; not the Android or
  web builds), DarkPlaces authenticates the server and, when one side asks for
  it with `crypto_aeslevel` 2 or 3, encrypts the connection, the join proof
  included. Without it nothing is encrypted.

## Ports and firewalls

The host listens on **UDP 26000** (or the port you chose); the search goes out
to UDP 26000-26004 as broadcasts and the replies come back to the client's
random UDP port. Nothing needs TCP.

| Platform | What to know |
|---|---|
| Linux Flatpak | `--share=network` gives the game the host's network; nothing else is needed. A host firewall must let UDP 26000 in: `sudo firewall-cmd --add-port=26000/udp` (Fedora, add `--permanent` to keep it) or `sudo ufw allow 26000/udp` (Ubuntu). Joining needs no rule (replies are tracked) |
| Android | No firewall to configure. Hosting holds the multicast lock described above. Many guest networks and some hotspots use **client isolation**, which blocks devices from talking to each other: nothing is found and joining by address times out too. Use a phone's own hotspot instead (the phone hosting the hotspot can host the game as well; its address is then usually 192.168.43.1 or 10.x.x.1) |
| Ubuntu Touch | The click's AppArmor policy has the `networking` policy group, which allows UDP sockets, binding a port and broadcasts. Not tried on a device |
| Windows (if built) | The extra subnet broadcasts are not compiled in; only the stock 255.255.255.255 query and the extra ports. Windows Firewall asks to allow the game the first time it hosts |
| Web | No UDP in a browser: no hosting, no LAN search |

## Troubleshooting

- **Nothing in the LAN list.** Are both on the same network (same Wi-Fi name,
  not one on 2.4 GHz guest and one on the main one)? Is the host's port
  allowed through its firewall (Linux)? Is it a guest or school network with
  client isolation? Try **Address** with the host screen's `Join at:` address:
  if that times out too, the network is blocking device-to-device traffic.
- **Found, but joining times out.** A firewall that lets broadcasts out but not
  the game in, or a VPN on one device: turn the VPN off.
- **"Port 26000 is in use".** Another copy of the game, a dedicated server or
  another app has it. Take the offered next port; the search covers 26000-26004.
- **The other player is missing the map.** Host one of the stock maps, which
  every Godnotic install has; a custom map has to be on both devices.
- **Lag on Android with the screen off.** The low-latency Wi-Fi lock needs
  Android 10+; older phones may add power-save delay.
- `net_slist_debug 1` prints every query and reply; `touch_lan_status` prints
  the hosted game's port, addresses and players.

## Tests

`scripts/test-lan-logic.sh` (in CI, `.github/workflows/tests.yml`) checks the
pure parts: directed broadcast addresses, LAN address ranges, which network
interfaces are listed, whether a game keeps running in the Android
background, the lock key in the server's status, the password reject markers
and port lists.

`scripts/test-lan.sh` runs the real engine end to end and needs game data, so it
is not in CI:

```sh
BASEDIR=<dir with data/*.pk3 and the Godnotic xonotic-data.pk3dir> \
  LD_LIBRARY_PATH=<dir with libjpeg.so.62> scripts/test-lan.sh
# SHOTS=<dir> also takes screenshots of the Host screen, the LAN list and the
# password prompt at 2400x1080 and 1920x1200.
```

It builds two network namespaces joined by a virtual Ethernet cable
(`unshare -rn`, no root), 10.77.0.2 for the host and 10.77.0.1 for the clients,
with no default route. In them: a listen server hosts with a password; the
Host screen's port check reports 26000 taken and offers 26001; a second server
asked for 26000 moves to 26001; a client finds both games through the LAN
search alone (no address typed, no master server asked); the game's status
carries the lock key; joining with no password and with a wrong one is refused
with the right reason; joining with the right one connects; the password is in
no log; and the host lists the joined player.

What that proves: the engine's discovery, password and port handling across a
real (virtual) Ethernet link between two network stacks, including the
no-default-route case where the stock query fails. What it does not: Wi-Fi.
A veth link never drops or delays a broadcast, has no access point that
filters broadcasts or isolates clients, no power-saving radio and no Android
multicast filter. The repeats, the multicast lock and the Wi-Fi lock are there
for exactly those, and only a test with real devices on a real Wi-Fi shows how
well they work.
