# Touch UX Redesign — 2026-08

Redesign of the in-game touch controls for tablets, replacing the square
`drawpic(white)` slabs with real shapes, a single design-token layer, and an
input model that lets a player move, aim, shoot and bunny-hop at the same time.

Supersedes the visual and feel sections of [TOUCH_LAYOUT_SPEC.md](TOUCH_LAYOUT_SPEC.md).
The coordinate contract in that document (centre semantics, console draw space,
`x' = 1-x` mirroring) is unchanged and still authoritative.

## 1. What was wrong

Captured on the Surface Pro 9 / Ultramarine target
([shots/00-baseline-ingame.png](test-runs/2026-08-08-touch-ux/shots/00-baseline-ingame.png)):

| Problem | Cause |
|---|---|
| Every control is a **square**, including the "joystick" and the round buttons | `Touch_Glass_*` built all shapes by stretching `gfx/colors/white`, so a circle was a rectangle |
| Controls read as pale **slabs** that wash out over bright scenery | Body tint `0.78 0.86 0.96` at alpha 0.72 — a light fill over a light scene |
| Joystick reads as a box-in-a-box-in-a-box | Base plate + inset rim + square knob, all axis-aligned rectangles |
| Labels are grey-on-grey | White text at 0.9 alpha with no stroke, over a light body |
| Controls look nothing like the game's own HUD | Stock Xonotic HUD panels are dark; the touch layer was light |
| Bottom row does not line up | FIRE centre `y=0.640`, MOVE `0.680`, CROUCH/JUMP `0.860` |
| Cannot move + aim + shoot + jump together | Four inputs, two thumbs, and JUMP is a discrete button |

The generated shape masks in `touch/gfx/touch/` (`disc`, `ring`, `rrect`,
`shadow`, …) already existed but **nothing referenced them** — the investment in
`scripts/gen-touch-shapes.py` was never wired into the draw path.

## 2. Device geometry (measured, not assumed)

Screen detection runs inside the Flatpak sandbox where `gdctl` and `fb0` are
unreachable, so `screen-calc.sh` falls back to a 1920×1080 guess and emits
`vid_conheight 540` with `vid_conwidthauto 1`. The engine then derives console
width from the real 3:2 surface:

| Quantity | Value |
|---|---|
| Physical panel | 2880 × 1920 px, 3:2, 13.0" → **274.8 × 183.2 mm** |
| Console draw space (`Touch_UISize`) | **810 × 540** |
| `Touch_ScreenMin()` | 540 |
| **mm per console unit** | 183.2 / 540 = **0.339 mm** |

Because widget `_size` is a fraction of the short axis, the conversion is
exactly `mm = nsize × 183.2`, i.e. `nsize = mm / 183.2`. This is the number that
makes every size decision below checkable rather than guessed.

`Touch_MMToUnits()` derives this at runtime from `vid_touchscreen_xdpi` when it
is trustworthy and falls back to this measured constant otherwise, so the layout
does not silently mis-scale on a different panel.

## 3. Sizing targets

From the research (full citations in §8):

| Source | Minimum touch target |
|---|---|
| Apple HIG | 44 × 44 pt |
| Material / Android | 48 dp ≈ **9 mm**, 8 dp spacing |
| Microsoft | 9 mm target, 13.5 mm recommended |
| NN/g (tablets) | ≥ 10 mm |
| Game Accessibility Guidelines | **24 mm ideal**, 9.6 mm phone compromise |
| ACM study, ≥90% accuracy | 9 mm primary attack, 8 mm skills; shipped games use 11–15 mm primary |

The ACM thumb-arc study also gives the reachable band on a held device:
**27.5–41.3 mm** from the thumb pivot is the primary comfort zone, 13.8–27.5 mm
secondary, and 41.3–55 mm reachable but occluding.

Adopted tokens:

| Token | mm | `nsize` | Use |
|---|---|---|---|
| `TOUCH_MM_MIN` | 9 | 0.049 | absolute floor, never ship below |
| `TOUCH_MM_COMFORT` | 12 | 0.066 | cold buttons |
| `TOUCH_MM_HOT` | 22 | 0.120 | FIRE, JUMP |
| `TOUCH_MM_STICK` | 42 | 0.229 | move stick base (spans the thumb arc) |

