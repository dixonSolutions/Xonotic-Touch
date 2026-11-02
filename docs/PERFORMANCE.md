# Performance on phones and tablets

Where a frame's time goes on a phone or tablet, what the port changes about
it, and how to measure it again.

## How it was measured

Every number below comes from one recorded match, played back with
`timedemo`, so each run draws exactly the same frames:

- Stormkeep, 8 players (7 bots, skill 5), 42 seconds, the camera following a
  bot through the fights, the touch HUD on (`vid_touchscreen_mode 2`).
- Recorded with `cl_autodemo 1`. Played back with `+csqc_usedemoprogs 0`, so
  the client QuakeC under test is the one that runs, not the copy stored in
  the demo.
- `-benchmark demos/bench +vid_vsync 0 +cl_maxfps 0` runs it as fast as the
  machine allows and writes `benchmark.log`.

The desktop runs use the GLES2 renderer that Android uses (the desktop
GLES2 build from [TESTING.md](TESTING.md)), offscreen on Mesa. A desktop CPU
is several times faster than a phone's, so at phone resolutions those runs
measure the CPU side. At 3840×2160 the desktop test GPU (Intel UHD 770) runs
out of fill rate first, the way a phone GPU does at its own resolution, so
those runs measure the GPU side.

The Android runs are the APK in a BlissOS VM (Android 13, virgl on the same GPU,
1280×800, 4 cores, 4 GB): the x86_64 build, and the arm64-v8a build running
through Android's ARM translation.

## Results

| Run | Before | After |
|---|---|---|
| Desktop, GLES2, 3840×2160 (GPU-bound) | 135 fps | 295 fps |
| Desktop, GLES2, 1280×720 (CPU-bound) | 344 fps | 394 fps |
| Desktop, GL 3.2 (Flatpak renderer), 2400×1080 | 258 fps | 338 fps |
| Android VM, x86_64 APK | 193 fps (min 152) | 210 fps (min 173) |
| Android VM, arm64 APK under ARM translation | 118 fps | 122 fps |
| Client QuakeC statements per frame, live 8-player bot match | 74 800 | 47 700 |

What each render setting is worth when the GPU is the limit (3840×2160,
balanced otherwise):

| Change | fps |
|---|---|
| balanced before this work | 100 |
| 3D view at 1080 rows (a quarter of the pixels) | 232 |
| bump-mapped world lighting off (`r_glsl_deluxemapping 0`) | 214 |
| specular off (`r_shadow_gloss 0`) | 163 |
| water and portal reflections off (`r_water 0`) | 131 |

## What changed

### The 3D view is drawn at a phone's fill rate, not its panel's

A 1080p phone panel has 2.6 million pixels and a 1600p tablet 4 million; a
mid-range phone GPU cannot shade Xonotic's world at 60 fps on that many.
`r_viewscale_maxheight` (engine, `gl_rmain.c`) draws the 3D view at no more
than that many rows and scales it up to the screen. The HUD, the menus and
the touch controls are drawn after it, at full resolution, so text and
controls stay sharp. One value suits every panel: a 720-row screen is not
scaled, a 1080-row one draws 44% of its pixels, a 1600-row one 20%.

| Profile | Rows |
|---|---|
| battery (Android low tier) | 540 |
| balanced (Android default) | 720 |
| balanced on the Android high tier | 1080 |
| thermal (Linux default, desktops included) | 720 |
| quality | full resolution |

It multiplies with the menu's own resolution scale (`r_viewscale`), so that
slider keeps working.

### Depth and stencil are not written back to memory

Phone GPUs render the screen in tiles held on the chip, and write each tile
out to memory when the pass ends. Depth and stencil are never read after the
pass, but unless the driver is told so it writes them out anyway. The engine
now discards them (`R_Mesh_InvalidateDepthStencil`, `gl_backend.c`) after
the scaled 3D view, after each water, mirror and portal view, and before the
buffer swap. That is memory bandwidth, which on a phone is battery. ES 3
uses `glInvalidateFramebuffer`, ES 2 `GL_EXT_discard_framebuffer`;
`-noinvalidateframebuffer` turns it off.

