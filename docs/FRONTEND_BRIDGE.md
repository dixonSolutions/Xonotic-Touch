# Front-end bridge (protocol 1)

The engine side of Godnotic's native interfaces: how a native UI (the Kotlin
views of the Android app; later the Rust loader core driving them) reads engine
state and drives the engine without touching engine structures from another
thread. Code: `engine/darkplaces/touch_frontend.c` / `.h`, the resident JNI
bridge `android/app/src/main/cpp/nativeui/bridge.c` (+ `engine.c`), and the
Kotlin side `android/app/src/main/kotlin/.../NativeUIApplication.kt`
(`NativeBridge`) and `.../fe/`.

The screens themselves are described by the menu VM through the **UI bridge**
([`UI_BRIDGE.md`](UI_BRIDGE.md), branch `work/uibridge`); this transport carries
its messages on the `ui` queue (below) and adds what is a platform matter: the
app state, attach/fallback, the heartbeat, power state, a load's Cancel, and a
few engine topics the stopgap screens and tests use.

## Transport

```
UI thread (Kotlin)                 libnativeui.so (resident)            engine thread (libmain.so)
------------------                 ------------------------             --------------------------
NativeBridge.enqueue("fe ...") --> bounded command queue (32)  -------> NativeUI_Frame: Cbuf_AddText
NativeBridge.frontendBeat()    --> heartbeat timestamp          <------ sink.beat_age_ms()
NativeBridge.frontendCancel()  --> cancel flag                  <------ sink.cancel_requested() (during loads)
NativeBridge.frontendTake()    <-- newest snapshot per topic    <------ sink.put(topic, json, 0)
                               <-- ordered queue (256)          <------ sink.put(topic, json, 1): events, "ui", "image"
onFrontendDirty() (posted)     <-- one wake-up until taken
```

- **Out**: a snapshot topic keeps only its newest JSON (a burst of changes costs
  one read); queued messages (events, the UI bridge's `ui` messages, `image`
  replies) keep their order. Past 256 queued messages an `overflow` event tells
  the UI to ask for fresh state (`fe get app`, UI bridge `sync`). The bridge calls
  `NativeBridge.onFrontendDirty()` on the engine thread once until the UI has
  taken what is waiting; the UI posts the read to its own thread.
- **In**: `fe <verb> <args>` console commands through the existing queue, which
  only the engine thread drains. Every argument is percent-encoded by the UI
  (`FeArgs.encode`: letters, digits and `-_.:@+,=` stay, every other byte is
  `%XX`; `~` is the empty string), so a name or a password with spaces, quotes,
  `;`, `$` or `//` arrives as one token and never as console syntax. Control
  bytes are dropped on decode.
- **Versions**: `nativeui_fe_version()` / `NativeBridge.frontendVersion` and the
  app topic's `proto` are 1. The engine installs its sink only when the resident
  bridge has the `nativeui_fe_*` symbols; the Kotlin side treats a missing native
  method (`UnsatisfiedLinkError`) as "no front end". Any mismatch means: no native
  interface, the QuakeC menu as before.
- **Desktop** (tests): no sink; `fe_dump <topic>` prints a topic as it is now,
  `fe_take` prints the queued events and the topics that changed, `fe_echo 1`
  prints everything as it is sent (`FE> topic json`), `fe beat` stands in for the
  heartbeat.

## Owning the menu, and falling back

`touch_native_frontend 1` (XonoticActivity passes it when the bridge is new
enough and `<basedir>/no-native-ui` does not exist) lets an attached UI own the
menu. While `TouchFE_MenuHidden()` is true the QuakeC menu VM stays loaded and
keeps its key_dest bookkeeping, so a local game still pauses under the native
pause menu exactly as under the QuakeC one, and its toggles still run, but
`MP_Draw` and `MP_KeyEvent` return at once. It is true only while all of:

- `touch_native_frontend` is 1,
- the UI has attached (`fe attach 1`, protocol 1) and not detached,
- the UI's heartbeat is younger than 4 s (`NativeBridge.frontendBeat()` once a
  second while the activity is resumed; a frozen or crashed UI layer stops it),
- the menu has not been handed back on purpose (`fe menu qcmenu`, Advanced).

Otherwise the QuakeC menu is drawn and takes keys again, by itself, in the next
frame. A heartbeat missing for 30 s detaches the UI. `_fe_state` says which case
holds: 0 not attached, 1 the native UI owns the menu, 2 attached but the QuakeC
menu is in charge.

The hidden menu does not run `m_draw`, where it notices that a join handed the
keys to the game; `TouchFE_Frame` tells it (`MR_ToggleMenu(0)` on the
menu-to-game transition), so the pause button's `togglemenu` keeps opening it.

