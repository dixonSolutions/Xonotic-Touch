#!/usr/bin/env python3
"""Generate the anti-aliased shape textures used by the touch overlay.

The overlay used to draw every control by stretching `gfx/colors/white`, which
is why circular controls rendered as hard-edged squares. Baking a handful of
alpha masks instead gives real circles, rings and capsules at one `drawpic` per
shape — cheaper than the previous multi-rect "glass" stacks and far better
looking.

Textures are white with meaningful alpha; the overlay tints them at draw time,
so one mask serves every colour.

Output: touch/gfx/touch/*.png (8-bit RGBA)

Usage: scripts/gen-touch-shapes.py [--outdir DIR]
"""
from __future__ import annotations

import argparse
import pathlib
import struct
import zlib

import numpy as np

# Supersampling factor for edge antialiasing. 4x is indistinguishable from 8x
# at the sizes these masks are drawn but builds noticeably faster. The round
# masks are 512 px so the largest controls on a high-density tablet (FIRE is
# over 300 px across at 2880x1920) are never magnified; the engine mipmaps
# them (gl_draw.c) so small ones stay smooth too.
SS = 4


def write_png(path: pathlib.Path, alpha: np.ndarray) -> None:
    """Write a white RGBA image with `alpha` (float 0..1) as PNG.

    PNG rather than the uncompressed TGA these used to be: white RGB with a
    smooth alpha compresses to a few kilobytes, where a 512 px TGA is a megabyte
    in the repository and in every package. The engine finds either by the same
    extensionless name.
    """
    h, w = alpha.shape
    a8 = np.clip(alpha * 255.0 + 0.5, 0, 255).astype(np.uint8)
    rgb = np.full((h, w), 255, dtype=np.uint8)
    rgba = np.dstack([rgb, rgb, rgb, a8])
    # Filter type 0 (none) on every row, then deflate.
    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))

    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


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
    "disc": (512, lambda: make_disc(512)),
    "disc_soft": (512, lambda: make_disc_gradient(512)),
    "shadow": (512, lambda: make_soft_shadow(512)),
    # Stroke weights are fractions of the radius, so rings keep their visual
    # weight proportional to the control they outline.
    "ring_thin": (512, lambda: make_ring(512, 0.035)),
    "ring": (512, lambda: make_ring(512, 0.065)),
    "ring_thick": (512, lambda: make_ring(512, 0.13)),
    "rrect": (256, lambda: make_rounded_rect(256, 0.25)),
}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    default_out = pathlib.Path(__file__).resolve().parent.parent / "touch" / "gfx" / "touch"
    ap.add_argument("--outdir", type=pathlib.Path, default=default_out)
    args = ap.parse_args()

    args.outdir.mkdir(parents=True, exist_ok=True)
    for name, (_size, build) in SHAPES.items():
        path = args.outdir / f"{name}.png"
        write_png(path, build())
        # A stale TGA of the same name would win: the engine tries .tga first.
        stale = args.outdir / f"{name}.tga"
        if stale.exists():
            stale.unlink()
        print(f"wrote {path} ({path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
