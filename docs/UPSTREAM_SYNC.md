# Upstream sync record

How this monorepo pulls latest Xonotic engine/game code while keeping Touch port work.

## Last sync (2026-07-27)

| Component | Upstream | SHA |
|-----------|----------|-----|
| darkplaces | https://gitlab.com/xonotic/darkplaces.git | `d93f9c4292039354a2b8d40d11bc386891e55fe5` |
| xonotic-data.pk3dir (`qcsrc/`) | https://gitlab.com/xonotic/xonotic-data.pk3dir.git | `8bca8a41e806e658485dddd221b6be5dc0d123f1` |
| gmqcc | https://gitlab.com/xonotic/gmqcc.git | `b2a319efb41069b9a250e76991dbe505ff8d030d` |

Branch used: `migrate/upstream-2026-07-27`.

## Recommended workflow (vendored monorepo)

Nested `git merge` of freshly `git init`'d trees against GitLab is an **unrelated history** and is painful. Prefer:

1. Commit or branch all Touch WIP.
2. Shallow-clone upstream tips (for data, sparse-checkout `qcsrc/` is enough).
3. Classify paths:
   - **Touch-only** — keep verbatim (e.g. `touch_ui.c/.h`, `client/touch_*.qc`, touch menu dialogs).
   - **Shared hotspots** — take upstream, then re-port Touch behaviour.
   - Everything else under the synced trees — take upstream.
4. Rebuild `gmqcc`, `menu.dat` / `csprogs.dat` / `progs.dat`, and `darkplaces-sdl`.
5. Update the SHA table above.

Optional nested-git / GitLab fork path: [`scripts/sync-upstream-fork.sh`](../scripts/sync-upstream-fork.sh) (`--init-git`, `--allow-unrelated`). Fork push only when `FORK_*` URLs are set.

## Touch-only paths (do not drop)

**darkplaces**

- `touch_ui.c`, `touch_ui.h`

**xonotic-data `qcsrc/`**

- `client/touch_*.qc` / `.qh`, `client/touch_api.qh`
- `menu/xonotic/dialog_settings_touch.*`
- `menu/xonotic/dialog_touch_*.`*
- `menu/xonotic/touch_*.`*, `touchbutton.*`
- `dpdefs/ut_touchfinger.qc`
- `common/mutators/mutator/overkill/cl_overkill.*` (Touch CSQC hook)

Do **not** restore obsolete `client/mapvoting.*` / `server/mapvoting.*` — upstream now owns `common/mapvoting/`. Touch code that included `<client/mapvoting.qh>` must use `<common/mapvoting/_mod.qh>` instead (see `client/touch_input.qc`).

## Shared hotspots (re-port after overlay)

**darkplaces:** `vid_sdl.c`, `cl_screen.c`, `screen.h`, `makefile.inc`, `clvm_cmds.c`, `svvm_cmds.c`, `vid_shared.c`, `vid.h`

**qcsrc:** `client/main.qc`, `client/view.qc`, `client/command/cl_cmd.qc`, `client/hud/panel/weapons.qc`, `menu/menu.qc`, `menu/item/inputbox.qc`, `menu/xonotic/dialog_gamemenu.qc`, `leavematchbutton.*`, `dialog_settings.qc`, `mainwindow.qc`, `dialog_firstrun.qc`, `dialog_termsofservice.qc`, `server/main.qc`, `*/_mod.inc` / `*/_mod.qh` include lists
