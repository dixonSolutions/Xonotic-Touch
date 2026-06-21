# Screen calculation layer

Landscape-first display sizing for touch devices. Implemented in **`touch/screen-calc.sh`** (no compile required).

## Behaviour

1. **Detect** raw display size (`mirout`, fb0, `xdpyinfo`, `wlr-randr`, or env override).
2. **Landscape** — swap if height > width so `vid_width` is always the long edge.
3. **Even dimensions** — width/height rounded down to even pixels (GLES-friendly).
4. **DPI** — from physical mm when `mirout` reports them; else 320 dpi default.
5. **Apply** — writes `data/screen.layout.cfg` and passes `+vid_*` cvars at launch.

## Landscape orientation

Xonotic Touch is **landscape-only** (including inverted landscape). `touch/screen-calc.sh` always maps the long edge to `vid_width`. Portrait is not supported.

## Testing without a phone

```bash
XONOTIC_SCREEN_WIDTH=1224 XONOTIC_SCREEN_HEIGHT=2700 \
  sh -c '. touch/screen-calc.sh; xonotic_screen_calc /tmp/screen.layout.cfg; echo $XONOTIC_VID_WIDTH x $XONOTIC_VID_HEIGHT'
# Expect: 2700 x 1224 (landscape)
```

## QuakeC / HUD layout

Use engine cvars already set at runtime:

- `vid_width`, `vid_height`
- `vid_touchscreen_xdpi`, `vid_touchscreen_ydpi`, `vid_touchscreen_density`

Touch HUD in `qcsrc/client/` should read these (not hardcoded 1920×1080). Control layout and feel: [CONTROLS.md](CONTROLS.md).

## Files

| File | Role |
|------|------|
| `touch/screen-calc.sh` | Calculation logic (maintainer edits) |
| `packaging/start.sh` | Sources calc, launches engine |
| `data/screen.layout.cfg` | Generated on device at launch |
