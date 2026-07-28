# Touch console / chat sheet

Keyboard-free text entry for the engine console and chat, with a no-typing command palette.

## UX

1. **CONSOLE** glass pill (CSQC) → opens the engine console (`toggleconsole`).
2. Console sheet (engine):
   - Header: **CLOSE** · **KEYS** · **COMMANDS** · **PGUP** · **PGDN**
   - Scrollback above the keyboard (native console)
   - **KEYS** tab: layered QWERTY with SHIFT / ?123, TAB completion, HIST, caret, SPACE, ENTER, hold-repeat BKSP
   - **COMMANDS** tab: file-driven preset grid (`touch/console_palette.txt`)
3. Chat (`messagemode` / `key_message`): compact sheet with quick-phrase strip + same keyboard.
4. Keyboard height / opacity / shade are engine cvars (`touch_kb_*` / `touch_conui_*`), tunable via cfg/console.
   Settings → Touch controls covers presets, look sensitivity, opacity, and scale (no live preview panel).

## Layout (960×640 reference)

| Region | Fraction of sheet |
|--------|-------------------|
| Header | ~10% height |
| Log / body | remainder above keyboard |
| Keyboard / palette | `touch_kb_height` (default 0.46) |

Optional `touch_kb_split 1` places left/right halves under both thumbs in landscape.

## Cvars

| Cvar | Default | Meaning |
|------|---------|---------|
| `touch_kb_height` | `0.46` | Keyboard fraction of sheet height |
| `touch_kb_gap` | `0.008` | Gap as fraction of sheet width |
| `touch_kb_opacity` | `0.92` | Glass plate opacity |
| `touch_kb_layout` | `0` | 0=QWERTY, 1=compact (reserved) |
| `touch_kb_split` | `0` | Split landscape keyboard |
| `touch_kb_minkey_px` | `36` | Warn when keys shrink below this |
| `touch_conui_shade` | `0.62` | Console background dim |
| `touch_conui_palette_file` | `touch/console_palette.txt` | Preset commands file |

## Files

| Area | Path |
|------|------|
| Layout / keyboard / palette | `engine/darkplaces/touch_ui.c` / `.h` |
| Input wiring | `engine/darkplaces/vid_sdl.c` |
| Glass draw | `engine/darkplaces/cl_screen.c` |
| Settings UI | `menu/xonotic/dialog_settings_touch.qc` |
| Palette defaults | `touch/console_palette.txt` |
| Preset defaults | `touch/profiles/standard.cfg`, `left.cfg` |

## Acceptance

| ID | Check |
|----|--------|
| C1 | Tap CONSOLE → sheet opens with glass keys and CLOSE |
| C2 | SHIFT then letter → uppercase; ?123 → symbols including `_` |
| C3 | Type `touch_con_x 0.4` and ENTER — cvar changes |
| C4 | TAB completes a partial cvar/command |
| C5 | HIST / NEXT walk command history; PGUP/PGDN scroll log |
| C6 | Hold BKSP deletes repeatedly |
| C7 | COMMANDS tab runs a preset (e.g. screenshot) |
| C8 | Chat sheet appears for `say` / messagemode without relying on compositor OSK |
| C9 | Console sheet still respects `touch_kb_*` / `touch_conui_*` cvars from cfg |