**Paint is decoupled from hit area.** The old code drew a filled slab the full
size of the hit rect, which is what made it chunky. Now the hit rect stays
generous (and grows via grab multipliers) while the *painted* body is a thin ring
plus a low-alpha fill. Research F58: expand the hit rect, not the paint rect.

## 4. Design tokens

Centralised in `touch_theme.qh`. Colours are libadwaita's, converted to QC
0..1 vectors, so the overlay matches GNOME rather than inventing a palette.

| Token | Value | Origin |
|---|---|---|
| `TOUCH_C_SURFACE` | `0.07 0.08 0.10` | dark scrim, near libadwaita `--view-bg-color #1d1d20` |
| `TOUCH_C_RIM` | `1 1 1` | hairline border |
| `TOUCH_C_ACCENT` | `0.208 0.518 0.894` | libadwaita accent blue `#3584e4` |
| `TOUCH_C_ACCENT_LIGHT` | `0.506 0.816 1.0` | libadwaita dark standalone accent `#81d0ff` |
| `TOUCH_C_DANGER` | `0.902 0.176 0.259` | libadwaita red `#e62d42` |
| `TOUCH_C_SUCCESS` | `0.227 0.580 0.290` | libadwaita green `#3a944a` |
| `TOUCH_C_INK` / `TOUCH_C_INK_SHADOW` | `1 1 1` / `0 0 0` | label + its stroke |

Alpha scale, all multiplied by `touch_opacity`:

| Token | Value | Justification |
|---|---|---|
| `TOUCH_A_SURFACE` | 0.34 | 20–40% backing-panel band for legibility over arbitrary scenes |
| `TOUCH_A_SURFACE_ACTIVE` | 0.58 | pressed state must be unambiguous |
| `TOUCH_A_RIM` | 0.44 | the shape is carried by the rim, not the fill |
| `TOUCH_A_RIM_ACTIVE` | 0.95 | |
| `TOUCH_A_SHADOW` | 0.36 | ≤50%, blur kept small |
| `TOUCH_A_INK` | 0.95 | |
| `TOUCH_A_INK_SHADOW` | 0.75 | the 1–2 px dark stroke that does the real work |
| `TOUCH_A_GHOST` | 0.16 | deadzone rings, hint geometry |

**Why dark-on-light instead of the old light-on-dark:** a light body over a
bright sky has no contrast, and Xonotic maps swing from white skyboxes to black
corridors. A dark low-alpha fill plus a bright rim plus a bright label with a
dark stroke is legible at both ends, which is the cheap approximation of Apple's
adaptive Liquid Glass polarity flip (research F39, F46).

Radii follow libadwaita's `6 / 9 / 12 / 15 px` scale and its 6 px spacing
increment, scaled by `Touch_UIScale()` so they hold at any console size.

## 5. Shape and widget layers

Two layers, so no widget invents its own look:

**`touch_shape.qc` — primitives.** Draws the pre-baked antialiased masks instead
of stacking rectangles:

| Helper | Texture | Notes |
|---|---|---|
| `Touch_Shape_Disc` | `gfx/touch/disc` | one `drawpic`, an actual circle |
| `Touch_Shape_Glow` | `gfx/touch/disc_soft` | radial falloff, pressed state |
| `Touch_Shape_Ring` | `ring_thin` / `ring` / `ring_thick` | stroke is a fraction of radius, so weight stays proportional |
| `Touch_Shape_Shadow` | `gfx/touch/shadow` | soft elevation blob |
| `Touch_Shape_Capsule`, `Touch_Shape_CapsuleRing` | `disc` / `ring` halves via `drawsubpic` + a middle rect | pills and the hop bar |
| `Touch_Shape_RRect` | `rrect` 9-sliced via `drawsubpic` | corner radius stays constant under stretch |
| `Touch_Shape_Label` | — | text plus a 1 px dark stroke in 4 directions |

Ring helpers snap the requested weight to the nearest baked mask and report the
snapped value, because a capsule's straight edges are drawn as plain rects and
have to match the thickness of its caps exactly or the joint shows a step.

**`touch_widget.qc` — composed controls.** `Touch_Widget_Button`,
`Touch_Widget_Capsule` and `Touch_Widget_ChromePill` are the only things the
draw code calls. Each is shadow, translucent surface, rim, label. Every visible
control — gameplay buttons, the console and pause pills, the HUD panels, the
edit-mode handles and its toolbar — goes through one of them, so consistency is
structural rather than a convention someone has to remember.

