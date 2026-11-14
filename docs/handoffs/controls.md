# Handoff: in-match touch HUD and controls (`work/controls`)

Written 2026-11-14 by the controls agent for whoever coordinates next.
Scratch clone: `~/.cache/godnotic-work/controls`. Based on `origin/main` 10c2d32.
Nothing here was run on a device or felt with a finger: every check is
offscreen, with scripted fingers or pure maths.

## Commits on `origin/work/controls`

| Commit | What | CI |
|---|---|---|
| db6cd1c | Join is Xonotic's own join dialog; one top-bar layout | Tests + Android green |
| 2caee35 | The same over the redesigned menu and with a native front end; `_menu_alpha` fix | Tests + Android green |
| ee0e7b4 | docs/CONTROLS.md for the two above `[skip ci]` | - |
| a88bbd5 | Controls: floating analog stick, look in degrees per dp with acceleration, every held button aims | (pushed together with the next) |
| 2eb65ea | Settings: Touch controls | Tests + Android green |
| (this)  | docs: review, CONTROLS.md feel and settings sections, this file `[skip ci]` | - |

Tasks 1 and 2 (join, top bar) end at ee0e7b4 and can be merged without the
rest. Tasks 3 to 6 are a88bbd5, 2eb65ea and the docs commit.

`components.json` is untouched and `scripts/component-push.sh` was not run
(the brief said not to). `engine/darkplaces` and
`engine/data/xonotic-data.pk3dir` both changed, so the pins have to be made at
merge.

## Done and verified

All of these pass on the tree at 2eb65ea.

```bash
bash scripts/test-touch-hudmode.sh      # modes, menu actions, when the join dialog opens, what its rows send
bash scripts/test-touch-spectate.sh
bash scripts/test-touch-controls.sh     # top bar at 4 sizes, look and stick maths, both HUD copies agree
python3 touch/gen-defaults.py --check
bash scripts/test-old-name.sh
bash scripts/test-touch-profiles.sh

# live: needs a built engine, a Godnotic basedir and a basedir with upstream data only
export PATH=~/.cache/xt-assets/bin:$PATH
ENGINE=$PWD/build/bin/xonotic BASEDIR=~/.cache/godnotic-work/controls-base \
STOCKBASE=~/.cache/godnotic-work/controls-stock LIBS=~/.cache/godnotic-slimtest/lib CPUS=16-23 \
OUT=/some/dir bash scripts/test-touch-controls-live.sh [input] [join] [local] [rects]
```

The live test, last full run (84 checks, all ok, log
`~/.cache/godnotic-work/controls-live-c2.log`). The server in it is a
dedicated server with stock Xonotic 0.8.6 game code, so the client runs the
engine HUD, as on every public server; the local cases run the port's CSQC HUD.

- **input** (both HUDs, 1920x1200 at 283 dpi and 2340x1080 at 395 dpi): a slow
  2 cm swipe turns 72.6 degrees (70.2 in runs where the first move arrived in
  the frame of the touch), the same at both densities within 4 %; a finger on
  FIRE fires, dragging 2 cm off it turns 72.6 degrees with attack still held,
  lifting stops the fire; the same off HOP with jump held; HOP held 3.3 s
  takes off 4 to 5 times; the stick pushed up-right moves at the thumb's angle
  (45 and 36 degrees for the two aspect ratios) and stops on lift; the same
  2 cm as an 80 ms flick turns 118 to 131 degrees.
- **join** (DM and CTF): the dialog is up for an observer at match start;
  Spectate keeps observing, the dialog is gone and does not come back; MENU >
  Join game reopens it; Join (a team in CTF) gives mode playing and the dialog
  closes itself.
- **local**: a Deathmatch started with `map` puts the player in the game with
  no dialog.
- **rects** (1920x1200, 2340x1080, 1280x720): spectating, playing, with the
  scoreboard, dead, intermission, map vote: no two controls intersect, nor the
  score list, clock and info messages (`touch_hud_state rects`).

Also checked by hand, headless:

- `make sv-release` in `engine/darkplaces` and `scripts/build-web.sh
  --engine-only` build at 2eb65ea.
- The join popup with a native bridge renderer attached
  (`~/.cache/godnotic-work/controls-tools/native/run.sh`:
  `touch_native_frontend 1`, `fe attach`, a hello through `fe ui`, menu VM
  hidden from the start): `app.modal == ["join"]` with the menu up;
  `join.spectate` keeps spectating and takes the menu down; `touch_action join`
  brings it back; `join.auto` gives mode playing with the menu down.
