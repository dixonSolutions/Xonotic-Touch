# Missing game functions — health, armour, ammo, chat (2026-08-08, session 2)

Target: Surface Pro 9, Ultramarine Linux, touch only, via `gdr --dev=marinesurface`

The player had no health readout, no armour readout, no ammo readout, and no way
to send a chat message. This records why, because in each case the code that was
supposed to provide it existed and looked correct.

## 1. Health, armour and ammo were disabled twice over

Two independent switches both had to be wrong, and both were:

- `touch/xonotic.cfg` sets `hud_panel_healtharmor 0` and `hud_panel_ammo 0`,
  turning off the stock readouts on the grounds that the touch layer replaces
  them.
- `touch_mobile_hud` defaults to **0**, so the replacement never draws.

`Touch_ApplyMobileHudEngineSettings()` only disables the stock panels *when the
mobile HUD is on*, which is the correct guard — but with the stock panels already
disabled in the shipped config, the guard had nothing to protect. Net effect: a
player could not see their own health.

Fixed by defaulting `touch_mobile_hud` to 1 and, more importantly, by making the
two settings each other's inverse in one place instead of two files that have to
agree. The touch HUD now falls back to the stock panels if it is switched off.

## 2. The ammo panel was positioned in the wrong coordinate space

`Touch_Hud_AmmoPanelOrigin()` used `getpropertyvec(VF_SIZE)` to find the right
edge. Every other part of the overlay uses `Touch_UISize()` (console space), and
`touch_layout.qc` opens with a comment warning about exactly this:

> VF_SIZE can be the Wayland surface (e.g. 1440×960) while 2D is clipped to
> 960×640 — laying out in VF_SIZE pushed FIRE/JUMP off the right edge.

On this device VF_SIZE is 1920×1280 and console space is 810×540, so the panel
was placed at x ≈ 1700 in an 810-wide space: off screen. It would have been
invisible even with the mobile HUD switched on. This is the third instance of the
VF_SIZE/console-space confusion in this codebase; the other two were the edit
mode hit test and the original widget layout.

## 3. Readouts were stealing aim area

`Touch_Hud_HitPanel()` was consulted in `Touch_AssignRole()` and returned
`TOUCH_ROLE_IGNORED` for touches inside either panel. Since unclaimed screen area
falls through to LOOK, that turned the HUD's footprint into a dead patch where
dragging would not turn the view. A readout is not a control and must not consume
input; the hit test is gone.

## 4. Every label in the overlay rendered at the same size

`TOUCH_FONT_RATIO` was 0.20 of the widget radius, against a `TOUCH_FONT_MIN` of
9 units. Working through the actual controls:

| Control | Radius / half-height | radius × 0.20 | Rendered |
|---|---|---|---|
| FIRE | 37.8 | 7.6 | 9 (clamped) |
| HOP | 19.4 | 3.9 | 9 (clamped) |
| DUCK | 17.8 | 3.6 | 9 (clamped) |
| CONSOLE | 10.8 | 2.2 | 9 (clamped) |

Every label hit the floor, so the clamp was doing all the work and a 26 mm button
drew exactly the same type as a 7 mm pill. There was no typographic hierarchy at
all, and 9 units is 3.05 mm — below comfortable reading size at arm's length.

Labels are now fitted to the control's box: bounded by half-height so type never
crowds the rim, and by width so a four-letter word in a small disc shrinks rather
than spilling past the edge. Which constraint binds depends on the shape, which
is why a single ratio could not work for both a thin bar and a wide disc.

## 5. The chrome pills were below the minimum touch target

MENU and CONSOLE were 7.3 mm tall (`size 0.040` × 183.2 mm short edge). The
Material floor is 9 mm and this project's own token file sets
`TOUCH_MM_COMFORT` at 12. They sit at the top edge, which is the furthest point
from a thumb on a tablet held in two hands, so Fitts's law argues they should be
*larger* than average rather than the smallest things on screen.

## 6. Chat could be read but not written, and barely read

The feed is drawn by the engine, not by QuakeC — the stock `HUD_Chat` panel only
sets `con_chatrect`, `con_chatrect_x/y`, `con_chatwidth` and `con_chat`, and the
engine renders the lines at `con_chatsize`. That is good news: the feed is fixed
with cvars rather than a reimplementation.

At the shipped `con_chatsize` of 8 units the text was 2.7 mm tall, and it was
positioned bottom-left directly underneath the move stick — so the player's own
thumb covered the messages. There was no composer of any kind, and
`touch_console.qc` is only the button that toggles the engine console; the
keyboard described in `TOUCH_CONSOLE_SPEC.md` was never built.