### 5.1 Every edge is two strokes

A white rim disappears against a white skybox; a dark rim disappears in a black
corridor. Xonotic ships both in the same map. So `Touch_Widget_Rim` draws a dark
contour and then the bright rim inset by `TOUCH_W_CONTOUR_INSET` — the same
trick the label already used for text, applied to geometry. This is what makes
the controls legible in [02-redesign-bright.jpg](test-runs/2026-08-08-touch-ux/shots/02-redesign-bright.jpg)
and [01-redesign-ingame.jpg](test-runs/2026-08-08-touch-ux/shots/01-redesign-ingame.jpg)
without changing opacity between them.

Chrome pills deliberately skip both the shadow and the contour. That is the
cheaper path *and* the correct one: chrome should recede.

### 5.1.1 Text is measured, not estimated

`Touch_Shape_LabelWidth` asks the engine for the string's width
(`stringwidth_builtin`) instead of multiplying character count by a nominal
advance. The estimate is fine for a label centred in a button and wrong everywhere
alignment matters: with a proportional font, "100" and "0" right-aligned to the
same edge landed on columns a few pixels apart, which is visible the moment three
readouts are stacked in one group. Fitting a label to a box uses the same measure
at unit size, since text width scales linearly with font size — so a wide label and
a long one are no longer assumed to be the same thing. The old constant survives
only as a fallback for frames before the font is resident, where the builtin
reports 0.

### 5.2 Measured cost

**78 draw calls** for the controls alone, from the `fills` field of the
`touch_debug` readout, at a sustained 30 fps cap with headroom (uncapped the same
scene runs >110 fps). Roughly: 25 for the five labels (each is 4 stroke passes
plus the glyphs), 22 for the hop bar and two chrome pills, 8 for the move stick,
8 for the two round buttons.

With the readouts added — vitals, ammo and the weapons list — a live frame reads
around 170 with the debug overlay itself on top. Two figures are worth keeping in
mind when adding to this layer: a rounded rect is **nine** draws (four corners,
four edges, one middle), and a label is four stroke passes plus glyphs. That is
what makes "one region with a highlighted row" cheaper than "a plate per row" by
an order of magnitude, and it is the arithmetic to do *before* drawing a shape per
item.

This is over the 34-fill ceiling in TOUCH_LAYOUT_SPEC §10, and that ceiling does
not transfer: it was measured for `drawfill` rectangle stacks, whereas these are
tinted textured quads sharing one texture per shape. The number that mattered on
this device was the frame cap, not the overlay — see §10.

**Fallback is mandatory.** If the masks are missing (partial package, stale
overlay) `drawpic` would render notexture checkerboards, which is worse than the
squares. `Touch_Shape_Ready()` probes `drawgetimagesize("gfx/touch/disc")` once
and every widget reverts to the legacy rectangle glass when it is absent. The
`touch_debug` readout ends with `shape tga` or `shape flat` so which path is live
is never a guess.

## 6. Input model — four inputs, two thumbs

This is the substantive part. No shipped mobile shooter solves four concurrent
inputs with two thumbs; they all **add fingers**, **merge two inputs into one
control**, or **automate one input**. We do the latter two.

### 6.1 Hop latch — jump stops being a finger

Xonotic's own in-game guide is explicit that bunny-hopping is a *held* key:
players "constantly hop by simply keeping [jump] pressed", and during a strafe
turn "as always … keep [jump] held". Jump is therefore not a fourth discrete
input competing for a thumb — it is a near-permanently-held modifier, and a held
state needs a latch, not a finger.

`touch_hop_mode`:

| Value | Behaviour |
|---|---|
| 0 | Legacy hold — `+jump` only while the bar is touched |
| 1 | **Latch (default)** — tap jumps once; hold past `touch_hop_latch_ms` latches continuous `+jump`; tap again to release |
| 2 | Auto — `+jump` is held whenever the move stick is deflected past its deadzone |

Mode 1 is a button first and a latch second. It jumps on press like any button —
a single jump clears a step or a gap and is the more common of the two actions —
and only latches if the finger stays down. A pure toggle would be cheaper and was
what shipped first, but it took the single jump away entirely.

