# Touch pause / escape

Keyboard-free escape from gameplay and menus.

## Problem

Escape / pause previously required `K_ESCAPE`. Touch had no entry into pause/menu and no reliable way back from the game menu.

## UX

1. **PAUSE** glass pill on the top system row (left of **CONSOLE**).
2. Tap (with drag-cancel, same as CONSOLE) opens a modal sheet: **RESUME**, **MENU**, **LEAVE MATCH**, **QUIT** (QUIT needs two taps within 3s).
3. Sheet always opens on the client; engine `pause` is requested when `touch_pause_freeze 1` (listen/local). Title shows `PAUSED` vs `GAME MENU`.
4. While the menu is open and the player is connected, a top-left **◀ RESUME** bar closes the menu (`m_hide`). GameMenu also gains a **Resume** row when `vid_touchscreen`.

Timers on the sheet use `gettime(GETTIME_REALTIME)` because engine `time` freezes while paused.

Console helpers: `touch_pause` / `touch_pause_exit`. Default `touch_pause_freeze 0` so the sheet stays the visible UI (listen-server console still auto-pauses the world).

## Layout (960×640 reference)

| Widget | x | y | size | aspect |
|--------|---|---|------|--------|
| PAUSE pill | 0.300 | 0.050 | 0.058 | 2.2 |
| CONSOLE pill | 0.500 | 0.050 | 0.058 | 4.2 |

Sheet buttons are fixed (not affected by `touch_scale`): RESUME largest at y≈0.313, then MENU, LEAVE, QUIT.

## Files

| Area | Path |
|------|------|
| CSQC pause module | `client/touch_pause.qc` / `.qh` |
| Input / draw wiring | `touch_input.qc`, `touch_draw.qc` |
| Cvars | `touch_init.qc` / `.qh`, `touch_config.qc` |
| Menu resume bar | `menu/xonotic/util.qc` (`postMenuDraw`) |
| GameMenu | `menu/xonotic/dialog_gamemenu.qc` |
| Defaults | `touch/profiles/standard.cfg`, `left.cfg` |

## Acceptance (device)

| ID | Check |
|----|--------|
| P2 | Tap PAUSE → sheet opens |
| P5 | RESUME closes sheet and restores play |
| P6 | MENU → GameMenu; ◀ RESUME returns to game |
| P8 | LEAVE MATCH disconnects |
| P9 | QUIT requires two taps |
| P12 | FIRE/MOVE ignored while sheet open |
