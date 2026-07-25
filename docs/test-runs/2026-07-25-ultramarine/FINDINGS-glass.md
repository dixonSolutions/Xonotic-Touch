# Glass layout session — 2026-07-25 (evening)

Target: Ultramarine `borysthebear@100.125.7.103`  
Spec: `docs/TOUCH_LAYOUT_SPEC.md`  
Runner: `~/xonotic-touch-test/run-fixed.sh`  
Override: `~/.local/share/xonotic-touch/data/zzz-touch-fix.pk3dir/csprogs.dat`

## Shipped in this pass

| Area | Status |
|------|--------|
| Opus layout spec | Done (`TOUCH_LAYOUT_SPEC.md`) |
| Sticky finger table + grace | Done (`touch_input.qc`) |
| Look EMA / ownership / no double-apply | Done (`touch_look.qc`, `Touch_Frame` before `SetCamera`) |
| Edge-triggered keys (D8) | Done |
| FIRE hold + SPACE bar hit/draw | Done (code) |
| CON hold-drag + debounced `seta` save | Done (`touch_console.qc`, `touch_config.qc`) |
| Chamfer glass (no scanlines) | Done (`touch_glass.qc`) |
| Profiles §3.1 + thermal glass_quality | Done |
| Layout path D14 (`touch.layout.cfg` after profiles) | Done (`packaging/start.sh`, `xonotic-shlib.sh`) |
| Remote CSQC deploy | Done |

## Verified on device

1. **CSQC override loads** — engine log shows changing CRC as we redeployed (`zzz-touch-fix.pk3dir`).
2. **CON handle** — top-middle glass bar labeled `CON` at `0.500, 0.045` (screenshots `08`–`14`).
3. **`touch_debug` line** — prints `con 0.500000,0.045000`.
4. **Autoscreenshot hook** — `Touch: autoscreenshot fired` works for remote capture.
5. **Profiles exec** — `standard.cfg` + `thermal.cfg` run after CSQC init.

Screenshots: `docs/test-runs/2026-07-25-ultramarine/screenshots/glass/`.

## Follow-up (same evening, after remote-verify agent)

Applied Opus blockers locally and redeployed (`csprogs` size ~4064K, new CRC on load):

| ID | Fix |
|----|-----|
| B1 | `touch_scoreboard_gesture` default **0**; two-finger team-select path removed |
| B2 | Pointer via `gettouchfinger(10)`; MOUSE1 consume always (no `getmousepos` 0,0 bail) |
| B3 | Look ownership uses `setsensitivityscale` + `GetCurrentFov` honors `Touch_Look_BlocksMouse()` |
| B4 | CON exempt from finger grace; hold promote only while live finger |
| B5 | Residual gaps (upper-left) assign **LOOK** |
| H2 | `Touch_ReleaseAll` clears finger table + `Touch_Console_Reset` |

Also fixed `glass-accept.sh`: **logical** 1440×960 coords (not physical 2880×1920); layout check includes `~/.xonotic/data/data/touch.layout.cfg`.

### Retest results
- Debug line still reports `con 0.500,0.045` and **`fills 31`** (= CON+MOVE+FIRE+SPACE+CR+WEP draw path ran).
- Solid RGB `drawfill` probes at widget centers still **do not appear** in in-engine TGA (pixel search finds no solid red/green squares). **`drawstring` works** (CON label + debug). So visibility is not just “menu covering controls” — CSQC `drawfill` → `screenshot` is broken or discarded on this build; CON “bar” may be mostly the label.
- CON drag via ydotool still does not change `touch_con_*` on disk (layout mtime unchanged without a successful drag+save).
- Wayland fullscreen remains **1440×960**; `fps_max`/`r_particles` still “Unknown” from profile exec (FPS ~120).

## Open issues (next session)

1. **CSQC `drawfill` invisible in `screenshot` TGA** while `drawstring` and fill-count debug work. Try `drawpic` white texel, `DRAWFLAG_ADD`, or Wayland capture (`gnome-screenshot` / Shell API) with console+menu fully dismissed; then restore glass alpha.
2. **Resolution stays 1440×960** under GNOME/Wayland fullscreen despite `vid_width 960`.
3. **`fps_max` / `r_particles` “Unknown command”** from CSQC-triggered profile exec — keep thermal caps on launcher `+set`.
4. **ydotool + `vid_touchscreen`** still flaky for CON drag / Join dismiss; prefer on-device finger for T13.
5. **`touch_customize` cvar vs command** — toggle via cvar only.

## How to continue on Ultramarine

```bash
export SSHPASS=1122
sshpass -e ssh -o PreferredAuthentications=password -o PubkeyAuthentication=no borysthebear@100.125.7.103
# restart
killall xonotic 2>/dev/null; ~/xonotic-touch-test/run-fixed.sh &
# in-game (landscape): Join, then:
#   touch_debug 1
#   screenshot
# Hold CON 350ms and drag; confirm ~/.xonotic/data/touch.layout.cfg has seta touch_con_*
```

Rebuild CSQC locally:

```bash
cd engine/data/xonotic-data.pk3dir/qcsrc
make ../csprogs.dat QCC=$PWD/../../gmqcc/gmqcc
# scp to both zzz-touch-fix.pk3dir and xonotic-data.pk3dir under ~/.local/share/xonotic-touch/data/
```