In latch and auto modes the HOP bar fills with the accent colour and shows an "on"
pip, so the armed state is never ambiguous; at rest it is a ghost capsule matching
DUCK. The latch is dropped on death, level change, `Touch_ReleaseAll`, **and any
change to `touch_hop_mode`** — auto-hop sets the latch from stick displacement, so
switching from auto to latch used to leave jump held down forever with no finger
on the bar.

The precedent is `cl_autojump`, which Quake-family mobile ports ship defaulted
**on** for exactly this reason. Terraria's mobile jump options (double-tap-up /
single-tap-up / swipe-up / off) are the reason this is a mode rather than a
single hardcoded behaviour.

### 6.2 Fire is also a look surface

Call of Duty Mobile's "R-Fire BTN for Camera Rotation" makes the fire button
itself an aim surface: pressing fires, and dragging *from* it keeps rotating the
camera. Every competitive settings guide turns it on, because it is the
mechanism that makes two-thumb play viable at all — shoot and look become one
finger instead of two.

`touch_fire_drag_look` (default 1): a finger that lands on FIRE holds `+attack`
and, once it moves past `touch_look_deadzone_px`, also drives look for as long
as it is down. Fire is not released when the drag starts.

With this plus the hop latch, the steady state is: left thumb on the stick,
right thumb on FIRE aiming and shooting, jump latched. All four at once, two
thumbs, nothing automated that the player did not ask for.

### 6.3 Tap-to-fire in the look zone

`touch_look_tap_fire` (default 1): a tap in the look zone that never escapes the
deadzone and lifts inside `touch_look_tap_ms` fires one shot. This is the
right-thumb fallback when the thumb is away from the FIRE disc.

We deliberately do **not** implement "shoot at the point you touched": fingers
are ~40 px wide and completely occlude what they aim at.

### 6.4 Second fire button, off by default

`touch_fire2_visible` adds a left-side fire button, mirroring PUBG Mobile's
always-on left fire button. It costs pixels, not fingers, and is the enabling
affordance for claw-grip players. Off by default to respect Hick's Law.

### 6.5 Shoulder buttons, inset from the edge

On a 13" tablet the index fingers rest near the top corners, so top-corner
buttons are the natural home for a fourth and fifth input. **GNOME reserves
drags from the top and bottom screen edges**, so these sit inset rather than
flush — which also matches Microsoft's warning that flush side placement causes
accidental activation.

### 6.6 Look filter: 1€ instead of a fixed lag

The old path ran a fixed exponential smoother (τ = 0.035 s or 0.070 s), which is
a constant low-pass: it costs latency at every speed. Aim smoothing that blends
past positions delays the crosshair by 1–3 frames, and at this device's frame
budget one frame is already 33 ms — far above the 5–10 ms perceptual floor for
*dragging*, which is the most latency-sensitive input class there is.

`touch_look_filter 1` (default) uses the **1€ filter**, whose cutoff adapts to
speed: low cutoff while the thumb moves slowly (kills jitter, where nobody
notices lag) and high cutoff while it moves fast (kills lag, where nobody
notices jitter). Two parameters with physical meaning, `touch_look_fcmin`
(slow-speed jitter) and `touch_look_beta` (high-speed lag). Valve shipped exactly
this for Steam Deck gyro in 2023. `touch_look_filter 0` disables filtering,
`2` restores the legacy fixed EMA.

## 7. Layout

MOVE and FIRE share one centre line (Law of Similarity / Continuity), the centre
third stays empty — both because the crosshair lives there and because the
middle of a two-hand-held tablet is physically unreachable — and secondary
buttons stay hidden by default.

| Widget | `x` | `y` | `size` | mm | Notes |
|---|---|---|---|---|---|
| MOVE | 0.170 | 0.680 | 0.229 | 42 ⌀ | ring only, knob 34% |
| FIRE | 0.855 | 0.680 | 0.140 | 26 ⌀ | hot, doubles as look surface |
| HOP | 0.855 | 0.885 | 0.072 | 13 × 34 | capsule, latch pip in the right cap |
| DUCK | 0.700 | 0.885 | 0.066 | 12 ⌀ | shares the HOP baseline |
| ALT fire | 0.135 | 0.240 | 0.120 | 22 ⌀ | hidden unless `touch_fire2_visible` |
| CONSOLE | 0.500 | 0.045 | 0.040 | 7 × 31 | pill, dimmed at rest |
| MENU | 0.320 | 0.045 | 0.040 | 7 × 16 | pill, dimmed at rest |

