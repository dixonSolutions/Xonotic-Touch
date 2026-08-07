#!/usr/bin/env python3
"""Measure on-screen touch widget geometry from a captured screenshot.

Touch controls have to be verified in real pixels, not by eye: a console space
whose aspect does not match the display silently stretches every control and
moves every hit box, and that is invisible in a side-by-side glance.

Given a rough region of interest, this reports the bounding box of the bright
overlay pixels inside it, plus the aspect ratio and the physical size implied by
the panel dimensions.

Usage:
  measure-widget.py shot.jpg --roi 0.78 0.50 0.98 0.80 --label FIRE
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tempfile

import numpy as np

# Surface Pro 9 panel, used to convert measured pixels into millimetres.
DEFAULT_PANEL_MM_W = 287.0
DEFAULT_PANEL_MM_H = 191.0


def load_image(path: pathlib.Path) -> np.ndarray:
    """Decode to a HxWx3 uint8 array via ffmpeg (avoids a Pillow dependency)."""
    with tempfile.TemporaryDirectory() as td:
        raw = pathlib.Path(td) / "out.rgb"
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", str(path)],
            capture_output=True, text=True, check=True)
        w, h = (int(v) for v in probe.stdout.strip().split("x"))
        subprocess.run(
            ["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo",
             "-pix_fmt", "rgb24", str(raw)], check=True)
        data = np.frombuffer(raw.read_bytes(), dtype=np.uint8)
        return data.reshape(h, w, 3)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=pathlib.Path)
    ap.add_argument("--roi", nargs=4, type=float, metavar=("X0", "Y0", "X1", "Y1"),
                    required=True, help="normalized region to search")
    ap.add_argument("--label", default="widget")
    ap.add_argument("--threshold", type=int, default=150,
                    help="min mean brightness counted as overlay")
    ap.add_argument("--panel-mm", nargs=2, type=float,
                    default=(DEFAULT_PANEL_MM_W, DEFAULT_PANEL_MM_H))
    args = ap.parse_args()

    img = load_image(args.image)
    h, w, _ = img.shape
    x0, y0, x1, y1 = args.roi
    px0, py0 = int(x0 * w), int(y0 * h)
    px1, py1 = int(x1 * w), int(y1 * h)
    crop = img[py0:py1, px0:px1].mean(axis=2)

    mask = crop >= args.threshold
    if not mask.any():
        print(f"{args.label}: nothing above threshold {args.threshold} in ROI")
        return 1

    rows = np.where(mask.any(axis=1))[0]
    cols = np.where(mask.any(axis=0))[0]
    bw = cols[-1] - cols[0] + 1
    bh = rows[-1] - rows[0] + 1

    mm_per_px_x = args.panel_mm[0] / w
    mm_per_px_y = args.panel_mm[1] / h

    print(f"{args.label}:")
    print(f"  image        {w}x{h}")
    print(f"  bounds       x {px0 + cols[0]}..{px0 + cols[-1]}  "
          f"y {py0 + rows[0]}..{py0 + rows[-1]}")
    print(f"  size         {bw} x {bh} px")
    print(f"  aspect h/w   {bh / bw:.3f}   (1.000 == geometrically square)")
    print(f"  physical     {bw * mm_per_px_x:.1f} x {bh * mm_per_px_y:.1f} mm")
    return 0


if __name__ == "__main__":
    sys.exit(main())
