# Touch console redesign — device deploy (2026-07-27)

Target: `borysthebear@100.125.7.103` (Ultramarine Surface, GNOME Wayland)  
Build: on-device `darkplaces-sdl` (Fedora ABI) + local `menu.dat` / `csprogs.dat` / `progs.dat`

## Result

| Check | Result |
|-------|--------|
| Engine build on tablet | Pass (`make sdl-release DP_SSE=0`) |
| Local Ubuntu binary | Fail — needs `libjpeg.so.8` / `LIBJPEG_8.0` (Fedora has `.62`) |
| Launch via `run-fixed.sh` | Pass — host binary at `~/xonotic-touch-test/xonotic-host` |
| `menu.dat` override | Pass (`zzz-touch-fix.pk3dir`, crc shows new menu) |
| `TouchConsolePreview` in menu.dat | Pass (string present) |
| `touch_kb_*` / TouchUI symbols in binary | Pass |

## Deploy path

1. Built QC locally; built engine on device (ABI mismatch for jpeg).
2. Rsynced `menu.dat` / `csprogs.dat` / `progs.dat` → `~/.local/share/xonotic-touch/data/zzz-touch-fix.pk3dir/`
3. Copied on-device `darkplaces-sdl` → `~/xonotic-touch-test/xonotic-host`
4. Cleared stale `zzzz-*` override dirs
5. Launched: `XONOTIC_TOUCH_HOST_BIN=…/xonotic-host ~/xonotic-touch-test/run-fixed.sh`

## Preview fix (same day)

Root cause for missing Settings preview:

1. Touch tab `rows` was 24 vs Settings TabController height **15.5** → `Tab dialog height mismatch!`
2. Stale `zzzz-touch-fix.pk3dir` (Jul 25) shadowed newer `zzz-touch-fix.pk3dir` `menu.dat`

Fix: restore `rows=15.5`, put Console & chat + menu-QC live preview at top of Touch tab, overwrite/clear `zzzz-*` so CRC `52774` / size `2121K` loads (`Console preview` string present).

## Manual test checklist (on tablet)

- [ ] Settings → Touch controls → **Console & chat** preview updates when dragging height / opacity / shade
- [ ] In-game: tap **CONSOLE** → glass KEYS sheet with CLOSE / COMMANDS
- [ ] SHIFT / ?123 → type `touch_con_x 0.4`
- [ ] TAB / HIST / PGUP / hold BKSP
- [ ] COMMANDS tab runs a preset
- [ ] Pause → Main menu disconnects (rides along from earlier fix)

## Notes

- Prefer on-device engine builds for Fedora/Ultramarine; Ubuntu host binaries need jpeg soname 8.
- Early `Unknown command "touch_*"` lines while execing profiles before CSQC registers cvars are expected.
- Log: `~/xonotic-touch-test/logs/game-console-redesign.log`