The FIRE radius measures **12.8 mm** on device (`mm/r` in the `touch_debug`
readout), i.e. 25.6 mm across, which is the 26 mm target — so the millimetre
conversion in `Touch_MMToUnits` is doing what §2 claims and not silently
falling back to a wrong panel size.

The first draft of this table kept the old 0.04 gap between the MOVE and FIRE
centres, which is precisely the misalignment §1 complains about. They are now
equal.

CONSOLE and MENU are developer/system affordances, not gameplay controls, so
they rest at 55% of the overlay alpha and only reach full opacity while touched.
They previously drew at `max(0.75, alpha)` — brighter than the gameplay
controls, which inverted their importance. They are hidden entirely in edit
mode, where the top bar belongs to SAVE / CANCEL / RESET.

Labels were renamed to fit the button: `CR` → `DUCK`, `R` → `RLD`,
`DODGE` → `DASH`, `JUMP` → `HOP` (it latches; it is not a jump key any more).
`Touch_Customize_WidgetLabel` carries the same strings, so edit mode never names
a control differently from the control.

### 7.1 Edit mode

Reworked alongside the controls, because it was drawing its own thing:

- Hit-testing ran in `VF_SIZE` while everything else uses console space, so on
  this device (1920×1280 surface, 810×540 console) a dragged handle jumped away
  from the finger. It now uses `Touch_UISize()` and re-applies the handedness
  mirror, so a dragged control lands where it was dropped.
- The toolbar's geometry had two independent definitions, one for drawing and one
  for hit-testing, and they disagreed. There is now one.
- Actions fire on *release*, and sliding off a button cancels it. RESET discards
  the whole layout; mistapping it on press was unrecoverable.
- RESET carries its danger colour at rest rather than only while held (Von
  Restorff) — by the time it is held the decision is already made.
- The hop bar's handle is a capsule ring and its hit rect is the bar's rect. A
  circular handle claimed the wrong bounds and left the ends ungrabbable.
- `TOUCH_WIDGET_NONE` is -1 but QC globals start at 0, which is a valid widget
  id *and* a valid toolbar action, so `Touch_Customize_Reset()` now establishes
  "nothing selected" before the first frame.

### 7.2 Mobile HUD

The health/armour and ammo panels were square `drawfill` slabs with a different
palette from the controls. They are now rounded-rect surfaces from the same
tokens, with capsule stat bars whose track stays visible at ghost alpha so the
bar is readable by *length* even when the value is low.

Health, armour and ammo are **one group of three rows** in the top-left corner,
sharing an icon column and a right-aligned number column. Anchoring each number
at the right of a fixed slot rather than centring it means 100 → 99 → 9 does not
walk the group sideways under the eye. All three are readouts and none claims
input, so none can put a dead patch in the aim area.

Ammo was originally given its own anchor beside FIRE, on the theory that the count
belongs to the trigger. On device that put it in the one strip of screen the right
thumb crosses on its way between FIRE and HOP: the number was under the thumb
exactly when it was being spent, and having no backing it also sat over whatever
the level happened to put there. It is the same kind of number as the other two,
so it now reads as one group with them (Law of Similarity) in a corner the eye
already visits. `touch_ammo_x` / `touch_ammo_y` are retired
(`TOUCH_LAYOUT_VERSION 4`).

Only the rows that have a genuine maximum get a bar. Reserve ammo has none, so
its row leaves the bar column empty rather than inventing a full-scale value; for
weapons that reload, that column carries the reserve at label weight next to the
clip count, because what is in the gun is the number you act on.

The group draws **no plate**. One was tried at surface alpha and measured: it took
a dark background from 57 to 30 and a sunlit wall from 255 to about 175, so it
neither disappeared nor guaranteed contrast — it only smudged a corner of the scene
for nine draw calls. Legibility here comes from the labels' own 1 px outline, which
is what already lets the numbers read over bright terrain. The weapons strip keeps
its plate because a plate is what makes nine loose icons read as one inventory,
where three aligned rows with bars already read as one readout.

Both left-band neighbours had to move for it. Three rows are 0.19 of the screen
height, so at the old two-row anchor the health row hung off the top edge, and the
chat feed's band started at 0.15 — the first chat line ran through the ammo row.

