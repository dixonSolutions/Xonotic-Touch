# Touch pause / escape

Keyboard-free escape from gameplay and the console.

## UX

1. **PAUSE** glass pill (top system row, left of **CONSOLE**).
2. Tap (drag-cancel, same as CONSOLE) fires the same path as **Escape**:
   - In-game → Xonotic **GameMenu** (`menu_showgamemenudialog`)
   - Otherwise → `togglemenu 1`
3. Local games auto-pause while the menu or console is open (engine behaviour).
4. GameMenu **Resume** closes the menu and returns to play. No duplicate custom pause sheet.
5. While the **console** is open, the screen is dimmed. Only **CLOSE CONSOLE** and the in-engine keyboard receive taps (HUD/menu absorbed). The engine also requests the platform OSK (`SDL_StartTextInput` — GNOME / Ubuntu Touch).

The old top-left `◀ RESUME` overlay is **removed** — use GameMenu **Resume** only.

## Layout (960×640 reference)

| Widget | x | y | size | aspect |
|--------|---|---|------|--------|
| PAUSE pill | 0.300 | 0.050 | 0.058 | 2.2 |
| CONSOLE pill | 0.500 | 0.050 | 0.058 | 4.2 |
| CLOSE CONSOLE | ~55% width, mid-screen (engine, console keydest only) |
| Console keyboard | Bottom ~48% — letters, digits, SPACE/BKSP/ENTER (engine) |

## Files

| Area | Path |
|------|------|
| CSQC pause pill | `client/touch_pause.qc` / `.qh` |
| Input / draw wiring | `touch_input.qc`, `touch_draw.qc` |
| Console close bar | `engine/darkplaces/vid_sdl.c` (`IN_Move_TouchScreen_Xonotic`) |
| GameMenu Resume | `menu/xonotic/dialog_gamemenu.qc` |
| Defaults | `touch/profiles/standard.cfg`, `left.cfg` |

## Acceptance

| ID | Check |
|----|--------|
| P1 | Tap PAUSE → GameMenu opens and match freezes (listen/local) |
| P2 | GameMenu Resume → back to play |
| P3 | Tap CONSOLE → console opens |
| P4 | Tap CLOSE CONSOLE → console closes |
| P5 | No custom RESUME/LEAVE/QUIT sheet overlay |
