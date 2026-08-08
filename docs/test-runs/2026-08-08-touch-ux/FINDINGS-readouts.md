# Findings — the readout layer, on device

Round after the chat and weapons-strip work. Everything here was found by looking
at engine screenshots of a live bot match at pixel level, not by reading code.

## 1. The ammo count was in the thumb's path

Ammo had its own anchor at `0.855 / 0.795`, chosen because the count "belongs to
the trigger". On device that is the gap between FIRE (centre `0.680`) and HOP
(centre `0.870`) — the strip of screen the right thumb crosses every time it moves
between them. So the number was covered by the thumb exactly while it was being
spent, and, drawing no surface, it also floated over whatever the level put behind
it. In [64-final.jpg](shots/64-final.jpg) it reads as a stray `⏸ 15` between two
controls; the two bars are the shells icon.

It is now the third row of the player-state group, top-left, sharing the icon and
number columns with health and armour ([73-state-group.jpg](shots/73-state-group.jpg)).
`touch_ammo_x` / `touch_ammo_y` are gone; `TOUCH_LAYOUT_VERSION` is 4 so saved
layouts carrying them are migrated.

## 2. A centre anchor plus a taller group ran off the screen

The group's cvar anchors its *centre*. Adding a third row took it from 0.126 to
0.189 of the screen height, so at `touch_hud_y 0.075` the health row hung off the
top edge — visible in [71-ammo-clipped-top.jpg](shots/71-ammo-clipped-top.jpg),
where the health icon is cut by the frame. Anchor moved to `0.115`.

The same growth collided with the chat feed, whose band started at `0.150`: the
first chat line ran through the ammo row. The feed starts at `0.230` now.

Worth stating as a rule: a centred anchor means every size change is also a
position change. Both of these were size changes that nobody thought were moves.

## 3. The region plate was measured, and dropped

A rounded-rect plate behind the group looked like the obvious way to guarantee
contrast. Sampling the frame with ImageMagick instead of trusting the eye:

| Background | Without plate | With plate at `TOUCH_A_SURFACE` |
|---|---|---|
| Dark geometry | 57 | 30 |
| Sunlit wall | ~255 | ~175 |

So it neither disappeared nor guaranteed contrast — it dimmed a corner of the scene
for nine draw calls and left white-on-bright as bright as before. The labels
already draw a 1 px dark outline in four passes, which is what actually carries
legibility, and the numbers are readable over the white wall in
[73-state-group.jpg](shots/73-state-group.jpg) with no plate at all. Dropped.

The weapons strip keeps its plate: there it is doing a different job — making nine
loose icons read as one inventory.

## 4. Right-aligned numbers were not aligned

`Touch_Shape_LabelWidth` multiplied character count by a nominal per-character
advance. With a proportional font, "100" and "0" right-aligned to the same edge
landed about 3 px apart — invisible in a single readout, visible as a ragged column
once three are stacked. It now asks the engine (`stringwidth_builtin`); label
fitting uses the same measure at unit size, since width scales linearly with size.

## 5. A badge with no corner to sit in

The impulse key on each weapon row was placed as a top-left corner badge. A row is
26 units and the digit is 10, so after the inset the glyph sat just above centre,
overlapping the icon's box. It read as a misplacement rather than a decision. Each
row is now two columns, key then icon, which also aligns the keys down the strip
([74-weprow.jpg](shots/74-weprow.jpg)).

## 6. Two right-edge bands wanted the same pixels

At a full nine-weapon inventory the strip is 0.43 of the screen height. Centred at
`0.430` its bottom edge reached the FIRE ring, and its top ran into the kill feed's
band. The strip is centred at `0.360` now, and the kill feed stops at x `0.900`,
short of the strip's column. Sparse inventories hid this: it only appears once you
hold most of the weapons.

## Method note

`gdr screenshot` returns a stale compositor frame during a match, so all captures
here are engine-side (`scripts/dev-shot.sh`, F12 bound to `screenshot`). Judging
subtle alpha by eye on a downscaled view was wrong twice in this round — both
times `convert -crop … -format '%[fx:int(mean*255)]'` settled it in one command.
