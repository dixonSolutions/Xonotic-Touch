#!/usr/bin/env python3
"""Build the Godnotic icon and banner from the upstream Xonotic emblem.

The emblem is engine/misc/logos/xonotic_icon.svg, unchanged (LGPL 2.1+ / CC-BY 3.0,
Xonotic Community). The banner sets GODNOTIC in Xolonium, the game's own typeface,
with that emblem standing in for the first O. Nothing here is redrawn by hand.

    python3 scripts/gen-branding.py            # writes branding/, web/, touch/gfx/

Needs rsvg-convert, ImageMagick and Pillow.
"""
import os
import re
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMBLEM = os.path.join(ROOT, "engine/misc/logos/xonotic_icon.svg")
FONT_DIR = os.path.join(ROOT, "engine/data/font-xolonium.pk3dir/fonts")
FONT = os.path.join(FONT_DIR, "xolonium-bold.otf")

BANNER_W, BANNER_H = 2172, 724
FONT_SIZE = 262
TRACKING = 34          # extra space between letters, as the upstream logotype has
EMBLEM_SIZE = 470      # the emblem is taller than the letters, as upstream
EMBLEM_GAP = 6

# The menu's second background layer is 2560x2048 with the logo in its upper left,
# where the stock skin's logo sits.
LAYER_W, LAYER_H = 2560, 2048
LAYER_BANNER_W = 1540
LAYER_BANNER_LEFT = 60
LAYER_BANNER_TOP = 10


def emblem_parts():
    """The emblem's <defs> content and its drawing group, ids prefixed to stay unique."""
    svg = open(EMBLEM, encoding="utf-8").read()
    defs = re.search(r"<defs[^>]*>(.*?)</defs>", svg, re.S).group(1)
    group = re.search(r"(<g\b[^>]*id=\"XonoticIcon\".*?</g>)", svg, re.S).group(1)
    # Inkscape/sodipodi attributes need namespaces the composed file does not declare.
    for part in ("defs", "group"):
        text = defs if part == "defs" else group
        text = re.sub(r"\s(?:inkscape|sodipodi):[\w-]+=\"[^\"]*\"", "", text)
        if part == "defs":
            defs = text
        else:
            group = text
    return defs, group


def render(svg_text, out_png, width, height):
    with tempfile.TemporaryDirectory() as tmp:
        conf = os.path.join(tmp, "fonts.conf")
        with open(conf, "w") as f:
            f.write(
                '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd">'
                f"<fontconfig><dir>{FONT_DIR}</dir><cachedir>{tmp}/cache</cachedir></fontconfig>"
            )
        src = os.path.join(tmp, "in.svg")
        with open(src, "w", encoding="utf-8") as f:
            f.write(svg_text)
        env = dict(os.environ, FONTCONFIG_FILE=conf)
        subprocess.run(
            ["rsvg-convert", "-w", str(width), "-h", str(height), src, "-o", out_png],
            check=True, env=env,
        )


def icon_svg(pad=3.0):
    defs, group = emblem_parts()
    box = 48 + 2 * pad
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'viewBox="{-pad} {-pad} {box} {box}"><defs>{defs}</defs>{group}</svg>'
    )


def banner_svg():
    defs, group = emblem_parts()
    font = ImageFont.truetype(FONT, FONT_SIZE)
    ascent_box = font.getbbox("D")
    cap_top, cap_bottom = ascent_box[1], ascent_box[3]
    cap_h = cap_bottom - cap_top

    # Lay out G, the emblem, then DNOTIC, and centre the lot.
    items = []
    x = 0.0
    for i, ch in enumerate("G@DNOTIC"):
        if ch == "@":
            x += EMBLEM_GAP - TRACKING
            items.append(("emblem", x))
            x += EMBLEM_SIZE + EMBLEM_GAP
        else:
            items.append((ch, x))
            x += font.getlength(ch) + TRACKING
    total = x - TRACKING
    left = (BANNER_W - total) / 2
    mid_y = BANNER_H / 2
    # Pillow's y origin for text is the top of the em box; SVG's is the baseline.
    baseline = mid_y + cap_h / 2

    letters = []
    emblem_x = 0.0
    for kind, ix in items:
        if kind == "emblem":
            emblem_x = left + ix
            continue
        letters.append(f'<text x="{left + ix:.1f}" y="{baseline:.1f}">{kind}</text>')
    text = "".join(letters)
    # The ring's visual centre sits a little above the emblem box centre (the box
    # includes the point under the ring), so drop it to line the ring up with the caps.
    emblem_y = mid_y - EMBLEM_SIZE * 0.47
    scale = EMBLEM_SIZE / 48.0
    glow_cx = emblem_x + EMBLEM_SIZE / 2

    return f'''<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink"
     width="{BANNER_W}" height="{BANNER_H}" viewBox="0 0 {BANNER_W} {BANNER_H}">
  <defs>{defs}
    <linearGradient id="gnFill" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#061426"/>
      <stop offset="0.5" stop-color="#0f3f6f"/>
      <stop offset="1" stop-color="#061426"/>
    </linearGradient>
    <linearGradient id="gnEdge" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#bfdfff"/>
      <stop offset="0.2" stop-color="#5fafff"/>
      <stop offset="0.8" stop-color="#5fafff"/>
      <stop offset="1" stop-color="#bfdfff"/>
    </linearGradient>
    <radialGradient id="gnHalo" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#175faf" stop-opacity="0.55"/>
      <stop offset="0.6" stop-color="#174f8f" stop-opacity="0.18"/>
      <stop offset="1" stop-color="#174f8f" stop-opacity="0"/>
    </radialGradient>
    <filter id="gnGlow" x="-10%" y="-40%" width="120%" height="180%">
      <feGaussianBlur stdDeviation="9"/>
    </filter>
  </defs>
  <ellipse cx="{glow_cx:.1f}" cy="{mid_y:.1f}" rx="{EMBLEM_SIZE * 0.95:.1f}" ry="{EMBLEM_SIZE * 0.72:.1f}" fill="url(#gnHalo)"/>
  <g font-family="Xolonium" font-weight="bold" font-size="{FONT_SIZE}">
    <g fill="#174f8f" stroke="#175faf" stroke-width="14" stroke-linejoin="round" filter="url(#gnGlow)">{text}</g>
    <g fill="url(#gnFill)" stroke="url(#gnEdge)" stroke-width="4.5" stroke-linejoin="round">{text}</g>
  </g>
  <g transform="translate({emblem_x:.1f} {emblem_y:.1f}) scale({scale:.4f})">{group}</g>
</svg>'''