**Drawing behind a native screen**: `fe screen <name> <cover>` (0 see-through,
1 dimmed, 2 opaque). With 2, while the UI owns the menu and the player is in the
menu or a join (never in a match), `CL_UpdateScreen` draws nothing
(`TouchFE_SkipDraw`) and the frame limiter uses `cl_maxidlefps`
(`TouchFE_MaxFPS`); sound, the network and the menu VM's toggles go on. With 1
only the frame rate drops.

## Topics (engine to UI)

| Topic | Sent | Content |
|---|---|---|
| `app` | on change, checked every frame (a few integers) | `proto`, `enabled`, `attached`, `owns_menu`, `advanced`, `state` (`menu`, `connecting`, `loading`, `ingame`, `paused`), `menu`, `console`, `local`, `paused`, `hosting`, `maxclients`, `map`, `server`, `demo`, `version`, `ui_bridge` (the engine describes screens through the UI bridge), `reconnect` (what the automatic reconnect of touch_net.c is doing, "Reconnecting to ... (attempt n of 5)"; empty when none runs), `reconnect_note` (why it gave up, `_net_reconnect_state`) |
| `connect` | every frame of a join or a load (on change), once more when it ends | `stage` (`connecting`, `downloading`, `loading`, `signon`, `idle`), `status` (cl_connect_status), `address`, `signon`, `signons`, `downloads`, `file`, `download_progress`, `speed`, `load`, `load_progress`, `local` |
| `cvars` | 4 Hz while a native screen shows, on change | the watched cvars (`fe watch`), `null` for one outside the schema |
| `servers` | 4 Hz while subscribed, on change | `refreshing`, `lan_only`, `favorites`, `rows` (`a` address, `n` name, `m` map, `g`, `mod`, `p` ping, `h` humans, `b` bots, `x` max, `pr`, `q` qcstatus, `l` locked, `f` favourite, `lan`), `count` |
| `lan` | 4 Hz while subscribed, on change | the Host screen's settings and the host status (`hosting`, `on_purpose`, `port`, `addresses`, `players`, `playercount`, `others`, `port_free`, `port_suggest`, `password_set` ...), and `cvars`: every non-private `touch_lan_*`/`net_slist_lan*` cvar, so what touch_lan.c adds (labelled VPN addresses, join links) arrives without a protocol change. Never a password. |
| `maps` | `fe get maps` | `list`: `name`, `title`, `author`, `description`, `types` (mode ids from `maps/*.mapinfo`, plus the modes Xonotic allows on any map of a listed one) |
| `gametypes` | `fe get gametypes` | `list`: `id`, `cvar`, `title`, `teams`, `current` |
| `mods` | 4 Hz while subscribed | the manager's cvars as they are: `mods`, `plugins`, `permissions`, `select`, `detail`, `install`, `last` |
| `player` | while subscribed | `name`, `name_plain`, `model`, `skin`, `color` |
| `image` (queued) | `fe image <ref>` | `{ref, path}`: `pk3:<path>` (with or without extension; `.jpg`, then `.png`) or a map name, copied out of the packs to `<userdir>/data/fe_cache/`; `path` is `""` when there is none |
| `event` (queued) | as they happen | `{"type":"password","kind":"required"\|"wrong","address"}`, `rejected` (`reason`), `failed`, `disconnected`, `refused` (a cvar outside the schema), `host_failed` (`reason`, `suggest`), `play_failed`, `overflow` |
| `ui` (queued) | the UI bridge's messages | docs/UI_BRIDGE.md |

## Intents (UI to engine): `fe <verb> ...`

