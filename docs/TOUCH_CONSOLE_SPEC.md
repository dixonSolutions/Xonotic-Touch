# Touch console / chat sheet

Keyboard-free text entry for the engine console and chat, with a no-typing command palette.

## UX

1. **CONSOLE** glass pill (CSQC) → opens the engine console (`toggleconsole`).
2. Console sheet (engine):
   - Header: **CLOSE** · **KEYS** · **COMMANDS** · **PGUP** · **PGDN**
   - Scrollback above the keyboard (native console)
   - **KEYS** tab: layered QWERTY with SHIFT / ?123, TAB completion, HIST, caret, SPACE, ENTER, hold-repeat BKSP
   - **COMMANDS** tab: file-driven preset grid (`touch/console_palette.txt`)
   - While spectating or observing on a server, the COMMANDS tab leads with the
     actions the game's own prompts name keys for, highlighted (see below)
3. Chat (`messagemode` / `key_message`): compact sheet with quick-phrase strip + same keyboard.
4. Keyboard height / opacity / shade are engine cvars (`touch_kb_*` / `touch_conui_*`), tunable via cfg/console.
   Settings → Touch controls covers presets, look sensitivity, opacity, and scale (no live preview panel).

## Spectator actions

A spectator sees prompts such as "Press SPACE to join" or "Press MOUSE1 to
spectate". On a public server those come from the server's own CSQC, which the
touch layer cannot rewrite (the port's CSQC answers with on-screen labels
instead, `Touch_CommandLabel`). So the engine puts the same actions at the head
of the COMMANDS tab while they apply. Each runs the command the prompt names and
closes the sheet, so the player sees the result.

| State | Detected by | Entries |
|-------|-------------|---------|
| Observing (free fly) | own scoreboard frags `-666`, view on self | spectate (`+fire`), fly faster (`weapnext`), fly slower (`weapprev`), join |
| Spectating a player | frags `-666`, view moved to another player | next player (`weapnext`), prev player (`weapprev`), observe (`+fire2`), camera (`weapon_drop`), join |
| Out of the round | frags `-616`, watching another player | next player, prev player, camera |

Not shown at intermission, in demos, or while playing. `+fire` / `+fire2` are
released with `defer 0.2`: a release in the same frame resets the button before
any packet carries the press. A palette-file entry with the same label as a
shown action (the stock file's `join` and `spectate`) is hidden meanwhile.
"Press i for gametype info" has no entry: `+show_info` shows its panel only
while held. Code: `TouchUI_SpectatorMode` in `touch_ui.c`.

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
| C10 | Observing on a public server: COMMANDS leads with spectate / fly faster / fly slower / join; tapping spectate closes the sheet and spectates a player |
| C11 | Spectating: next player switches player, observe returns to observing |