# Android launcher icon. Adaptive (API 26+): a 108 dp foreground with the emblem
# inside the 66 dp safe circle every mask keeps, a dark background layer, and a
# monochrome layer for themed icons (Android 13+). icon.png stays for older
# Android and for launchers that ignore adaptive icons: the emblem on the same
# dark plate, so no launcher puts it on a pale one. The resource name stays
# mipmap/icon, which the manifest of every (AndySU) host names.
ANDROID_DENSITIES = (("mdpi", 1.0), ("hdpi", 1.5), ("xhdpi", 2.0), ("xxhdpi", 3.0), ("xxxhdpi", 4.0))
ANDROID_FG_EMBLEM_DP = 62.0   # of 108: the ring's circle fits the 66 dp safe zone
ANDROID_BG_TOP = "#123a66"
ANDROID_BG_EDGE = "#050b14"


def android_background_svg():
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 108 108">'
        '<defs><radialGradient id="g" cx="0.5" cy="0.42" r="0.62">'
        f'<stop offset="0" stop-color="{ANDROID_BG_TOP}"/><stop offset="1" stop-color="{ANDROID_BG_EDGE}"/>'
        '</radialGradient></defs><rect width="108" height="108" fill="url(#g)"/></svg>'
    )


def android_icons(out_dir):
    """Writes res-style folders under out_dir; scripts/android-build.sh copies them into the app."""
    fg_pad = (108.0 / ANDROID_FG_EMBLEM_DP * 48.0 - 48.0) / 2.0
    fg = icon_svg(pad=fg_pad)
    bg = android_background_svg()
    for density, scale in ANDROID_DENSITIES:
        d = os.path.join(out_dir, f"mipmap-{density}")
        os.makedirs(d, exist_ok=True)
        n = round(108 * scale)
        render(fg, os.path.join(d, "icon_foreground.png"), n, n)
        render(bg, os.path.join(d, "icon_background.png"), n, n)
        # Monochrome: the emblem's shape in white, its inner detail kept as
        # alpha from the artwork's own brightness, so the eagle reads when tinted.
        fg_img = Image.open(os.path.join(d, "icon_foreground.png")).convert("RGBA")
        lum = fg_img.convert("L").point(lambda v: min(255, int(v * 1.6) + 70))
        alpha = Image.composite(lum, Image.new("L", fg_img.size, 0), fg_img.getchannel("A"))
        mono = Image.new("RGBA", fg_img.size, (255, 255, 255, 0))
        mono.putalpha(alpha)
        mono.save(os.path.join(d, "icon_monochrome.png"), optimize=True)
        # Legacy square icon: background plate, rounded, with the emblem.
        legacy = round(48 * scale)
        plate = Image.open(os.path.join(d, "icon_background.png")).convert("RGBA").resize((legacy, legacy), Image.LANCZOS)
        mask = Image.new("L", (legacy, legacy), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, legacy - 1, legacy - 1), radius=round(legacy * 0.22), fill=255)
        emblem_png = os.path.join(d, "_legacy_emblem.png")
        render(icon_svg(pad=(48.0 / 0.80 - 48.0) / 2.0), emblem_png, legacy, legacy)
        emblem = Image.open(emblem_png).convert("RGBA")
        os.remove(emblem_png)
        plate.alpha_composite(emblem)
        out = Image.new("RGBA", (legacy, legacy), (0, 0, 0, 0))
        out.paste(plate, (0, 0), mask)
        out.save(os.path.join(d, "icon.png"), optimize=True)
    anydpi = os.path.join(out_dir, "mipmap-anydpi-v26")
    os.makedirs(anydpi, exist_ok=True)
    with open(os.path.join(anydpi, "icon.xml"), "w", encoding="utf-8") as f:
        f.write(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<!-- Generated by scripts/gen-branding.py -->\n'
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@mipmap/icon_background" />\n'
            '    <foreground android:drawable="@mipmap/icon_foreground" />\n'
            '    <monochrome android:drawable="@mipmap/icon_monochrome" />\n'
            '</adaptive-icon>\n'
        )