- Settings: both renderings open and show the rows
  (`controls-shots/set_qc_*`, `set_uib_*`).
- Screenshots, looked at: `~/.cache/godnotic-work/controls-shots/`
  `before_{local,remote}_{tablet,phone,desk}/` (old main; `before_remote_desk`
  is missing, that run died), `c1_*` (join + top bar only, at 2caee35),
  `final_*` (everything; a rerun was still in progress when this was written,
  the `final_remote_phone` set from the earlier run is mistimed: it started
  during the loading screen). Each has a `_sheet.jpg`.

## Not done

- **No device run.** Look speed, acceleration, stick size and the 0.6
  full-speed point are first values. The checklist below is for the tablet.
- **Kotlin side unverified** (see the contract below).
- **Haptics**: the engine has `touch_haptics` and ticks on every button press,
  but no platform gives it a way to vibrate (`_touch_haptics_available` 0).
  Android needs the VIBRATE permission and to set `TouchLua_VibrateHook`. The
  settings row hides itself until then.
- **Gyro aim**: cvars only, no sensor is read.
- **Separate sensitivity while firing or zoomed**: not added.
- **Fewer buttons** (KEEP and TURN are now mostly redundant): left for the
  owner to judge after trying HOP-drag and the flick.
- **Left-handed**: the top bar mirrors; the game's own panels keep their stock
  places.
- **docs/UI_BRIDGE.md** does not describe the `join` popup or the new
  settings sub-tab (it is the screens agent's file); CONTROLS.md does.
- **Layout editor and the top bar**: with `touch_topbar_auto 1` the engine
  writes the three pills' positions every frame, so a pill dragged in the
  editor snaps back. Set `touch_topbar_auto 0` to hand-place them. The editor
  does not do that by itself.
- **Slow-swipe first frame**: when the first move of a touch arrives in the
  same frame as the touch, that frame's travel is not counted (about 4 dp,
  3 % of a 2 cm swipe). Seen only with scripted events.

## Known bugs and oddities

- **Phone game menu**: at 2340x1080 the touch game menu is a scrolling sheet
  that opens showing Resume, Leave and Quit; Join game / Spectate / Settings
  are further down. It was like that before this work
  (`before_remote_phone/06_menu.png`). Not the HUD's; the menu's layout.
- **Join dialog in Deathmatch** has an empty band where the team row would be.
- **Stock server's `spec`** takes a few seconds (the server's own delay);
  the mode reads playing until then.
- The live test is timing-based. A case that fails is run a second time by the
  script; on a machine at load 30 a third run can be needed. Do not build
  QuakeC in the clone while it runs: it reads the `.dat` files from the clone.

## Defaults changed

| What | Before | After | Reason | Back |
|---|---|---|---|---|
| Joining | swipe-up handle at the bottom edge | Xonotic's Team Selection dialog, opened by itself for an observer at match start, and from MENU | the owner: "just show the join popup from xonotic"; a swipe up from the bottom edge is Android's Home | `touch_join_dialog_auto 0` |
| Local game without teams | starts as an observer | joins by itself | nobody starts a local match to watch it | `touch_local_autojoin 0` |
| Top bar | pills at screen fractions, three modules with their own maths | one layout in dp from the corner; the game's panels placed around it | the owner: "Top buttons overlap" | `touch_topbar_auto 0` |
| Look unit | fraction of the screen width | degrees per dp: `dp x 0.16 x touch_sens_base x gain` | the same swipe in cm turns the same angle on every screen (was 28 deg/cm on the tablet, 32 on a phone) | - |
| `touch_sens_base` | 2.8 | 3.6 | "more sensitivity for mobile"; 2.8 is the old speed on the 283 dpi tablet | `2.8` |
| `touch_look_accel` (new), `_lo`, `_hi` | - | 0.8, 150, 900 dp/s | a slow drag stays exact, a flick turns up to 1.8 times as far | `touch_look_accel 0` |
| Look smoothing | 1 euro filter on velocity, residue dropped on lift | on the turn still owed, the lift pays the rest | fast flicks lost part of their turn | `touch_look_filter 0` |
| `touch_look_fcmin` / `touch_look_beta` | 3.0 / 0.03 | 4.0 / 0.05 | the cutoff now reads dp/s | set them |
| Button drag-look | FIRE, HOOK, NADE; ignored while another finger looked | also ALT, HOP, DUCK, ZOOM (`touch_button_drag_look` 1, new); adds to other fingers; dead-zone travel counts | "shoot button intercepts mobile swipe" | `touch_button_drag_look 0`, `touch_fire_drag_look 0` |
| Stick centre | fixed | floats to the thumb, follows it past the rim (`touch_stick_float` 1, `touch_stick_follow` 1) | no reaching for the ring | `0` |
| Stick output | 8 directions (keys) | analog, full speed from 0.6 of the radius (`touch_stick_analog` 1, `touch_stick_run` 0.6) | strafe-jumping needs the angles between the keys; full deflection is exactly the keys' speed | `touch_stick_analog 0` |
| `touch_move_size` | 0.229 | 0.300, never under 128 dp across | "larger joystick"; 1.6 cm on a phone | `0.229` |
| `touch_stick_deadzone` | 0.18 | 0.12 | a floating stick starts centred | `0.18` |
| `touch_fire_size` | 0.140 | 0.160 | under 1 cm on a phone | `0.14` |
| `touch_crouch_size` | 0.066 | 0.080 | 45 dp on the tablet | `0.066` |
| Smallest touch target | 44 dp | 48 dp | Material's minimum | - |
| Layout schema | 7 | 8 | a saved layout keeps what the player changed; values at an old default take the new one | - |
| `touch_haptics` (new) | - | 1, inert until a platform can vibrate | | `0` |

