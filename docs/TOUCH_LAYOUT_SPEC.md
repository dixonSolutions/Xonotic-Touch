# Touch layout & feel spec — Surface Pro 4 (thermal target)

Design spec for the in-game touch layer. **No code here**; this is the contract a coding agent
implements in `engine/data/xonotic-data.pk3dir/qcsrc/client/touch_*.qc`.

Supersedes the layout/feel numbers in [CONTROLS.md](CONTROLS.md) §"Default: Arena touch" and
§"Feel"; CONTROLS.md must be updated to point here once implemented.

Reference device (from [test-runs/2026-07-25-ultramarine](test-runs/2026-07-25-ultramarine/FINDINGS.md)):
Surface Pro 4 class, fanless, GNOME Wayland, Mesa Iris, panel 2880×1920 @120 Hz, GNOME scale 2,
**game renders 960×640 fullscreen** (`vid_conwidth`/`vid_conheight` pinned to the same values),
`fps_max 30`, `vid_vsync 1`, perf profile `thermal`.

---

## 0. Target-device math (use these constants when reasoning about sizes)

| Quantity | Value |
|---|---|
| Render size | 960 × 640 (3:2) |
| `Touch_ScreenMin()` | 640 |
| Physical panel | 260.0 × 173.3 mm (12.3" 3:2) |
| mm per render pixel | **0.2708 mm** (identical both axes) |
| Widget diameter | `640 × nsize × touch_scale` px = **`173.3 × nsize` mm** |
| Comfortable thumb target | ≥ 12 mm ⇒ `nsize ≥ 0.070` |
| Minimum any target | ≥ 9 mm ⇒ `nsize ≥ 0.052` |

Every size below is quoted in px **and** mm at this configuration so a reviewer can sanity-check
Fitts targets without running the game.

---

## 1. Defect inventory — why the previous look/fire attempt corrupted

These are the root causes the implementation must remove. Do not "tune around" them.

| # | Defect | Location today | Effect |
|---|---|---|---|
| D1 | Look delta applied **twice**: once from polled finger positions in `Touch_Frame`, once from pointer-motion events | `touch_input.qc` `Touch_Frame` + `Touch_InputEvent` (`bInputType == 2`) | Double/uneven turn rate, direction fights itself under Wayland pointer emulation |
| D2 | Engine mouse-look is still live (`sensitivity`, `m_yaw`, `m_pitch` set by `Touch_ApplySensitivity`) while CSQC also writes `VF_CL_VIEWANGLES` | `touch_init.qc` | Two writers per frame ⇒ jitter and angle snap-back ("corruption") |
| D3 | Smoothing ring buffer writes at `idx & 3` but averages **fixed** slots `0..n-1` | `touch_look.qc` | Averages stale/never-written samples; periodic spikes |
| D4 | Per-axis hard deadzone applied to **every frame's** delta (`fabs(dx) < 6..8 → 0`) | `touch_look.qc` | Slow drags produce nothing, then burst — the classic "sticky then jumpy" feel |
| D5 | First frame after finger-down uses the **previous** finger's last position as `prev_pos` | `touch_input.qc` `Touch_PollFingers` | One-frame giant delta ⇒ view teleport on every touch |
| D6 | Fixed slot mapping `logical i → gettouchfinger(i)`; ids churn when a finger lifts | `touch_input.qc` `Touch_GetFinger` | Roles swap mid-gesture; look finger inherits another finger's position ⇒ teleport |
| D7 | Role cleared whenever a slot reports `z <= 0` for a single frame | `touch_input.qc` | `+attack` / `+forward` released mid-action |
| D8 | `Touch_SetKey` `localcmd`s ~8 `-key` lines **every frame** regardless of state | `touch_input.qc` | 240 console cmds/s (CPU + heat), and it cancels a physically held key (real SPACE / mouse1) 30×/s |
| D9 | Pointer press assigns role from a stale `touch_finger_last_pos[0]` (often `0 0`) | `touch_input.qc` `Touch_InputEvent` | A tap anywhere can be read as the top-left console pad ⇒ console opens instead of firing |
| D10 | `Touch_MirrorX` uses top-left-origin formula `1 - nx - nsize` on **center**-semantics cvars | `touch_layout.qc` | Left-handed layout off by half a widget |
| D11 | `Touch_WidgetScale()` multiplies widget size by `touch_hud_scale` (0.85 in `standard.cfg`) | `touch_layout.qc` | Controls shrink when the player scales the HUD; two unrelated knobs coupled |
| D12 | `Touch_WidgetVisible()` hides anything at `x <= 0.001` / `y <= 0.001` | `touch_layout.qc` | Position doubles as a visibility flag; can't place a widget at the screen edge |
| D13 | Glass discs drawn as per-row scanline fills (3 discs per button, `step = max(2, r/24)`) | `touch_glass.qc` | ~600 `drawfill` per frame; this is what cooked the chassis (hence `touch_simple_draw 1`) |
| D14 | `Touch_SaveLayoutFile` writes to the gamedir (`~/.xonotic/data/touch.layout.cfg`); the launcher `exec`s `${HOME}/.xonotic/touch.layout.cfg` | `touch_config.qc` + `packaging/start.sh:23` | Saved layout is never loaded ⇒ nothing is remembered across sessions |
| D15 | Layout file writes bare `key value`, not `seta` | `touch_config.qc` | Values are not archived by the engine either |
| D16 | Dead band between the move capture zone (`x < 0.40 W`) and `touch_look_zone_left 0.42` | `touch_input.qc` / `standard.cfg` | 19-px vertical strip that swallows touches |
| D17 | `Touch_Frame()` runs at the **end** of `CSQC_UpdateView` (after `CSQCPlayer_SetCamera`) | `view.qc:1878` | Look input is one frame (33 ms) late and races prediction |

---

## 2. Coordinate contract (fix first — everything else depends on it)

1. `touch_<w>_x`, `touch_<w>_y` are the widget **centre**, normalized to `VF_SIZE.x` / `VF_SIZE.y`.
2. `touch_<w>_size` is the **short-axis diameter**, normalized to `Touch_ScreenMin()`.
   Non-circular widgets add `touch_<w>_aspect` (long axis ÷ short axis).
3. Widget geometry scale is `touch_scale` **only**. Remove `touch_hud_scale` from
   `Touch_WidgetScale()` (D11); `touch_hud_scale` stays exclusive to `touch_hud.qc`.
4. Mirror for left-handed is `x' = 1 - x` (centre semantics, D10). `y` never mirrors.
5. Visibility is an explicit `touch_<w>_visible` cvar per widget; position no longer implies
   hidden (D12). Keep `Touch_WidgetVisible()` as a thin wrapper reading the flag.
6. Drop the `eZ` radius smuggled into `Touch_WidgetCenter()`'s return value; callers ask
   `Touch_WidgetRadius()`.
7. **Input-space guard:** finger/pointer coordinates (`gettouchfinger`, `getmousepos`) and draw
   space (`VF_SIZE`) must agree. The launcher pins `vid_conwidth == vid_width`. Implement a
   one-line scale `p *= VF_SIZE / vec(vid_conwidth, vid_conheight)` applied at ingest, so a
   profile that changes `conwidth` degrades gracefully instead of mis-hitting every button.
8. Add `Touch_HitRect(p, nx, ny, nsize, aspect, grab)` for capsule/bar widgets
   (`grab` = hit-box inflation factor). Rect hit tests are cheaper than the circle path and are
   what SPACE and the console handle use.

---

## 3. Layout — exact defaults

Default control set is **5 glass controls** + the stock HUD weapons strip: MOVE, FIRE, JUMP,
CR, CONSOLE. Weapon switching uses the **right-edge weapons panel** (tap icon → `impulse N`);
the old WEP glass button is off by default (Hick's Law; ZOOM / RELOAD / DODGE stay hidden).

### 3.1 Cvar values (right-handed default, `touch_scale 1.0`)

| Widget | `_x` | `_y` | `_size` | `_aspect` | `_visible` | Shape |
|---|---|---|---|---|---|---|
| MOVE  | `0.165` | `0.680` | `0.250` | — | `1` | circle (stick) |
| FIRE  | `0.860` | `0.640` | `0.200` | — | `1` | circle |
| JUMP (space) | `0.860` | `0.860` | `0.110` | `2.6` | `1` | capsule/bar |
| CR (crouch) | `0.700` | `0.860` | `0.100` | — | `1` | circle |
| WEP   | `0.540` | `0.860` | `0.100` | — | `1` | circle |
| CONSOLE | `0.500` | `0.050` | `0.058` | `4.2` | `1` | pill/bar |
| ZOOM  | `0.860` | `0.480` | `0.100` | — | `0` | circle |
| RELOAD| `0.720` | `0.480` | `0.100` | — | `0` | circle |
| DODGE | `0.165` | `0.480` | `0.100` | — | `0` | circle |

> **Draw space:** layout maths use `vid_conwidth` × `vid_conheight` (`Touch_UISize`), not `VF_SIZE`.
> On Wayland, VF can be 1440×960 while con stays 960×640; 2D clips to con — VF-based layout hid FIRE/JUMP.

### 3.2 Resolved geometry at 960×640 (verify these numbers on device)

| Widget | Centre px | Size px | Size mm | Nearest-neighbour gap |
|---|---|---|---|---|
| MOVE | (144, 486) | ⌀141 | 38.1 | 88 px to WEP (23.8 mm) |
| FIRE | (821, 470) | ⌀112 | 30.3 | 66 px to SPACE (18.0 mm) |
| SPACE | (614, 563) | 169 × 77 | 45.7 × 20.8 | 45 px to CR (12.2 mm) |
| CR | (451, 563) | ⌀67 | 18.2 | 48 px to WEP (13.0 mm) |
| WEP | (336, 563) | ⌀67 | 18.2 | — |
| CON | (480, 29) | 102 × 32 | 27.7 × 8.7 | — |

Invariants a reviewer/test can assert:

* Every visible widget's bounding box sits ≥ 20 px (5.4 mm) inside the screen.
* Minimum edge-to-edge gap between any two visible widgets ≥ 40 px (10.8 mm).
* Bottom row (SPACE / CR / WEP) shares one baseline `y = 0.880`; ZOOM / RELOAD / DODGE share
  `y = 0.545` when enabled (Law of Similarity / Continuity: aligned rows read as one group).
* FIRE and MOVE are the two largest targets (Fitts on the two continuous-use controls).
* Nothing overlaps the centre band `y ∈ [0.30, 0.60]`, `x ∈ [0.34, 0.66]` (crosshair / centerprint).

### 3.3 Zones (no dead bands, D16)

| Cvar | Default | Meaning |
|---|---|---|
| `touch_move_zone_w` | `0.32` | Move capture region: `x < 0.32·W` **and** `y > 0.42·H` |
| `touch_look_zone_left` | `0.32` | Must equal `touch_move_zone_w` — look starts exactly where move ends |
| `touch_look_zone_right` | `1.0` | |
| `touch_edge_deadzone_px` | `10` | ≈2.7 mm; CON handle is exempt (evaluated before the deadzone) |

Everything not claimed by CON, a widget, or the move zone falls through to **LOOK**. There is no
`TOUCH_SLOT_NONE` region except the edge deadzone and HUD panels.

### 3.4 Profile file changes

`touch/profiles/standard.cfg` — replace the layout block with §3.1, plus:

```
touch_scale 1.0
touch_opacity 0.90
touch_hud_scale 0.85          // HUD only; no longer scales controls
touch_glass_quality 1
touch_weapon_mode 2           // tap WEP = weapnext; swipe on WEP = prev/next
touch_look_zone_left 0.32
touch_move_zone_w 0.32
touch_edge_deadzone_px 10
touch_sens_base 2.8
touch_sens_y_mult 0.85
touch_look_smoothing 1
touch_look_deadzone_px 4
touch_stick_deadzone 0.18
touch_zoom_visible 0
touch_reload_visible 0
touch_dodge_visible 0
```

`touch/profiles/thermal.cfg` and `battery.cfg` — replace `touch_simple_draw 1` with
`touch_glass_quality 1` (glass is now inside budget; see §8) and keep everything else.
`left.cfg` regenerates from §3.1 via `x' = 1 - x`.

---

## 4. Finger tracking & role assignment

### 4.1 Finger table

Replace the fixed `logical i → gettouchfinger(i)` mapping (D6) with an identity-tracked table:

```
struct TouchFinger {          // TOUCH_MAX_FINGERS = 4 (move + look + fire + spare)
  bool   used;
  int    src;                 // 0..9 engine finger slot, or 10 = pointer/mouse
  int    role;                // TOUCH_ROLE_*
  vector down_pos, last_pos;  // input space, already conwidth-corrected
  float  down_time;
  vector accum;               // motion consumed by the role this frame
  float  lost_time;           // 0 while reported, else time of first miss
  bool   first_frame;         // suppress delta on the frame it was claimed (D5)
}
```

Per frame:

1. Scan engine slots `0..9` (and the pointer, see §4.3). Match each live contact to an existing
   `TouchFinger` **by `src`**. New `src` ⇒ claim a free entry, assign role, set `first_frame`.
2. A slot that reports `z <= 0` is *not* immediately released: keep the entry for
   `touch_finger_grace_ms` (default `120`, i.e. ~4 frames at 30 fps) before freeing it (D7).
   Grace applies to role output too — a held button stays held through the gap.
3. Role is assigned **once, on claim, and never re-evaluated** ("sticky roles"). Sliding a
   finger off FIRE keeps firing; a look drag crossing FIRE never fires.
4. Extra contacts beyond `TOUCH_MAX_FINGERS`, or a second contact for an already-owned exclusive
   role (MOVE, LOOK), get `TOUCH_ROLE_IGNORED` — they must not disturb existing roles.

### 4.2 Priority order (first match wins)

```
1  CON handle rect      (grab 1.20)   → ROLE_CON        // before edge deadzone
2  edge deadzone        → ROLE_IGNORED
3  HUD panel hit        → ROLE_IGNORED
4  FIRE circle          (grab 1.15)   → ROLE_FIRE
5  SPACE rect           (grab 1.10)   → ROLE_JUMP
6  CR / WEP / ZOOM / RELOAD / DODGE (visible only, grab 1.10) → ROLE_*
7  move zone or MOVE circle           → ROLE_MOVE       (if unowned, else IGNORED)
8  look zone                          → ROLE_LOOK       (if unowned, else IGNORED)
9  fallback                           → ROLE_IGNORED
```

Grab factors must not create overlap: assert at init that inflated hit boxes of visible widgets
stay disjoint, and log once if not (a customize-mode user can violate it).

### 4.3 Pointer / Wayland fallback

Wayland frequently delivers touch as pointer events. Rules:

* `Touch_InputEvent` **only records state**: press ⇒ `pointer_down = true` *and*
  `pointer_pos = getmousepos()` sampled at that instant (fixes D9); motion ⇒ update
  `pointer_pos`; release ⇒ `pointer_down = false`. It never touches viewangles (fixes D1).
* If a press arrives before any motion event and `getmousepos()` is exactly `0 0`, ignore the
  press (defensive: that is the stale-origin case that opened the console).
* The pointer is exposed to the finger table as `src = 10`, only while no real
  `gettouchfinger` contact is live, so real multitouch always wins.

### 4.4 Key output (D8)

`Touch_SetKey(key, down)` caches the last sent state per key and `localcmd`s **only on change**.
Additionally keep a `touch_owns_key` bit per key: only emit `-key` for keys this layer pressed, so
a physically held SPACE / mouse1 is never cancelled. `Touch_ReleaseAll()` force-clears the cache
and emits `-key` for owned keys only. Steady-state budget: **0 localcmd/frame**.

---

## 5. Look swipe — algorithm

Owner: `touch_look.qc`. Single writer of `VF_CL_VIEWANGLES` from the touch layer.

### 5.1 Ownership and scheduling

* New cvar `touch_look_owns_view` (default `1`). While `Touch_Active()`, CSQC owns look:
  save and set `sensitivity 0` (engine mouse-look off, kills D2), restore the previous value when
  touch deactivates or on `vid_touchscreen 0`. `m_yaw` / `m_pitch` are left alone (menus use
  absolute pointer). `Touch_ApplySensitivity()` stops writing `sensitivity` in this mode.
* Split the frame hook (D17): `Touch_Frame()` (input, angle write) is called **before**
  `CSQCPlayer_SetCamera()` in `CSQC_UpdateView` (~`view.qc:1720`); `Touch_Draw()` stays after
  `HUD_Draw()`. Exactly one call site each — the duplicated pair is already fixed, keep it that way.
* Timebase: the touch layer keeps its own `dt = bound(1/240, time - touch_prev_time, 0.1)`;
  `drawframetime` is not yet valid at the early hook.

### 5.2 Cvars

| Cvar | Default | Range | Meaning |
|---|---|---|---|
| `touch_sens_base` | `2.8` | 1.0–6.0 | Multiplier on `TOUCH_LOOK_DEG_PER_PX` |
| `touch_sens_y_mult` | `0.85` | 0.5–1.5 | Pitch multiplier |
| `touch_invert_y` | `0` | 0/1 | |
| `touch_look_smoothing` | `1` | 0/1/2 | EMA time constant: `0 / 0.035 s / 0.070 s` |
| `touch_look_deadzone_px` | `4` | 0–12 | **Radial escape** distance from touch-down, once per gesture |
| `touch_look_escape_carry` | `0.5` | 0–1 | Fraction of the escape travel credited on activation |
| `touch_look_max_deg_per_s` | `900` | 300–2000 | Hard clamp; absorbs id churn / teleports |
| `touch_look_glide` | `0` | 0/1 | Off = motion stops the instant the finger lifts (no inertia drift) |

Constant: `TOUCH_LOOK_DEG_PER_PX = 0.18` at a reference render width of 960.
Effective `deg_per_px = 0.18 · touch_sens_base · (960 / VF_SIZE.x)` — normalizing by **render
width**, not DPI, is what makes 960×640 and 1440×960 feel identical. At the default that is
0.504 °/px ⇒ 1.86 °/mm ⇒ 180° per 96 mm of drag.

### 5.3 Pseudocode

```c
// ---- ingest (once per frame, per finger with ROLE_LOOK) ----
if (f.first_frame) { f.accum = '0 0 0'; }              // D5: never use a stale prev_pos
else              { f.accum += f.last_pos - f.prev_pos; }

// ---- Touch_Look_Update(vector raw_px, float dt) ----
if (!look_active) {                                     // finger lifted (or in grace)
    look_vel = '0 0 0';                                 // touch_look_glide 0
    return;
}

if (!look_escaped) {
    // radial, once per gesture — NOT a per-frame per-axis gate (D4)
    if (vlen(f.last_pos - f.down_pos) < touch_look_deadzone_px) return;
    look_escaped = true;
    raw_px = (f.last_pos - f.down_pos) * touch_look_escape_carry;
}

float tau = (float[]){0.0, 0.035, 0.070}[touch_look_smoothing];
vector v_raw = raw_px / dt;                             // px/s — frame-rate independent
float  a     = (tau <= 0) ? 1.0 : 1.0 - exp(-dt / tau); // one-pole EMA, dt-compensated (D3 gone)
look_vel    += (v_raw - look_vel) * a;
vector applied = look_vel * dt;                         // px to consume this frame

float k     = 0.18 * touch_sens_base * (960 / VF_SIZE.x);
float yaw   = applied.x * k;
float pitch = applied.y * k * touch_sens_y_mult * (touch_invert_y ? -1 : 1);

float cap = touch_look_max_deg_per_s * dt;
yaw   = bound(-cap, yaw,   cap);
pitch = bound(-cap, pitch, cap);

// sub-degree residue carried, so slow drags are smooth instead of stair-stepped
look_carry += vec(yaw, pitch);
vector step = look_carry;  look_carry -= step;          // (keep full float precision; carry only
                                                        //  matters if the angle write quantizes)

vector ang = getpropertyvec(VF_CL_VIEWANGLES);
ang.y -= step.x;                                        // drag right => turn right
ang.x += step.y;                                        // drag down  => look down
ang.x  = bound(-89, ang.x, 89);
ang.z  = 0;                                             // never accumulate roll
setproperty(VF_CL_VIEWANGLES, ang);
```

Behavioural requirements that follow:

* A stationary finger produces **exactly zero** angle change (EMA of zero velocity decays to 0).
* Total travel is preserved to within the EMA lag: a 200 px drag turns ~101° at defaults
  regardless of whether it took 3 frames or 30.
* `touch_look_smoothing 0` must be bit-for-bit "raw delta × k" so testers can A/B smoothing.
* Aim assist is out of scope for this pass; `touch_aim_assist` must **not** scale yaw/pitch down
  (today it silently reduces sensitivity, which reads as "sluggish"). Force it to `0` in profiles
  until a real magnetism implementation lands.

---

## 6. FIRE reliability rules

Requirement: hold = `+attack`, released only when the player lifts.

| Rule | Detail |
|---|---|
| R1 | FIRE is priority 4, above SPACE/CR/WEP and far above LOOK — a touch inside the inflated FIRE box can never be claimed by look. |
| R2 | Sticky role: the owning finger keeps `ROLE_FIRE` for its whole life. Sliding out of the circle keeps `+attack` down (`touch_fire_slide_release 0`, default). |
| R3 | Ref-counted: `fire_down = (count of live fingers with ROLE_FIRE) > 0`. A second finger landing on FIRE is additive, and lifting it does not release. |
| R4 | Grace: a FIRE finger whose slot reports `z <= 0` for < `touch_finger_grace_ms` (120 ms) stays held. This is the fix for mid-burst dropouts from finger-id churn. |
| R5 | Edge-triggered output: `+attack` emitted once on the rising edge, `-attack` once on the falling edge (§4.4). Never re-emitted per frame. |
| R6 | `touch_fire_mode 1` (toggle) flips state **only** on the finger-down edge, never while held. `touch_fire_toggle_state` is force-cleared whenever `touch_fire_mode` changes to 0, so hold mode can't inherit a stuck toggle. |
| R7 | Hard release conditions (all force `-attack` + clear toggle): `!Touch_Active()`, menu alpha ≥ 1, customize mode, scoreboard open, intermission, map change, `vid_touchscreen 0`. |
| R8 | Look-zone interaction: FIRE and LOOK are independent fingers; firing while turning is the default case and must be tested explicitly (§10-T7). A single finger can never be both. |
| R9 | Pointer fallback: with only a pointer available, a press inside FIRE claims `src = 10` as `ROLE_FIRE`; motion does not reassign it (this is what made single-touch/Wayland taps unreliable). |
| R10 | `touch_auto_fire` remains `0`; it must OR into the output *after* the edge tracker so it doesn't corrupt the cached key state. |

---

## 7. SPACE / JUMP control

* Shape: capsule/bar (`touch_jump_aspect 2.2`, 169 × 77 px, 45.7 × 20.8 mm) — deliberately
  spacebar-shaped so the affordance is legible (Jakob's Law), label `SPACE`, sublabel `JUMP` at
  0.6× when `touch_glass_quality ≥ 1`.
* Behaviour: **hold = `+jump`** (needed for Xonotic bunny-hop / ramp-jumps), not tap-pulse.
  Replace today's one-frame `touch_btn_jump` pulse with a held state driven by finger liveness,
  same grace and edge rules as FIRE (R2–R5).
* Keyboard SPACE must keep working: `touch_owns_key` (§4.4) guarantees the touch layer never
  emits `-jump` for a keypress it didn't own.
* Double-tap on SPACE within `touch_jump_doubletap_ms` (`250`) while `touch_dodge_mode 1` fires
  the dodge; default `touch_dodge_mode 0` ignores it.

---

## 8. Console handle — drag & drop state machine

### 8.1 Cvars (all archived, all written to the layout file)

| Cvar | Default | Meaning |
|---|---|---|
| `touch_con_visible` | `1` | Draw + hit-test the handle |
| `touch_con_x` | `0.500` | Handle centre X (top-middle default) |
| `touch_con_y` | `0.045` | Handle centre Y |
| `touch_con_size` | `0.050` | Short axis (32 px / 8.7 mm) |
| `touch_con_aspect` | `3.2` | ⇒ 102 px / 27.7 mm long axis |
| `touch_con_hold_ms` | `350` | Hold time before drag arms |
| `touch_con_tap_slop_px` | `10` | Max movement still counted as a tap / still arming |
| `touch_con_snap` | `0.005` | Position quantization on drop |
| `touch_con_margin_px` | `4` | Minimum on-screen margin when clamped |
| `touch_con_lock` | `0` | `1` = position frozen (drag disabled, tap still toggles) |

### 8.2 State machine

```
IDLE
 └─ finger down inside handle rect (grab 1.20) ─────────────► ARMING
      t0 = time, p0 = pos, grab_offset = centre_px - p0

ARMING                              (visual: handle brightens to accent 0.65)
 ├─ release  &&  time-t0 <  hold_ms  &&  |p-p0| <= slop ───► TAP    → toggleconsole ; IDLE
 ├─ |p-p0| > slop  &&  time-t0 < hold_ms ──────────────────► CANCEL → role becomes IGNORED
 │                                                            (a look swipe across the top
 │                                                             centre must never toggle console)
 ├─ time-t0 >= hold_ms && !touch_con_lock ─────────────────► DRAGGING
 │                                                            (visual: outline pulse, 1 extra fill)
 └─ release && time-t0 >= hold_ms ─────────────────────────► IDLE (armed but never moved: no-op,
                                                              explicitly NOT a console toggle)

DRAGGING
 ├─ motion ────► centre_px = p + grab_offset ; clamp ; live-preview only (no cvar writes)
 └─ release ───► DROP

DROP
 ├─ nx = quantize(clamp_x(centre.x)/W, touch_con_snap)
 ├─ if |nx - 0.5| < 0.02 → nx = 0.5                    // magnet back to centre
 ├─ ny = quantize(clamp_y(centre.y)/H, touch_con_snap)
 ├─ cvar_set touch_con_x / touch_con_y
 ├─ schedule debounced persist (see §8.4)
 └─ IDLE  (visual: accent flash 0.25 s)
```

Clamp (keeps the whole bar reachable, `m = touch_con_margin_px`):

```
half_w = ScreenMin*size*aspect*0.5*touch_scale ;  half_h = ScreenMin*size*0.5*touch_scale
nx ∈ [ (half_w + m)/W , 1 - (half_w + m)/W ]
ny ∈ [ (half_h + m)/H , 1 - (half_h + m)/H ]
```

Additional constraint: on DROP, if the clamped rect overlaps the inflated hit box of any visible
control, nudge `ny` (then `nx`) by the smallest amount that clears it; if impossible, reject the
drop and restore the previous position with a red flash. Priority must never be gameable into
"the console handle sits on top of FIRE".

### 8.3 Interaction with roles

`ROLE_CON` is priority 1 and exempt from the edge deadzone, so the handle works at
`y = 0.045` even with `touch_edge_deadzone_px 10`. While `DRAGGING`, all other roles keep
working (you can drag the handle while moving); the drag finger emits no keys.

### 8.4 Persistence (fixes D14 / D15)

1. `Touch_SaveLayoutFile()` writes `seta <cvar> <value>` lines (D15) and gains the whole
   `touch_con_*` block plus the new `touch_*_visible`, `touch_*_aspect`, `touch_move_zone_w`,
   `touch_look_*`, `touch_finger_grace_ms`, `touch_glass_quality`, `touch_look_owns_view`.
2. Debounced auto-save: on DROP, arm a `touch_autosave_at = time + 1.0`; the first frame past it
   writes the file **once**. Never more than one `fopen` per second, never during a drag.
3. Path unification: the file lives at the CSQC write dir, i.e. `~/.xonotic/data/touch.layout.cfg`.
   `packaging/start.sh` and `scripts/lib/xonotic-shlib.sh` must emit
   `exec touch.layout.cfg` (engine-resolved, last line of `touch/startup.cfg`, after the profiles)
   and keep the absolute `${HOME}/.xonotic/touch.layout.cfg` line only as a legacy fallback.
   `XONOTIC_TOUCH_LAYOUT` default changes to `${HOME}/.xonotic/data/touch.layout.cfg`.
4. Because `registercvar` does not set `CF_ARCHIVE`, the `seta` lines in the layout file are the
   authoritative persistence path. Load order guarantee: profiles first, layout file last, so a
   player's dragged console position survives a profile re-exec.
5. First-run/migration: if `touch_con_x` is unset/`0`, initialize to the §8.1 defaults *before*
   the first draw, so no build ever shows a handle at `0 0`.

---

## 9. Glass draw budget

Goal: real glass look, `touch_simple_draw` retired, **no scanline circles** (D13).

### 9.1 Quality levels

`touch_glass_quality` replaces `touch_simple_draw` (read the old cvar once for migration):

| Value | Look | Budget |
|---|---|---|
| `0` | flat: shadow + body only | ≤ 14 fills |
| `1` | **default** glass: shadow + body + chamfers + top highlight | ≤ 34 fills |
| `2` | glass + weapon wheel + stick ticks | ≤ 60 fills (not for fanless) |

### 9.2 Recipe — rounded widget ("chamfer trio", 5 fills, no per-row loops)

A convincing rounded-square glass button from axis-aligned `drawfill` only:

1. **Shadow** — body rect offset `(+1, +2)`, `TOUCH_GLASS_SHADOW`, `α·0.18`.
2. **Body** — full rect, `TOUCH_GLASS_TINT`, `α·0.30`.
3. **Chamfer V** — rect inset `12%` on X, full height, same tint, `α·0.14` (darkens/rounds sides).
4. **Chamfer H** — rect inset `12%` on Y, full width, same tint, `α·0.14`.
5. **Specular** — top 38% of the body inset 10% on X, `TOUCH_GLASS_HIGHLIGHT`, `α·0.10`.

Pressed state adds **one** fill: inner rect at 55% size, `TOUCH_GLASS_ACCENT`, `α·0.14`, plus a
`(0, +2)` px body offset. Steps 3–4 are what read as "rounded glass" at 90% opacity — verify
against `screenshots/` rather than by argument.

Bar/capsule (SPACE, CON): steps 1, 2, 5 only = **3 fills** (+1 pressed).

Move stick: base (5) + two 1-px cross ticks (2) + knob (3: shadow, body, specular) = **10 fills**.

### 9.3 Per-frame budget (hard limits)

| Item | Limit |
|---|---|
| `drawfill` calls, steady state | **≤ 34** |
| `drawfill` calls, worst case (all pressed + drag preview) | **≤ 40** |
| `drawstring` calls | **≤ 7** (5 widget labels + SPACE sublabel + optional debug line) |
| `drawpic` calls | **0** in the control overlay (HUD panels excluded) |
| Full-screen fills | `0` in gameplay; `1` allowed in customize mode only |
| Union of widget area | ≤ 10% of framebuffer (§3.2 sums to 9.4%) |
| Blended pixels | ≤ **0.30 × framebuffer** per frame (≈184 k px at 960×640) |
| Per-frame allocations | `0` `strcat`/`ftos` in the draw path — labels are constants, geometry is cached |
| `getpropertyvec`/`cvar` reads | Cache widget geometry in a static table; recompute only when `VF_SIZE` or a `touch_*` cvar generation counter changes (≤ 1 rebuild per cvar change, not per widget per frame) |

Forbidden: per-row/per-column loops, `drawsetcliparea` churn, generated textures,
`drawrotpic`, anything that scales with radius.

---

## 10. Thermal invariants (must not regress)

| # | Invariant | How it is checked |
|---|---|---|
| C1 | Render 960×640, `vid_conwidth`/`vid_conheight` equal to it, `vid_fullscreen 1` | `cvar vid_width` etc. in console screenshot |
| C2 | `fps_max 30`, `vid_vsync 1` | `showfps 1` reads 30 ± 1 while playing |
| C3 | Touch overlay ≤ 40 `drawfill` + ≤ 7 `drawstring` per frame | `touch_debug 1` counter (§10.1) |
| C4 | ≤ 1 CSQC `localcmd` per second in steady state (no input) | `touch_debug 1` cmd/s counter |
| C5 | No `fopen` during gameplay except the debounced layout save (≥ 1 s apart) | code review + debug counter |
| C6 | No `search_begin` / glob / model-parameter scan per frame (portrait HUD stays cached) | code review |
| C7 | `prvm_traceqc 0`, `prvm_statementprofiling 0`, `prvm_timeprofiling 0`, `developer 0` in every launch path | `touch/startup.cfg` + screenshot |
| C8 | thermal profile effects stay off: particles, decals, shadows, coronas, bloom, motionblur, water, dynamic, glsl post/deluxe/offset, viewmodel; `r_picmip*world/sprites 3`; `gl_texturecompression 1` | `diff` against `touch/profiles/thermal.cfg` |
| C9 | `sensitivity 0` while touch owns look (engine mouse-look path disabled) — correctness *and* CPU | `cvar sensitivity` in-game shows 0; restored to prior value after `vid_touchscreen 0` |
| C10 | Touch layer CPU ≤ 1.5 ms and GPU ≤ 0.8 ms per frame | `prvm_profile csqc` before/after; frame time delta with `touch_con_visible 0`+all widgets hidden vs default |
| C11 | Launcher keeps `powerprofilesctl set power-saver` | `powerprofilesctl get` |
| C12 | 20-minute session: sustained 30 fps, no `% lost` growth, chassis still hand-holdable | manual + `status` |

### 10.1 `touch_debug` overlay (required for screenshot-only verification)

Because remote verification happens through in-engine `screenshot` (no interactive access), add
`touch_debug` (default `0`, one `drawstring` when `1`) printing a single line:

```
f0:LOOK f1:FIRE f2:- f3:-  dt 33.2ms  vel 412px/s  fills 29  str 6  cmd/s 0  con 0.500,0.045
```

This makes every acceptance item below assertable from a PNG.

---

## 11. Acceptance checklist (remote Ultramarine)

Harness: `docs/test-runs/<date>-ultramarine/remote-test.sh` pattern — launch the Flatpak or
`run-fixed.sh`, drive with `ydotool` (physical coords = logical × 2), capture with in-engine
`screenshot`, pull TGA over SCP, convert to PNG. Record results in a new
`docs/test-runs/<date>-ultramarine/FINDINGS.md`. **Tablet must be held landscape.**

Setup: `touch_debug 1`, `showfps 1`, load `courtfun` (or any loaded map), join.

| # | Test | Pass criteria |
|---|---|---|
| T1 | Layout screenshot vs §3.2 | 6 controls visible; centres within ±3 px of the table; no overlaps; ≥ 20 px screen margins |
| T2 | Glass look | Rounded glass reads correctly at `touch_opacity 0.90`; `fills ≤ 34`, `str ≤ 7` in the debug line |
| T3 | Move | Finger in bottom-left drives `+forward/+back/+moveleft/+moveright` correctly (forward = finger *above* stick centre); knob follows; releasing stops movement within 1 frame |
| T4 | Look smoothness | Slow 30 mm drag turns smoothly with no stalls or steps; `vel` in debug rises and decays smoothly; no view snap on touch-down (D5) or touch-up |
| T5 | Look magnitude | 100 mm horizontal drag ⇒ 180° ± 15° at `touch_sens_base 2.8`; same result at `fps_max 30` and `fps_max 60` (frame-rate independence) |
| T6 | Look isolation | With `touch_look_owns_view 1`, `cvar sensitivity` = 0; no double-speed turning under Wayland pointer emulation; `ang.z` stays 0 (no roll) |
| T7 | Fire + look together | Hold FIRE with the right thumb while dragging look with a second finger: continuous fire for 10 s, no dropouts; weapon keeps firing when the fire thumb slides 20 mm off the button |
| T8 | Fire reliability, 30 taps | 30/30 taps produce a shot; zero taps open the console or move the view |
| T9 | Fire release | Lifting always stops fire within 1 frame; grace never leaves `+attack` stuck (check `status`/no runaway ammo) |
| T10 | SPACE hold | Holding SPACE gives repeated/held jumps (bunny-hop viable); tap gives exactly one jump; a physical spacebar still jumps while the touch layer is active |
| T11 | Console tap | Short tap on the top-middle handle toggles the console; `con_closeontoggle 1` closes it on the next tap |
| T12 | Console swipe immunity | A fast look swipe across the top-centre (through the handle) does **not** toggle the console (ARMING→CANCEL) |
| T13 | Console hold-drag | Hold 350 ms (handle brightens) then drag to bottom-right; handle follows the finger; release snaps to the grid and flashes accent |
| T14 | Console clamp | Attempt to drag off each of the 4 edges: the whole bar stays on screen with ≥ 4 px margin; attempt to drop on FIRE: nudged clear or rejected with a red flash |
| T15 | Console persistence | After the drop, `~/.xonotic/data/touch.layout.cfg` contains `seta touch_con_x/…_y`; quit; relaunch via `packaging/start.sh`; handle reappears at the dragged position (this is the D14 regression test) |
| T16 | Profile re-exec | `exec touch/profiles/standard.cfg` then `exec touch.layout.cfg` leaves the dragged position intact (load-order guarantee) |
| T17 | Left-handed | `touch_handedness 1` mirrors every centre as `1 - x` (MOVE lands bottom-right, FIRE bottom-left); look zone mirrors; no widget lands off-screen |
| T18 | Zone continuity | Slow horizontal probe along `y = 0.60` from `x = 0.30` to `0.40`: role transitions MOVE→LOOK with no ignored strip (D16) |
| T19 | Thermal — cmd flood | Standing still with no touches: `cmd/s 0` in the debug line for 10 s (D8 regression test) |
| T20 | Thermal — sustained | 20-minute bot match: `showfps` holds 30, no thermal-induced stutter, `powerprofilesctl get` = `power-saver`, chassis hand-holdable; screenshot at 0/10/20 min |
| T21 | Draw-cost delta | Frame time with `touch_glass_quality 1` vs all controls hidden: ≤ 2.3 ms difference (C10) |
| T22 | Clean logs | No new engine warnings; specifically `VM_drawstring: z value from pos discarded` must be gone (pass explicit 2D positions with `z = 0`) |
| T23 | Suspend/restore | Menu open → close, map change, and `vid_touchscreen 0/1` all release keys (no stuck `+attack`/`+forward`) and restore `sensitivity` |

---

## 12. Work order for the implementing agent

Each step must compile (`./scripts/…` QC build) and be committed separately.

1. **Coordinate contract** (§2): `touch_layout.qc` scale/mirror/visibility fixes, `Touch_HitRect`,
   input-space guard. Touches D10–D12.
2. **Key output + finger table** (§4): `touch_input.qc` rewrite of `Touch_GetFinger`,
   `Touch_PollFingers`, `Touch_SetKey`. Touches D5–D9.
3. **Look pipeline** (§5): `touch_look.qc` EMA + escape deadzone + clamp; `touch_init.qc`
   ownership of `sensitivity`; `view.qc` split hook. Touches D1–D4, D17.
4. **Fire/SPACE semantics** (§6, §7): held jump, ref-counted fire, hard-release list.
5. **Glass** (§9): `touch_glass.qc` chamfer recipe, delete scanline paths, geometry cache,
   `touch_glass_quality`. Touches D13.
6. **Console handle** (§8): new state machine module (extend `touch_customize.qc` or a small
   `touch_console.qc`), cvars, clamp, debounced save.
7. **Persistence + launcher** (§8.4): `touch_config.qc` `seta` writer + new cvar block;
   `packaging/start.sh` and `scripts/lib/xonotic-shlib.sh` path fix. Touches D14, D15.
8. **Profiles** (§3.4): `standard.cfg`, `thermal.cfg`, `battery.cfg`, regenerate `left.cfg`.
9. **`touch_debug` overlay** (§10.1) — do this before step 10 or the tests are unverifiable.
10. **Docs**: fold §3 numbers into `CONTROLS.md` tables, note the retired `touch_simple_draw`,
    and file a fresh `docs/test-runs/<date>-ultramarine/FINDINGS.md` with the §11 results.
