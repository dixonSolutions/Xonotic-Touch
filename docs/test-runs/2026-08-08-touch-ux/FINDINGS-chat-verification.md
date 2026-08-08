# Verifying the chat composer on the device (2026-08-08, session 3)

Target: Surface Pro 9, Ultramarine Linux, touch only, via `gdr --dev=marinesurface`

Session 2 built the chat sheet and the on-screen keyboard but could only verify
them by screenshot, because no tap had ever been delivered to the game. This
session got taps working, and every one of the four defects below was found by
actually pressing the keys. None of them were visible in a screenshot.

## 0. How to drive the game by touch (the thing that unblocked everything)

Three coordinate spaces are in play on this device and they are all different:

| Space | Size | What lives here |
|---|---|---|
| Console (2D) | 810 × 540 | all overlay drawing and hit-testing; `Touch_UISize()` |
| Video / engine screenshot | 1440 × 960 | `screenshot` output, `VF_SIZE` |
| Physical / compositor | 2880 × 1920 | `gdr click`, `gdr screenshot` |

`gdr click` takes **physical** pixels. So a control identified in an engine
screenshot needs its coordinates doubled before clicking. Every earlier attempt
to tap a key had used screenshot coordinates and silently missed, which is what
made the input path look dead. Read the console size with `F8` (see below) rather
than assuming it: `vid_conwidthauto 1` derives it from the surface each frame.

`scripts/dev-deploy.sh` now also writes `xt-probe.cfg`, bound to **F8**, which
dumps `vid_con*`, `con_chat*`, `hud_panel_chat*`, `hud_panel_weapons*`,
`touch_hop*`, `touch_mobile_hud` and `touch_layout_version` to
`/tmp/xonotic-dev.log`. `cvarlist <prefix>` is the only way to read a value back
out of a running engine over SSH — `echo` does not expand cvars — and several
rounds of layout debugging had been spent guessing at values that F8 now prints.

One harness limit remains: a synthetic click's press and release can land in the
same engine frame, in which case the finger is never observed and the tap is
lost. Roughly one tap in three vanishes. Real fingers hold for 50–100 ms, so this
is a property of the injector, not of the overlay. Space taps ≥ 1 s apart.

## 1. `Touch_ReleaseAll()` was erasing the pointer the sheet ran on

`Touch_Chat_Frame()` calls `Touch_ReleaseAll()` every frame while the sheet is
open, so that no movement or fire key can creep through a modal. But
`Touch_ReleaseAll()` clears `touch_mouse_down` — and `Touch_Pointer_Frame()` used
`Touch_MouseDown()` as its only non-finger source. The modal was therefore
wiping, every frame, the one flag its own input depended on.

It survived review because it reads as two correct functions: releasing
everything on a modal is right, and falling back to the mouse is right.

`Touch_Pointer_Frame()` now reads engine slot `TOUCH_SRC_POINTER` (10, the SDL
mouse) through the same `Touch_SampleEngineFinger()` the gameplay controls use,
and keeps `getmousepos()` only as a last resort. That also fixed a second gap: the
pointer scanned slots 0–9 only, so the mouse's own slot was never checked.

## 2. Every keypress wiped the message buffer

The field held only the last character typed. Typing `h`, `e`, `y` gave `y`.

```c
strcpy(touch_chat_text, Touch_Kb_Apply(touch_chat_text, row, cell));
```

`strcpy` is a macro:

```c
#define strcpy(this, s) MACRO_BEGIN if (this) strunzone(this); this = strzone(s); MACRO_END
```

The target is unzoned *before* the argument is evaluated, so `Touch_Kb_Apply`
read a freed string, saw it as empty, and returned `"" + c`. `strcpy(x, f(x))` is
a use-after-free for any `f`. Fixed by building the new string into a local first.

Worth grepping for as a class: the pattern is invisible at the call site and the
symptom (data quietly truncating) does not look like a memory bug.

## 3. The hop latch survived a mode change

`touch_hop_latched` is only cleared by a deliberate tap in latch mode, but
`touch_hop_mode 2` (auto-hop) *sets* it from stick displacement. Switching
profiles from auto-hop to latch-hop therefore left the latch stuck on: jump held
down permanently, with the HOP bar drawn in its accent-filled "on" state and no
way to clear it but another profile change. Captured in `41-ingame.jpg`, where
HOP is lit with nothing touching it.

The mode is now tracked, and any change to it clears the latch. This is the same
bug class as the config drift from session 2 — state that outlives the setting
that produced it.

## 4. Chrome pills were unreadable against a bright wall

`ChromePill` had been deliberately flattened to a single light stroke and a glass
fill, on the reasoning that chrome should recede. Over a sunlit white pillar
(`43-clicklogical.jpg`) that put white-on-light-grey at roughly 3:1 and erased the
pill's outline entirely, so MENU read as floating text.

Chrome recedes by being small and flat, not by being faint. The pills now draw a
near-solid plate (`TOUCH_A_CHROME_FILL 0.88`, past 4.5:1 on any background) and
the same dual-stroke rim as every other control. Three pills at the screen edge
occlude nothing worth seeing.

Related: the chat sheet at `TOUCH_A_SHEET 0.94` let the dead player's scoreboard
ghost through as legible white text. Raised to 0.98 — a modal sheet is a surface,
like a GNOME dialog, not a tint.

## Verified end to end

- CHAT pill opens the sheet; Escape and CLOSE dismiss it.
- Keys type into the field; the caret tracks the insertion point (`50-hey.jpg`).
- SEND delivers to the server: `TouchAgent: "hey"` appears in the engine feed
  (`51-sent.jpg`).
- `CSQC_Parse_Print` capture works: reopening the sheet shows the sent line in
  the backlog with its colour codes intact (`52-history.jpg`).
- Health, armour and ammo readouts draw live and track values (`54-alive.jpg`:
  100 health green, 5 armour red, 28 shells).
- HOP rests as a ghost capsule matching DUCK, and no longer as a lit switch.

## 5. The weapons strip was the loudest thing on screen

The selected weapon drew the skin's `weapon_current_bg`, an opaque chamfered
plate, beside a HUD that is otherwise dark glass with hairline strokes — so the
brightest, hardest-edged object in the frame was the one nobody needs to study
(`41-ingame.jpg`, right edge). Cvars cannot reach it: the brightness is in the
asset, and `hud_panel_fg_alpha` dims the weapon icons along with the plate.

Rather than ship an override asset under a skin name we do not control, the strip
is now drawn by the overlay: rounded slots, glass for owned, accent tint plus a
rim for held, impulse number as a corner badge (`57-weps.jpg`). It costs about
what the stock panel cost — both draw a background, an icon and a label per
weapon — and it claims no input, since the WEP button and the wheel switch guns.

First attempt filled the held slot at latch opacity and reproduced the original
complaint in our own palette. Hue and an edge are enough to mark one slot in a
column of five; weight is not needed and is what made it shout.

## 6. Every frag printed across the controls

`hud_panel_notify` ships bottom-right, which on this layout is FIRE, HOP and
DUCK. Each kill wrote a line of text over the buttons (`57-weps.jpg`, bottom
right). It now sits in the right band under the chrome pills, above the weapons
strip: the placement other shooters use, and clear of every control.

That makes three engine-drawn feeds that had to be told where to go — kill
notifications, chat, and the match clock. A touch layout puts controls where a
desktop HUD puts text, so any panel whose position was never questioned is a
candidate. Checking them is worth doing before adding anything new to a corner.