Reasons at length, and what was measured: `docs/TOUCH_CONTROLS_REVIEW.md`.
Every cvar: `docs/CONTROLS.md` (Joining, The top bar, Feel, Behavior, Touch
controls settings).

## What the Kotlin renderer must do

Engine and menu side are done; nothing new in the protocol.

**Join popup.** With a native bridge renderer attached, the join dialog is a
bridge popup:

- `app.modal == ["join"]`, `app.context == "game"`, and the engine's menu is up
  (key_dest menu) although the player did not open it.
- node `join`: type `screen`, `presentation: "dialog"`, `dismissable: true`,
  `title: "Join game"`, `primary: {id: "join.auto", label: "Join"}` (label
  "Auto-select team" and a `subtitle` in a team game);
  optional child `join/teams`: a `grid` of buttons with actions `join.red`,
  `join.blue`, `join.yellow`, `join.pink` (only the teams the server offers);
  child `join/spectate`: a button with action `join.spectate`.

Kotlin has to:

1. show its surface when `app.modal` becomes non-empty in context `game`,
   with no user gesture, and draw the dialog over the running match, not on the
   pause page;
2. send the action event with the action's id on a tap, and `back` or
   `dismiss` on Back or a tap outside (the menu side then closes the popup and
   hides the menu; it never falls through to the pause screen);
3. when `app.modal` empties and the menu hides (also by itself, once the player
   is in the game), remove its overlay and give touches back to the game.

`ViewRenderer.kt` already handles `presentation` dialog and sheet, so point 1
is the one to check on the tablet. If the dialog does not show there, the
player is left with the menu up and nothing drawn: `touch_join_dialog_auto 0`
stops the auto-open until it is fixed (MENU > Join game still opens it).

**Pause screen.** The description now has `join` ("Join game", action
`join.open`) while `_touch_act_join`, `team` ("Change team", `join.open`) while
`_touch_act_team`, and `spectate` only while `_touch_act_spectate` (with a
confirm when it is 3).

