# Pause escape — device findings (2026-07-25)

## Result

Touch pause/escape path works on Ultramarine Surface after loading the new `csprogs.dat` / `menu.dat`.

| Check | Result |
|-------|--------|
| `touch_pause` opens sheet | Pass (`touch_pause_sheet_open` echo + screenshot) |
| Sheet actions: RESUME / LEAVE MATCH / QUIT | Visible on sheet |
| Menu **◀ RESUME** bar | Pass (drawn while connected + menu/console) |
| GameMenu **Resume** row | Added (always) |
| PAUSE pill tap via ydotool | Flaky when menu/console has focus; use pill in pure gameplay or `touch_pause` |

## Blocker found during test

A leftover `zzzz-touch-pause.pk3dir` with an older `csprogs.dat` overrode newer packages (`zzz-*` / `/app/data`). Symptom: `touch_save` worked, `touch_pause` was `Unknown command`. Removed stale `zzzz-*` dirs after syncing.

## Notes

- Prefer `touch_pause_freeze 0` (default) so the CSQC sheet stays the visible UI; listen-server console open still triggers engine `PAUSE` via autopause.
- Escape paths without keyboard: PAUSE pill / `touch_pause` sheet, menu **◀ RESUME**, GameMenu **Resume**, CONSOLE pill to dismiss console.