The weapons strip is also ours now. The stock `hud_panel_weapons` drew the held
weapon on the skin's `weapon_current_bg` — an opaque chamfered plate — which made
the brightest, hardest-edged object on screen the one thing a player never needs
to study. That brightness is in the asset, so no cvar could tone it down, and
`hud_panel_fg_alpha` would have dimmed the weapon icons with it.

The replacement is a **list**, not a row of buttons: one rounded region on the
right edge holding what you own, with the held weapon washed in the accent colour
the way a list view marks a selected row. Each row is two columns — impulse key,
then icon. The key started as a corner badge, but a row is 26 units and the digit
is 10, so there is no corner for it to sit in: it landed just above centre,
overlapping the icon's box and reading as a mistake rather than a decision. As a
column it aligns down the whole strip. Two reasons it is one region rather than a
plate per weapon. It is a single
inventory, so it should read as one object (Law of Common Region). And a rounded
rect is nine draws, so per-slot plates put the overlay at 175 draw calls, where
the list is a fixed ~18 no matter how many weapons you hold.

The first attempt did draw a plate per slot, at latch opacity, and reproduced the
exact complaint about the stock panel in our own palette — a readout was again the
loudest thing on screen. Marking a selection is a job for hue and an edge, not
weight. Switching stays with the WEP button and the wheel; the strip is a readout.

`touch_mobile_hud` is the single switch for all three — vitals, ammo and weapons —
because it is one decision (does the touch layer own the HUD), and three switches
would let a player end up with two weapon strips or none.

### 7.2.1 Where the engine's own feeds go

Three feeds are drawn by the engine or by stock panels, and all three shipped
somewhere that a touch layout cannot afford:

| Feed | Was | Now |
|---|---|---|
| Kill notifications | bottom-right, printing lines across FIRE, HOP and DUCK | right band under the chrome pills, stopping short of the weapons strip's column |
| Chat | bottom-left at 2.7 mm type, under the move stick | left band between the vitals and the stick, 4.7 mm |
| Match clock | crowded against the chrome pills | centre of the top band, which the pills vacated |

Their content stays with the stock panels — warmup, overtime and round rules are
already implemented there and a second copy would drift. Only placement and
weight are ours.

### 7.3 Presets

`standard.cfg` is the layout, and `left`, `casual`, `competitive` and `minimal`
now `exec` it and state only what they change. Previously each carried a full
copy of every coordinate, so this redesign would have had to be applied five
times and `left.cfg` had already drifted. `left.cfg` is down to two lines, since
`Touch_MirrorX` and the look-zone flip already handle handedness.

Preset choices worth noting: `competitive` turns the second fire button on (claw
grip is the point of that preset) and raises the 1€ filter cutoff; `casual` and
`minimal` use `touch_hop_mode 2` so hopping needs no input at all.

## 8. Research basis

Findings referenced above, with sources:

