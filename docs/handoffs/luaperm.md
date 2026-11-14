# Hand-off: Lua mod permissions (work/luaperm)

Branch `work/luaperm`, from origin/main `10c2d32`. It has not been merged into main yet
(see "Not done").

## The rule

A mod can use a tier only when its `mod.txt` **declares** it and the player has
**granted** it. `tlua_has` in `engine/darkplaces/touch_lua.c` checks this on every call.

| Declared | Granted | Effect |
|---|---|---|
| `standard` (implied for every mod) | always | allowed |
| a tier | yes | allowed |
| a tier | no, or not decided yet | `permission denied: the <tier> tier is not turned on for this mod` |
| not declared | yes (grant file edited by hand) | `permission denied: needs the <tier> tier (add "permissions: <tier>" to mod.txt)` |
| not declared | asked for (`grant`, `enable ... with`) | grant refused: `<mod> does not declare the <tier> tier ...` |
| no `mod.txt` at all | anything | standard only; listed as "declares nothing (no mod.txt): standard only"; grants refused |

Other rules:
- **Packages.** `lua_mod_install` refuses a zip, folder or link without `mod.txt`.
- **API 3 without a `permissions:` line.** The mod declares standard only.
- **Seen mask.** Stored in `luastate/<realm>/seen/<id>.txt` as hex. Without the file, the granted tiers count as seen. Any `grant`, `revoke`, `decline` or `enable ... with` marks every declared tier seen.
- **Not decided yet.** Declared optional tiers outside the seen mask are "not decided yet" and stay off. An update that declares more therefore keeps the old grants and asks about the new tiers only.
- **Commands:**
  - `lua_mod_enable <mod> [with <tiers>|all|none|keep [confirm]]`: `keep` turns the mod on with its grants as they are and marks the remaining declared tiers decided (off).
  - `lua_mod_permission <mod> grant|revoke|decline <tiers>|all [confirm]`.
  - `all` never includes `engine_full`. `engine_full` for a mod installed from a link needs `confirm` within 60 s.
  - Server text still cannot grant anything.
- **Live effect.** A revoke takes effect on the next call, with no restart.
- **Usage tracking** since the mod last started: calls per optional tier, hosts contacted by `xt.http`, and the number of `xt.engine.cmd` calls.
- **Icons.**
  - mod.txt names the icon: `icon: icon.png` (`.png`/`.tga`, ≤256 KB, ≤256x256).
  - It is read through `TLua_ModImageBGRA` (confined read, client realm first, then server).
  - The UI bridge resolves the `mod:<id>/<file>` reference through a `// Touch:` hook in `touch_uib_img.c`.
  - A mod without an icon gets no picture.
- **Web.** `web/mods.js` only hands the zip to `lua_mod_install`, so the same engine rules apply there.

## Done and verified (on head `fee7b12` + this file)

Builds (all clean; no new warnings in Lua or bridge files):

| Build | Command |
|---|---|
| SDL engine and QC | `xonotic_compile_qc_only && xonotic_compile_engine_only` |
| Dedicated server | `make sv-release` |
| Web engine (5.1 MB wasm) | `scripts/build-web.sh --engine-only` |

Tests: every suite passed, run pinned with `taskset -c 16-23` (setup and exact commands below).

| Suite | Result | New checks |
|---|---|---|
| `scripts/test-mods.sh` | 111/111 | undeclared-tier negative self-test; panel_clock icon read (64x64) and present in its `_cl_lua_mods` row; hit_feedback has no icon (`icon: none`); install review lists both tiers vampire declares |
| `scripts/test-lua-sandbox.sh` | all passed | undeclared call denied with the grant bit set by hand; undeclared grant refused; manifest-less folder cannot be raised and is listed as declaring nothing; `enable ... with`; live revoke; update keeps grants and lists the new tier as undecided; `decline`; `with keep` |
| `scripts/test-lua-packages.sh` | 69 ok | zip without mod.txt refused; update keeps the internet grant and asks about engine_full only |
| `scripts/test-lua-match.sh` | all passed | |
| `scripts/test-uibridge.sh --engine` | all passed | |
| `scripts/test-old-name.sh` | PASS | |
| `scripts/test-web-mods.sh` | 8 passed | |

CI on `work/luaperm` at `fee7b12`: Tests success, Android success.

Coordinator's tablet bugs:
- **(a) Truncation warnings.** Fixed in `fee7b12`:
  - The `tlua_uib_key` and `VM_CL_touch_lua_clevent` warnings came from intentional prefix copies through `dp_strlcpy`.
  - These now go through `TLua_CopyN` (touch_lua_int.h), as do four similar copies in chan, ui and pkg.
- **(b) Install review listed only "standard" for vampire.** Not reproduced in the engine:
  - `_lua_install` contains `perm\tstandard\task\t...` and `perm\tgamerules\task\tchanges damage and players' health`, both with this branch and with the main-era manifest. test-mods now asserts it.
  - The likely cause is the Android dialog (FrontendHost.kt `onMods`) cutting a long body. The standard tier's default text used to be about 170 characters; it is now one short line.
  - If it still happens, check how that dialog was reached and whether it has a max-lines limit.