def main():
    branding = os.path.join(ROOT, "branding")
    os.makedirs(branding, exist_ok=True)

    icon = icon_svg()
    with open(os.path.join(branding, "godnotic-icon.svg"), "w", encoding="utf-8") as f:
        f.write(icon)
    master = os.path.join(branding, "godnotic-icon.png")
    render(icon, master, 1024, 1024)
    for size in (512, 256, 192, 128):
        render(icon, os.path.join(branding, f"godnotic-icon-{size}.png"), size, size)
    for size in (192, 512):
        render(icon, os.path.join(ROOT, "web", f"icon-{size}.png"), size, size)

    android_icons(os.path.join(branding, "android"))

    banner = banner_svg()
    banner_png = os.path.join(branding, "godnotic-banner.png")
    render(banner, banner_png, BANNER_W, BANNER_H)
    img = Image.open(banner_png).convert("RGBA")
    img.save(banner_png, optimize=True)
    for dest in ("touch/gfx/xonotic_logo.png", "branding/game-gfx/xonotic_logo.png"):
        img.save(os.path.join(ROOT, dest), optimize=True)
    # The web page's copy: trimmed to the glow, 960 px wide, WebP with alpha
    # (47 KB instead of 434 KB), on the page's own background.
    web = img.crop((46, 2, 2131, 722))
    web = web.resize((960, round(web.height * 960 / web.width)), Image.LANCZOS)
    web.save(os.path.join(ROOT, "web", "godnotic-banner.webp"), quality=80, method=6, alpha_quality=85)

    # Loading screen: the banner centred on the stock picture's 2:1 sheet. Its
    # name is ours alone (touch/xonotic.cfg points scr_loadingscreen_picture at
    # it), because downloaded data carries a gfx/loading that would outrank it.
    loading = Image.new("RGBA", (2048, 1024), (0, 0, 0, 255))
    lw = 1280
    lh = round(lw * BANNER_H / BANNER_W)
    lb = img.resize((lw, lh), Image.LANCZOS)
    loading.paste(lb, ((2048 - lw) // 2, (1024 - lh) // 2), lb)
    loading.convert("RGB").save(os.path.join(ROOT, "touch/gfx/godnotic_loading.jpg"), quality=90, optimize=True)

    # Console background: the same idea as the loading picture (touch/xonotic.cfg
    # points touch_conback_picture at it; downloaded data carries a gfx/conback
    # with the Xonotic logo). Kept dark so the console text stays readable: a
    # deep blue fade with the banner faint below the middle. The engine draws
    # the picture's bottom edge at the console's bottom edge, so a half-height
    # console shows the lower half: the banner sits where both show it.
    cw, ch = 2048, 1280
    con = Image.new("RGB", (cw, ch), (0, 0, 0))
    px = con.load()
    for y in range(ch):
        t = y / (ch - 1)
        row = (int(4 + 6 * t), int(8 + 10 * t), int(16 + 18 * t))
        for x in range(cw):
            px[x, y] = row
    bw = 1100
    bh = round(bw * BANNER_H / BANNER_W)
    cb = img.resize((bw, bh), Image.LANCZOS)
    alpha = cb.getchannel("A").point(lambda a: a * 0.14)
    cb.putalpha(alpha)
    con.paste(cb, ((cw - bw) // 2, round(ch * 0.72 - bh / 2)), cb)
    con.save(os.path.join(ROOT, "touch/gfx/godnotic_conback.jpg"), quality=85, optimize=True)

    # Menu background layer: the banner alone on a transparent sheet.
    layer = Image.new("RGBA", (LAYER_W, LAYER_H), (0, 0, 0, 0))
    h = round(LAYER_BANNER_W * BANNER_H / BANNER_W)
    scaled = img.resize((LAYER_BANNER_W, h), Image.LANCZOS)
    layer.paste(scaled, (LAYER_BANNER_LEFT, LAYER_BANNER_TOP), scaled)
    layer_png = os.path.join(branding, "godnotic-menu-layer.png")
    layer.save(layer_png, optimize=True)
    # branding/game-gfx is copied over touch/gfx when a package is staged, so it
    # has to carry the same layer or its copy wins.
    for dest in ("touch/gfx/menu/luma/touch_background_l2.tga",
                 "branding/game-gfx/menu/luma/touch_background_l2.tga"):
        subprocess.run(["magick", layer_png, "-compress", "RLE", os.path.join(ROOT, dest)], check=True)
    print("branding written")


if __name__ == "__main__":
    sys.exit(main())
