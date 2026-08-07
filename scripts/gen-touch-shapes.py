#!/usr/bin/env python3
"""Generate the anti-aliased shape textures used by the touch overlay.

The overlay used to draw every control by stretching `gfx/colors/white`, which
is why circular controls rendered as hard-edged squares. Baking a handful of
alpha masks instead gives real circles, rings and capsules at one `drawpic` per
shape — cheaper than the previous multi-rect "glass" stacks and far better
looking.

Textures are white with meaningful alpha; the overlay tints them at draw time,
so one mask serves every colour.

Output: touch/gfx/touch/*.tga (32-bit uncompressed BGRA, bottom-up)

Usage: scripts/gen-touch-shapes.py [--outdir DIR]
"""
from __future__ import annotations

import argparse
import pathlib
import struct

import numpy as np

# Supersampling factor for edge antialiasing. 4x is indistinguishable from 8x
# at the sizes these masks are drawn but builds noticeably faster.
SS = 4


def write_tga(path: pathlib.Path, alpha: np.ndarray) -> None:
    """Write a white RGB image with `alpha` (float 0..1) as 32-bit TGA."""
    h, w = alpha.shape
    a8 = np.clip(alpha * 255.0 + 0.5, 0, 255).astype(np.uint8)
    rgb = np.full((h, w), 255, dtype=np.uint8)
    # TGA is BGRA and bottom-up by default.
    bgra = np.dstack([rgb, rgb, rgb, a8])[::-1]
    header = struct.pack(
        "<BBBHHBHHHHBB",
        0,      # id length
        0,      # no colour map
        2,      # uncompressed true-colour
        0, 0, 0,  # colour map spec
        0, 0,   # x/y origin
        w, h,
        32,     # bits per pixel
        8,      # 8 alpha bits, origin bottom-left
    )
    path.write_bytes(header + bgra.tobytes())


def _radius_field(size: int) -> np.ndarray:
    """Distance from centre in units of the half-size, supersampled."""
    n = size * SS
    # Sample at pixel centres so the mask stays symmetric.
    coords = (np.arange(n) + 0.5) / n * 2.0 - 1.0
    yy, xx = np.meshgrid(coords, coords, indexing="ij")
    return np.sqrt(xx * xx + yy * yy)


def _downsample(field: np.ndarray, size: int) -> np.ndarray:
    return field.reshape(size, SS, size, SS).mean(axis=(1, 3))


def make_disc(size: int) -> np.ndarray:
    """Solid antialiased circle inscribed in the texture."""
    r = _radius_field(size)
    return _downsample((r <= 1.0).astype(np.float32), size)


def make_ring(size: int, thickness: float) -> np.ndarray:
    """Annulus whose stroke is `thickness` of the radius, hugging the edge."""
    r = _radius_field(size)
    mask = ((r <= 1.0) & (r >= 1.0 - thickness)).astype(np.float32)
    return _downsample(mask, size)


def make_soft_shadow(size: int, falloff: float = 2.2) -> np.ndarray:
    """Radial shadow blob for elevation under round controls."""
    r = _radius_field(size)
    a = np.clip(1.0 - r, 0.0, 1.0) ** falloff
    return _downsample(a.astype(np.float32), size)


def make_disc_gradient(size: int) -> np.ndarray:
    """Circle that fades toward its edge — used as a soft inner glow."""
    r = _radius_field(size)
    a = np.where(r <= 1.0, np.clip(1.0 - r, 0.0, 1.0) ** 0.65, 0.0)
    return _downsample(a.astype(np.float32), size)


def make_rounded_rect(size: int, radius_frac: float) -> np.ndarray:
    """Square rounded rect, for 9-slicing panels and bars."""
    n = size * SS
    coords = (np.arange(n) + 0.5) / n
    yy, xx = np.meshgrid(coords, coords, indexing="ij")
    rad = radius_frac
    # Distance outside the inner rect that the corner arcs are centred on.
    dx = np.maximum(np.maximum(rad - xx, xx - (1.0 - rad)), 0.0)
    dy = np.maximum(np.maximum(rad - yy, yy - (1.0 - rad)), 0.0)
    dist = np.sqrt(dx * dx + dy * dy)
    return _downsample((dist <= rad).astype(np.float32), size)


SHAPES = {
    # name: (size, builder)
    "disc": (256, lambda: make_disc(256)),
    "disc_soft": (256, lambda: make_disc_gradient(256)),
    "shadow": (256, lambda: make_soft_shadow(256)),
    # Stroke weights are fractions of the radius, so rings keep their visual
    # weight proportional to the control they outline.
    "ring_thin": (256, lambda: make_ring(256, 0.035)),
    "ring": (256, lambda: make_ring(256, 0.065)),
    "ring_thick": (256, lambda: make_ring(256, 0.13)),
    "rrect": (128, lambda: make_rounded_rect(128, 0.25)),
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    default_out = pathlib.Path(__file__).resolve().parent.parent / "touch" / "gfx" / "touch"
    ap.add_argument("--outdir", type=pathlib.Path, default=default_out)
    args = ap.parse_args()

    args.outdir.mkdir(parents=True, exist_ok=True)
    for name, (_size, build) in SHAPES.items():
        path = args.outdir / f"{name}.tga"
        write_tga(path, build())
        print(f"wrote {path} ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
