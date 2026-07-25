# UI redesign — mid-game glass (2026-07-25)

## User feedback
No visible console control; bottom looked like overlapping boxes — not a usable tablet FPS HUD.

## Root causes (from mid-game screenshots)
1. **Labels scaled to widget diameter** (`size * 0.35`) → huge white text piled on the bezel; glass fills were invisible → “overlapping boxes”.
2. **CSQC `drawfill` often never appears** in in-engine TGA on this Flatpak build; `drawstring` did. Panels now use **`drawpic gfx/colors/white`**.
3. **MOVE stick** was nested `DrawWidget` base+knob → double rectangle. Simplified to one plate + rim + knob.
4. **Tiny “CON” chip** at low contrast. Replaced with a top **CONSOLE** pill.

## New default layout (`touch/profiles/standard.cfg`)

| Widget | x | y | size | aspect |
|--------|---|---|------|--------|
| MOVE | 0.165 | 0.680 | 0.250 | — |
| FIRE | 0.860 | 0.640 | 0.200 | — |
| JUMP | 0.860 | 0.860 | 0.110 | 2.6 |
| CR | 0.700 | 0.860 | 0.100 | — |
| WEP | 0.540 | 0.860 | 0.100 | — |
| CONSOLE | 0.500 | 0.050 | 0.058 | 4.2 |

Raised off the bottom edge; FIRE above JUMP on the right; secondary CR/WEP on a shared baseline with gaps.

## Code changes
- `touch_glass.qc` — `Touch_Glass_Fill` / `Frame` via `drawpic`; clearer stick
- `touch_draw.qc` — capped label font; JUMP single label; probes only at `touch_debug >= 2`
- `touch_console.qc` — “CONSOLE” pill
- `touch/profiles/standard.cfg`, `left.cfg`
- `gfx/colors/white.tga` shipped in override pk3dir
- Screenshots: `screenshots/glass/midgame-engine-*.png`, `ui-fix-latest.png`, `ui-fix-v2*.png`

## Coordinate bug (critical)
Wayland surface is often **1440×960** while `vid_conwidth/height` stay **960×640**. CSQC 2D is clipped to con size. Laying out with `VF_SIZE` drew FIRE/JUMP/CR past x=960 (invisible). Fixed: `Touch_UISize()` uses conwidth/conheight; `Touch_NormalizeInput` no longer upscales to VF.

## Still hard remotely
Join/Spectate + engine console stay open under ydotool, so freelook frames are rare. On-device: Join, Esc until freelook, confirm CONSOLE pill top-center and MOVE/FIRE/JUMP without overlap.