### Less per-pixel work

`touch/profiles/render-lite.cfg`, exec'd by battery, balanced and thermal:

- `r_glsl_deluxemapping 0`: bump-mapped world lighting samples a normal map
  and a light-direction map for every pixel. The lightmaps still light the
  world.
- `hud_postprocessing 0`: no full-screen blur pass on damage, powerups or
  under water.
- `r_subdivisions_tolerance 8`, shorter particle and decal draw distances.

Battery and thermal also turn specular off (`r_shadow_gloss 0`). Quality
turns all of it back on, but keeps realtime world lights off: they render
the scene once more per light, with shadows.

### Less work on the CPU every frame

Most of a frame's CPU time is the client QuakeC (the HUD, the player models,
the touch controls) and the builtins it calls.

- **Player animation.** Xonotic builds each player's skeleton in QuakeC every
  frame to blend upper and lower body (`skeleton_from_frames`), about a
  quarter of the client QuakeC in a bot match. With `cl_lod_skeletonblend 0`
  a player far enough away to use its farthest LOD model animates as one body
  instead. `cl_playerdetailreduction 6` (Xonotic: 4) moves the LOD distances
  in a little, so distant players also cost fewer vertices to skin.
- **Race checkpoints.** The checkpoints panel walked all 256 checkpoint slots
  through QuakeC's array accessor every frame in every game mode, about 7 000
  statements a frame. It now skips the walk when no split is stored, which
  is always outside race and CTS.
- **`cvar()`.** The HUDs call it a couple of hundred times a frame. It copied
  the name into a 16 KB buffer, looked the cvar up twice and parsed its value
  from the string. It now looks it up once and returns the value the cvar
  already holds (`prvm_cmds.c`). `Touch_UISize` reads its two cvars once per
  frame instead of on every call.
- **Rounded shapes.** The touch controls are drawn as geometry, a few hundred
  vertices each, and every vertex went through a hash search for a duplicate
  that never exists. They are appended directly now (`Mod_Mesh_AddVertex`),
  and the corner directions are computed once per shape instead of once per
  corner. The controls draw pixel for pixel the same.
- **Menu scene swap.** The menu's scene was swapped in and out around its
  draw call every frame, menu open or not, copying 256 full light records
  each way. Only the lights in use are copied now.
- `cl_casings 0`: spent bullet casings each ran QuakeC physics every frame.

### Existing installs get it too

The launchers apply a performance profile once, on the first start, and
record that in `touch_perf_version`. Version 2 is this work. An install
already on version 1 gets `render-lite.cfg` and the resolution cap for its
tier once, on its next start, and keeps its frame cap and texture size,
which the player may have changed. Android: `DeviceProfile.upgradeProfileCommands`.
Linux: `packaging/start.sh`.

## Measuring it again

```bash
# Record (from a cfg exec'd at start): 8 players, spectate, follow a bot.
cl_autodemo 1; cl_autodemo_nameformat bench; cl_welcome 0
minplayers 8; bot_number 7; g_warmup 0; map stormkeep
defer 7 "cmd spectate"; defer 9 "+fire"; defer 9.2 "-fire"
defer 52 "disconnect"
```

```bash
# Play back as fast as the machine allows.
darkplaces-sdl -xonotic ... +exec touch/profiles/balanced.cfg \
  +vid_vsync 0 +cl_maxfps 0 +csqc_usedemoprogs 0 -benchmark demos/bench
```

On a phone, put the demo in `<basedir>/userdata/data/demos/` and the last
line's arguments in `xonotic.args` ([ANDROID.md](ANDROID.md), Debugging on a
device). `prvm_timeprofiling 1` and `prvm_profile client 60` during a match
list where the client QuakeC spends its time; `r_speeds 1` shows draw calls
and render target sizes on screen.