**Settings.** Settings > Controls has a new first sub-tab `feel` ("Touch
controls"): sections `look`, `stick`, `buttons`, `layout`, rows bound to cvars
(sliders, switches, segmented choices), and two actions: `touch.edit` and
`touch.reset` (the latter carries a confirm). Nothing renderer-specific.

**Haptics.** Set `TouchLua_VibrateHook` (and add the VIBRATE permission); the
engine then sets `_touch_haptics_available` and the settings row appears.

## What the screens description (work/screens2) must know

- Hooks I added in its files, one line each:
  `touch_uib_screens.qc`: `case "join": UIB_BuildJoin()` in `UIB_Build`,
  `UIB_JoinAction(a)` in `UIB_Action`, `UIB_JoinBack()` at the top of
  `UIB_Back`, `UIB_JoinTick()` in `UIB_Tick`, and the pause screen's Join game
  / Change team / Spectate from `_touch_act_*`.
  `touch_uib_settings.qc`: `UIB_SecTouchControls()` as the first sub-tab of
  `UIB_SecTouch`, `UIB_TouchControlsAction(a)` in `UIB_SettingsAction`.
  Declarations: `touch_uib_touch.qh`. My files: `touch_uib_join.qc`,
  `touch_uib_touchcontrols.qc`.
- **`_menu_alpha` fix** (2caee35, one line in `touch_uib_render.qc`,
  `TouchUIB_Draw`): with `menu_touch_uib 1`, after any stock dialog over the
  bridge menu closed (game menu > Resume, the join dialog), `_menu_alpha`
  stayed 1, because `m_draw` publishes it only when it draws the stock menu
  and the bridge renderer has the frames of the fade-out. The game's HUD fades
  by that cvar, so the clock, score list, scoreboard, intermission board and
  map vote never came back. Reproduce on main: in a match,
  `menu_showgamemenudialog`, then `togglemenu 0`, then `echo $_menu_alpha`
  (1 before the fix, 0 after). The fix keeps the cvar in step with the alpha
  the renderer is given. Please keep it, or move it somewhere you prefer.
- The join dialog drawn by the QuakeC bridge renderer (popup on the
  full-screen pause page, small) looked wrong, so without a native renderer
  the dialog is the stock-style `dialog_touch_join.qc`, opened through `m_goto`
  as the game menu is. If the QuakeC renderer gets a real over-the-match
  dialog, `UIB_JoinOpen` in `touch_uib_join.qc` is the one line to change.
- `TouchUIB_Classic` takes any non-empty `m_goto` name for a stock dialog,
  including a name that is no dialog: the old launcher then shows for a second.

## Open question for the owner

**Should a LAN host get the join dialog?** As it is, any game hosted on the
device with no team to pick joins by itself (`touch_local_autojoin 1`): a
Singleplayer match, a `map` from the console, and a LAN game the player hosts.
The previous coordinator expected LAN hosts to get the dialog (so a host can
choose to watch). To do that, make the engine skip the straight-in join while
`touch_lan_hosting` is set, or default `touch_local_autojoin` to 0 and rely on
screens2's `sv_spectate 0` for non-LAN local games. Team games hosted here
already get the dialog.

Also decided by me against the letter of the brief, say if it should change:
after MENU > Spectate while playing, the join dialog is **not** reopened (it
was the player's own choice; MENU > Join game brings it back).

## On-device checklist

The full list, with what to feel for and the cvar to change live for each
step, is at the end of `docs/TOUCH_CONTROLS_REVIEW.md`. In short:

1. Public DM server: the Join game dialog comes up by itself; Join plays,
   Spectate stays and it does not return; MENU > Join game reopens it.
2. CTF: auto-select, the two teams, Spectate; never FOLLOW while alive.
3. Local DM: straight into the game, no dialog.
4. Top bar in play, dead, spectating, scoreboard: clear gaps, score list under
   it, clock clear. If not: screenshot and `touch_hud_state rects`.
5. Stick: appears under the thumb, small wobble zone, full speed about halfway
   out, reverses at once after dragging far (`touch_move_size`,
   `touch_stick_deadzone`, `touch_stick_run`, `touch_stick_float 0`).
6. Look: a slow 2 cm drag is about a fifth of a circle (`touch_sens_base`,
   3.6; the old feel was 2.8; `touch_sens_y_mult`).
7. Flick: a quick 3 cm swipe turns you around (`touch_look_accel` 0.8, 0 off,
   1.4 strong; `touch_look_accel_lo` 150, `_hi` 900).
8. Smoothness: no shimmer at rest, no lag on a slow drag (`touch_look_fcmin`,
   `touch_look_beta`, `touch_look_filter 0`).
9. Hold FIRE and slide: the view follows while firing. Two fingers add.
10. Hold HOP and run: keeps jumping; slide the same thumb: turns while hopping
    (`touch_button_drag_look 0` if the view jumps on the press).
11. DUCK and ALT: the same slide.
12. Sizes: FIRE, HOP, DUCK, KEEP, TURN hit without looking (`touch_scale`).
13. Settings > Mobile > Touch and Settings > Controls > Touch controls: every
    row does what it says; Reset to defaults.
14. Update over an install with a saved layout: positions kept, stick larger,
    look faster, `touch.layout.bak` written.

## Tools left in `~/.cache/godnotic-work/`

`controls-tools/` (`run.sh`, `srv.sh`, `after.sh`, `sheet.py`, tour files in
`s/`, the native probe in `native/`), `controls-base/` (Godnotic basedir linked
to this clone), `controls-stock/` (upstream data only), `controls-before/` (old
main's engine and data for "before" shots), `controls-shots/`,
`controls-live-c1/`, `controls-live-c2/` (logs and screenshots of the live
runs). A local branch `wip/all` in the clone holds the pre-split snapshot and
can be deleted.