| Verb | Arguments | Does |
|---|---|---|
| `attach` | protocol | attaches (refused with an event on another protocol) |
| `detach` | reason | lets go; the QuakeC menu is in charge |
| `beat` | | heartbeat (desktop; Android uses `NativeBridge.frontendBeat`). The engine detaches a UI that has not beaten for 30 s, but not while its window is inactive (the app is in the background and does not beat); the UI attaches again when the `app` topic says it is detached |
| `screen` | name, cover 0/1/2 | what the UI shows (`none` when nothing); see Drawing |
| `sub` / `get` | topic [0/1] | subscribe to a slow topic / publish one now |
| `watch`, `watch_add` | cvar... | the cvars topic's set |
| `set` | cvar, value | only cvars in the schema (prefix list in `cvar_allowed`), never `CF_PRIVATE` or `CF_READONLY`, never a name with `password` or `rcon` |
| `refresh` | `internet`\|`lan` [`keep`] | server list query (`net_slist_lan_only` for LAN) |
| `favorite` | address, 0/1 | edits `net_slist_favorites` |
| `connect` | address | `connect "<address>"` |
| `password` | address, password, [remember 0/1] | the password prompt's Join: the password only goes into the private cvar touch_lan.c answers the challenge from |
| `cancel` | | cancels a join (the loading screen's CANCEL path) and the automatic reconnect; during a map load `NativeBridge.frontendCancel()` is read between items |
| `reconnect_cancel` | | `net_reconnect_cancel`: the reconnect bar's Cancel |
| `portcheck` | port | `_touch_lan_portcheck` |
| `host` | name, mode, map, bots, slots, port, assist, [password], [remember] | the Host screen's Start (dialog_touch_lan.qc), port check included (`host_failed` with `suggest`) |
| `stophost`, `clear_host_password` | | |
| `play` | mode, map, bots, [skill], [timelimit], [fraglimit], [g_mutator...] | a local bot match |
| `menu` | `resume`, `leave`, `quit`, `console`, `open`, `qcmenu [dialog]`, `native` | pause menu actions; `qcmenu` hands the QuakeC menu back (optionally at a dialog), `native` takes it again |
| `mod` | verb, realm (`mod`\|`plugin`), name, [arg] | `select`, `enable`, `disable`, `grant`/`revoke` permission, `uninstall [keepdata]`, `install <source>`, `confirm`, `cancel` (the `lua_*` commands the Mods dialog runs) |
| `image` / `preview` | ref / map | picture out of the packs (topic `image`) |
| `exec` | cfg | a Touch preset (`touch/...`, `touch/profiles/...`), never a path out |
| `save` | | `saveconfig` |
| `ui` | json | the UI bridge's renderer messages, when the engine has registered the `ui` verb (`TouchFE_RegisterVerb`, for touch_uib*.c) |

Kotlin also sets, through `fe set`, what the menu-idle warm-up (touch_warm.c) reads:
`touch_device_thermal` (`PowerManager.getCurrentThermalStatus()`, on every change),
`touch_device_powersave` (battery saver, on its broadcast), `touch_device_charging`
(plugged in, on ACTION_POWER_CONNECTED/DISCONNECTED; under battery saver touch_warm
runs fully while charging and otherwise one reduced pass, `touch_warm_saver`) and `touch_warm_map` (the
map Home's Play starts), plus `_android_idle_home`. `onTrimMemory`/`onLowMemory`
send `touch_warm_drop`. Behind an opaque native Home nothing is drawn but the
warm-up still runs (the `CL_UpdateScreen` hook calls `TouchWarm_Frame` before it
returns), and the engine still counts as visible and focused: the native screen
changes neither `vid_hidden` nor `vid_activewindow`.

## For other engine modules

```c
void TouchFE_Put(const char *topic, const char *json, int event); // send on the transport
void TouchFE_RegisterVerb(const char *verb, touchfe_verb_fn fn);  // receive `fe <verb> <json>`
```

The UI bridge (touch_uib*.c) registers `ui` and puts its messages on topic
`ui` with `event = 1` (ordered, never coalesced). Registering `ui` sets the app
topic's `ui_bridge`, which tells the Android side to send its `hello`.

## Upstream hooks

All one line, marked `// Touch:`:

| File | Line |
|---|---|
| `menu.c` `MP_KeyEvent` | `{ extern qbool TouchFE_MenuHidden(void); if (TouchFE_MenuHidden()) return; }` |
| `menu.c` `MP_Draw` | the same |
| `vid_sdl.c` `Sys_SendKeyEvents` | `{ extern void TouchFE_Frame(void); TouchFE_Frame(); }` |
| `cl_screen.c` `CL_UpdateScreen` | `{ extern qbool TouchFE_SkipDraw(void); if (TouchFE_SkipDraw()) { TouchWarm_Frame(); return; } }` |
| `cl_screen.c` `SCR_PushLoadingScreen` | `{ extern void TouchFE_Loading(const char *, float); TouchFE_Loading(s->msg, s->absolute_loading_amount_min); }` |
| `cl_main.c` `CL_Frame` | `{ extern double TouchFE_MaxFPS(double); maxfps = TouchFE_MaxFPS(maxfps); }` |
| `makefile.inc` | `touch_frontend.o` in the Touch object list |

`TouchFE_Init` is called from `TouchHook_Init` (touch_hook.c, Touch-owned).

## Tests

`BASEDIR=<basedir> scripts/test-frontend.sh` (game data needed, so not in CI):
every topic, attach / heartbeat / switch-off / Advanced / detach fallbacks,
the cvar schema (private cvars neither readable nor writable, a percent-encoded
`;quit` inside a name stays text), a local bot match through `fe play` with the
pause menu hidden and the game paused under it, resume and leave, and two engines
on the loopback: a LAN game hosted through `fe host` with a password, found as a
favourite, joined through `fe password` after a wrong one. 40 checks.

The Kotlin model (UI bridge store and client, list windows, row diffs, sticky
placement, `fe` encoding, join links) has JVM unit tests:
`gradle :app:testReleaseUnitTest` in `android/` (`src/test`), run against every
UI bridge fixture in `tests/uibridge/fixtures` when the tree has them.