- **(d) Lua work behind a covered native screen.** `TouchFE_SkipDraw` (androidui2) calls only `TLua_InitOnce` and `TLua_FrameStart`. That skips:
  - loading or unloading client mods
  - timers, coroutines, the frame, map and menu events, and channels
  - unloading server mods when their server stops

  This branch adds `TouchLua_ClientTick()` (touch_lua.h): the whole client frame without drawing. `TouchFE_SkipDraw` should call it instead of the two calls (a two-line change in `touch_frontend.c` after androidui2 lands).

## Not done

- **Merge with main after androidui2.**
  - `origin/work/androidui2` (663f9cb) was not on main yet. A test merge conflicts in `engine/darkplaces/touch_lua.c` `tlua_publish_detail`: it added a `fromlink` line, and this branch added declares/unseen/icon/used lines.
  - Resolve by keeping both. Also drop the duplicate forward declaration of `tlua_fromurl` if the compiler complains (it should not).
  - Then switch `TouchFE_SkipDraw` to `TouchLua_ClientTick()` and rerun the suites.
- **Folder rescans behind the native screen.** The Mods-folder rescan every 2 s runs only while `key_dest` is the menu (`tlua_cl_frame`). It does not run while a native Mods screen is open. Installs rescan by themselves; folders copied in by hand appear after the next rescan.
- **check-mod-docs.py usage detection is a heuristic** (regexes on the Lua source). The engine's per-call check is the real enforcement.
- Not run on a device.

## Known bugs

None known in this work. Warnings left in other people's files (web build: touch_uib_store.c, touch_uib.c, touch_uib_stream.c; touch_lua_clapi.c `tlua_cl_hello_signon` and touch_lua_api.c `tlua_http_local` set but not used) were there before this branch.

## Exact test commands

```
cd <clone>
export PATH=~/.cache/xt-assets/bin:$PATH DP_LINK_JPEG=dlopen
. scripts/lib/xonotic-shlib.sh && xonotic_compile_qc_only && xonotic_compile_engine_only
R=$PWD; export LD_LIBRARY_PATH=~/.cache/godnotic-slimtest/lib
export BASEDIR=~/.cache/godnotic-work/luaperm-base ENGINE=$R/engine/darkplaces/darkplaces-sdl
taskset -c 16-23 $R/scripts/test-mods.sh
taskset -c 16-23 $R/scripts/test-lua-sandbox.sh
taskset -c 16-23 $R/scripts/test-lua-packages.sh
taskset -c 16-23 $R/scripts/test-lua-match.sh
UIB_BASEDIR=$BASEDIR UIB_ENGINE=$ENGINE UIB_LIBDIR=$LD_LIBRARY_PATH taskset -c 16-23 $R/scripts/test-uibridge.sh --engine
$R/scripts/test-old-name.sh; $R/scripts/test-web-mods.sh
(cd engine/darkplaces && make sv-release); scripts/build-web.sh --engine-only
```

Notes:
- The basedir holds pk3 symlinks plus `data/zzz-luaperm-data.pk3dir`, which points at the clone's `engine/data/xonotic-data.pk3dir`.
- `UIB_ENGINE` must be an absolute path.

## For the UI agents (node and cell names)

### Engine data

| Data | Fields |
|---|---|
| `_cl_lua_mods` / `_sv_lua_plugins` rows | `name<TAB>state<TAB>title<TAB>error<TAB>description<TAB>icon`; `icon` (6th field) is `mod:<id>/<file>` or empty |
| `_cl_lua_permissions` rows | `name<TAB>mode<TAB>declared<TAB>granted<TAB>not decided yet`; mode is `v1`, `legacy`, `invalid` or `none` (no mod.txt) |
| `_lua_install` (install review) | one `perm<TAB><tier><TAB>ask\|kept\|new<TAB><reason>` line per declared tier, standard included |

`_lua_detail` keys added by this branch:

| Key | Value |
|---|---|
| `declares` | comma list of tiers, or `nothing` |
| `unseen` | comma list of tiers not decided yet |
| `icon` | `mod:<id>/<file>`, only when present |
| `used` | one line per tier: `used<TAB><tier><TAB><N> calls` |
| `hosts` | hosts contacted |
| `engine_commands` | number of `xt.engine.cmd` calls |

The existing keys are unchanged: `perm<TAB><tier><TAB>0|1<TAB><reason>` (declared tiers only), `fromlink` (androidui2), title, state, version, author, description, error, enabled.

### `touch_uib_settings.qc`

**Screens agent: `UIB_ModRows`.** Pass `argv(5)` as the third argument of `uib_row(name, cells, image)`, giving the row-level `image` from the schema, and only when it is non-empty. Do not use an `icon` cell: column type `icon` is a glyph name and cannot carry a `mod:` reference.

**Android agent: `UIB_BuildModDetail` / `mod.*` (androidui2 a5e4da2).** It already:
- shows only declared tiers as live switches bound to `vm:mods.perm.<tier>`
- sets the preview `image` only when `icon` is present
- shows a turn-on confirmation

Remaining:
1. Make `mod.enable` run `<prefix>enable <sel> with keep` instead of a plain enable. The switches have already granted what the player chose, and `keep` marks the rest decided, so the engine stops listing them as "not decided yet".
2. Mark tiers listed in `unseen` as new on their switch, e.g. by appending a "New: not decided yet" note to the switch's `help`.
3. When `declares` is `nothing`, show "Declares nothing (no mod.txt): standard only".
4. Optionally add a "What it used" section from `used`, `hosts` and `engine_commands` (text nodes).

The "Pages from mods" cards use the glyph `"icon":"mods"`, not a picture, so they can stay as they are.