- Bunny-hop is a hold: [Xonotic in-game guide source](https://xonotic.org/doxygen/qcsrc/pages_8qc.html), [Halogene's newbie corner](https://xonotic.org/posts/2012/halogenes-newbie-corner-part-3-ramp-jumping/)
- `cl_autojump` precedent: [Client-Mod](https://github.com/hasandramali/Client-Mod)
- Terraria jump modes: [505 Games support](https://support.505games.com/support/solutions/articles/150000147248-i-don-t-like-the-new-controls-can-i-change-or-customize-them-)
- CoDM R-Fire / camera rotation, fixed joystick: [Charlie INTEL settings](https://www.charlieintel.com/call-of-duty-mobile/best-cod-mobile-settings-multiplayer-145921/)
- PUBGM left fire button, 4-finger HUD: [buffget](https://buffget.com/news/pubg-mobile-4-finger-claw-guide-best-hud-for-conqueror), [lootbar](https://www.lootbar.com/blog/en/best-hud-layout-guide-2-3-4-finger-setup-pubgm.html)
- Don't shoot where the finger is (occlusion, ~40 px hit area): [Game Developer UX deep-dive](https://www.gamedeveloper.com/game-platforms/ux-deep-dive-painful-reality-of-building-fps-experiences-on-mobile---part-1)
- Thumb arc 27.5–41.3 mm, 9 mm for 90% accuracy: [doi:10.1145/3490355.3490357](https://doi.org/10.1145/3490355.3490357)
- Tablet centre unreachable: [Wolf, touch accessibility on held tablets](https://katrinwolf.info/wp-content/uploads/2014/04/eurohaptics2014_touchAccessibility_cameraReady.pdf)
- 24 mm ideal virtual control size: [Game Accessibility Guidelines](https://gameaccessibilityguidelines.com/ensure-interactive-elements-virtual-controls-are-large-and-well-spaced-particularly-on-small-or-touch-screens/)
- 9 mm / 48 dp minimum: [Android accessibility](https://support.google.com/accessibility/android/answer/7101858); 44 pt: [Apple](https://developer.apple.com/design/tips/)
- Tablets ≥10 mm: [NN/g tablet UX report](https://media.nngroup.com/media/reports/free/Tablet_Website_and_Application_UX.pdf)
- GNOME reserves top/bottom edge drags and 3/4-finger gestures: [GNOME HIG pointer & touch](https://developer.gnome.org/hig/guidelines/pointer-touch.html)
- libadwaita colours, radii, opacity tokens: [CSS variables reference](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/css-variables.html)
- Dark 1–2 px stroke is the largest readability win; 20–40% backing opacity: [overlay legibility analysis](https://live-subtitles.com/articles/en/article-22.html)
- Adaptive polarity over arbitrary content: [Meet Liquid Glass, WWDC25](https://developer.apple.com/videos/play/wwdc2025/219/)
- 5–10 ms perceptual floor for dragging: [Designing for Low-Latency Direct-Touch Input, UIST'12](https://www.tactuallabs.com/papers/designingLowLatencyDirectTouchInputUIST12.pdf)
- 1€ filter, tuning procedure: [Casiez](https://gery.casiez.net/1euro/); Steam Deck adoption: [GamingOnLinux](https://www.gamingonlinux.com/2023/09/valve-overhauled-steam-deck-as-mouse-gyro-option-in-new-beta/)
- No majority default layout (25% each of four presets): [Game Accessibility Guidelines](https://gameaccessibilityguidelines.com/allow-interfaces-to-be-rearranged/)

## 9. Dev loop

Four things had to be fixed before any of the above could be *seen*, let alone
judged. They are recorded here because each one silently produced a plausible
wrong answer.

### 9.1 Deploys were going nowhere

`scripts/dev-deploy.sh` had been uploading into `~/.local/share/xonotic-touch/data`,
which the Flatpak build abandoned when it moved its packs under
`~/.var/app/<id>/data/xonotic-touch/data` so that "Delete app data" would wipe
them. Every deploy silently did nothing. It now probes for the directory that
actually holds `xonotic-*-data.pk3` and fails loudly if neither has assets.

Restarting also needed fixing: killing the engine strands the launcher's
`instance.lock` (so `start.sh` hands off to the tray instead of booting) and
DarkPlaces' `~/.xonotic/lock` (so the new process dies with a "session lock could
not be acquired" dialog), and the tray relaunches the engine on its own and
races our launch. The restart now stops the tray, clears both locks, and boots
with `XONOTIC_TOUCH_NO_TRAY=1 … -condebug`.

### 9.2 Configs must go to the engine userdir

Deployed presets kept reverting to their old values mid-session. The launcher
runs `sync-bundle-data.sh`, which restores config files from the Flatpak bundle
over anything in the data directory. Uploading a preset there is a race that the
bundle wins. Configs now also go to `~/.xonotic/data/`, the engine's userdir,
which is the highest-priority search path and is not something the bundle sync
touches.

Worth stating plainly, because it cost an hour of reading correct-looking code
that was executing a stale file: **when a config change appears not to work,
verify which copy the engine loaded before changing the code.**

### 9.3 Screenshots must come from the engine

`gdr screenshot` returned a frame showing the loading screen long after the map
had loaded — a stale compositor buffer for a fullscreen GL surface. The evidence
said "the map never finishes loading"; the game was fine. Captures now come from
the engine's own `screenshot` command (F12) and are pulled from the userdir.
`scripts/dev-shot.sh` wraps the whole sequence — join a bot match, wait for
`alive`, capture, pull.

One note for anyone driving this device: `gdr key` needs numeric evdev keycodes,
not names. `88`, not `F12`.

### 9.4 State preview instead of synthetic multi-touch

Pressed, latched and drag-look states only exist while fingers are down, and
`tools/touch-inject.py` (uinput virtual touchscreen) never reached the engine's
input path. Rather than keep debugging the injector, `touch_debug 3` forces every
control into its active state so the feedback can be inspected in a still frame
([03-active-states.jpg](test-runs/2026-08-08-touch-ux/shots/03-active-states.jpg)).
That verifies rendering, not input; the input model still needs a human with two
thumbs, and this document's §6 claims are unverified in that sense.

```bash
scripts/dev-deploy.sh --shot NAME   # compile QC, upload, restart, screenshot
scripts/dev-shot.sh NAME            # join a bot match and capture in-game
```

### 9.5 Taps do reach the game — in physical pixels

§9.4 concluded that synthetic touch could not be delivered. That was a coordinate
error, not a limitation. Three spaces are in play and no two of them match:

| Space | Size here | What uses it |
|---|---|---|
| Console (2D) | 810 × 540 | all overlay drawing and hit-testing, `Touch_UISize()` |
| Video | 1440 × 960 | engine `screenshot`, `VF_SIZE` |
| Physical | 2880 × 1920 | `gdr click`, `gdr screenshot` |

`gdr click` wants **physical** pixels, so a control located in an engine
screenshot needs its coordinates doubled. Taps aimed with screenshot coordinates
land in the wrong quadrant and do nothing, which is what made the input path look
dead. Console size is derived from the surface each frame
(`vid_conwidthauto 1`), so read it rather than assume it.

Residual harness limit: a synthetic click's press and release can fall inside one
engine frame, so the finger is never observed and about one tap in three is lost.
A real finger holds for 50–100 ms. Space scripted taps ≥ 1 s apart.

### 9.6 Read the engine's state instead of inferring it

`cvarlist <prefix>` is the only way to get a value out of a running engine over
SSH; `echo` does not expand cvars. `scripts/dev-deploy.sh` writes `xt-probe.cfg`,
bound to **F8**, which dumps `vid_con*`, `con_chat*`, `hud_panel_chat*`,
`hud_panel_weapons*`, `touch_hop*`, `touch_mobile_hud` and
`touch_layout_version` to `/tmp/xonotic-dev.log`. Several rounds of layout
debugging were spent guessing at values this prints in one keypress.

## 10. Performance

Every performance preset set `fps_max`, which is not a Xonotic cvar. The engine
accepted the line, created a dud, and ran uncapped — so the thermal preset for a
fanless tablet had no frame cap at all, and had appeared to work for as long as
it had existed. The cvar is `cl_maxfps` (plus `cl_maxidlefps`). With that fixed
the 30 fps cap in `thermal.cfg` applies, which is the largest single power win
available here and worth more than the overlay's entire draw cost.

Confirmed on device: capped at 30 fps with headroom to spare, versus >110 fps
uncapped in the same scene.

## 11. Verified on device

Surface Pro 9, Ultramarine Linux, touch only, via `gdr --dev=marinesurface`.
Screenshots in [`docs/test-runs/2026-08-08-touch-ux/shots/`](test-runs/2026-08-08-touch-ux/shots/).

| | Result |
|---|---|
| QC compile | clean, no new warnings (GMQCC treats them as errors) |
| Rounded shapes | `shape tga`, all masks load; flat fallback also exercised |
| Legibility | holds over both dark geometry and a bright skybox |
| FIRE size | 12.8 mm radius = 25.6 mm across, matching the 26 mm target |
| MOVE / FIRE alignment | shared centre line |
| Overlay cost | 78 draw calls, 30 fps cap sustained |
| Edit mode | handles track the finger; hop bar grabbable end to end |
| Presets | load from the userdir copy; `cl_maxfps` applies |
| Vitals | health, armour and ammo draw live and track values |
| Chat | CHAT pill opens the sheet; keys type; SEND reaches the server; the sent line comes back into the backlog |
| Chrome pills | legible over a sunlit white wall after the plate fix |

Single-finger interaction is now verified by driving real taps (§9.5): the chat
composer was typed and sent from the on-screen keyboard, which is what exposed the
four defects in
[FINDINGS-chat-verification.md](test-runs/2026-08-08-touch-ux/FINDINGS-chat-verification.md).

Still not verifiable this way: everything in §6 that depends on *concurrent*
fingers — the hop latch, fire-drag-look, tap-to-fire, and whether the 1€ filter
defaults feel right while aiming. Those need hands on the tablet, and that is the
next thing to do.
