#!/usr/bin/env python3
"""Audit the touch layout against thumb reach and touch-target guidance.

Works from the cvar layout rather than from a screenshot, so it can be run
against any profile without the game being live. It answers the two questions
that decide whether a tablet FPS is playable one-handed-per-thumb:

  1. Is each control big enough to hit, but not so big it eats the screen?
  2. Can the thumb that owns a control actually reach it without regripping?

Reach model: holding a slab tablet in landscape, each thumb pivots near the
lower corner of its side and sweeps an arc. Distances are measured from that
pivot; anything past the comfortable radius forces a regrip, which in an FPS
means you stop aiming.

Usage:
  audit-touch-ergonomics.py touch/profiles/standard.cfg
  audit-touch-ergonomics.py --con 960x640 touch/profiles/standard.cfg
"""
from __future__ import annotations

import argparse
import math
import pathlib
import re
import sys

# Surface Pro 9: 13" 3:2 panel, active area in millimetres.
PANEL_MM_W = 287.0
PANEL_MM_H = 191.0

# Thumb pivots sit slightly inboard and above the physical corner, because the
# hand grips the bezel rather than the very corner.
PIVOT_INSET_MM = 18.0
# Comfortable sweep for an adult thumb on a slab held two-handed. Past the
# "max" figure the grip has to change.
REACH_COMFORT_MM = 62.0
REACH_MAX_MM = 78.0

# Touch target guidance, in millimetres.
TARGET_MIN_MM = 9.0
TARGET_COMFORT_MM = 11.0
# Above this a control is a thumb rest (a stick base), not a button; anything
# bigger than this that is *not* a stick is simply wasting screen.
TARGET_LARGE_MM = 26.0

# widget -> (cvar prefix, owning thumb, kind)
WIDGETS = [
    ("MOVE",    "touch_move",   "left",  "stick"),
    ("FIRE",    "touch_fire",   "right", "button"),
    ("JUMP",    "touch_jump",   "right", "button"),
    ("CROUCH",  "touch_crouch", "right", "button"),
    ("WEAPON",  "touch_weapon", "right", "button"),
    ("ZOOM",    "touch_zoom",   "right", "button"),
    ("RELOAD",  "touch_reload", "right", "button"),
    ("DODGE",   "touch_dodge",  "left",  "button"),
    ("CONSOLE", "touch_con",    "right", "button"),
    ("PAUSE",   "touch_pause",  "left",  "button"),
]


def parse_cfg(paths: list[pathlib.Path]) -> dict[str, float]:
    """Read `seta`/`set`/bare cvar assignments from config files, in order."""
    values: dict[str, float] = {}
    pattern = re.compile(r'^\s*(?:seta?\s+)?([A-Za-z_][\w]*)\s+"?(-?[\d.]+)"?\s*(?://.*)?$')
    for path in paths:
        for line in path.read_text().splitlines():
            m = pattern.match(line)
            if m:
                try:
                    values[m.group(1)] = float(m.group(2))
                except ValueError:
                    pass
    return values


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cfg", nargs="+", type=pathlib.Path)
    ap.add_argument("--con", default="960x640",
                    help="console space, e.g. 960x640")
    args = ap.parse_args()

    con_w, con_h = (int(v) for v in args.con.lower().split("x"))
    cv = parse_cfg(args.cfg)

    scale = cv.get("touch_scale", 1.0)
    screen_min = min(con_w, con_h)
    mm_per_con_x = PANEL_MM_W / con_w
    mm_per_con_y = PANEL_MM_H / con_h

    pivots = {
        "left":  (PIVOT_INSET_MM, PANEL_MM_H - PIVOT_INSET_MM),
        "right": (PANEL_MM_W - PIVOT_INSET_MM, PANEL_MM_H - PIVOT_INSET_MM),
    }

    print(f"panel {PANEL_MM_W:.0f}x{PANEL_MM_H:.0f} mm   console {con_w}x{con_h}"
          f"   touch_scale {scale}")
    print(f"thumb pivots: left {pivots['left']}, right {pivots['right']} mm")
    print(f"reach: comfortable <= {REACH_COMFORT_MM:.0f} mm, max {REACH_MAX_MM:.0f} mm\n")

    header = f"{'widget':8} {'vis':3} {'centre mm':>15} {'size mm':>13} {'reach':>7} {'verdict'}"
    print(header)
    print("-" * (len(header) + 26))

    total_area_mm2 = 0.0
    for name, prefix, thumb, kind in WIDGETS:
        visible = cv.get(f"{prefix}_visible", 1.0)
        nx = cv.get(f"{prefix}_x")
        ny = cv.get(f"{prefix}_y")
        nsize = cv.get(f"{prefix}_size")
        if nx is None or ny is None or nsize is None:
            continue
        aspect = max(1.0, cv.get(f"{prefix}_aspect", 1.0))

        short_con = screen_min * nsize * scale
        long_con = short_con * aspect
        w_mm = long_con * mm_per_con_x
        h_mm = short_con * mm_per_con_y
        cx_mm = nx * PANEL_MM_W
        cy_mm = ny * PANEL_MM_H

        px, py = pivots[thumb]
        reach = math.hypot(cx_mm - px, cy_mm - py)

        notes = []
        if visible:
            total_area_mm2 += w_mm * h_mm
            if reach > REACH_MAX_MM:
                notes.append("UNREACHABLE")
            elif reach > REACH_COMFORT_MM:
                notes.append("strained")
            small = min(w_mm, h_mm)
            if small < TARGET_MIN_MM:
                notes.append("too small")
            elif kind == "button" and small > TARGET_LARGE_MM:
                notes.append("oversized")
        verdict = ", ".join(notes) if notes else ("ok" if visible else "hidden")

        print(f"{name:8} {int(visible):^3} "
              f"{cx_mm:6.1f},{cy_mm:6.1f}  {w_mm:5.1f} x{h_mm:5.1f}  "
              f"{reach:6.1f}  {verdict}")

    panel_area = PANEL_MM_W * PANEL_MM_H
    print(f"\nvisible controls cover {total_area_mm2:.0f} mm² of "
          f"{panel_area:.0f} mm² ({100 * total_area_mm2 / panel_area:.1f}% of the screen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
