#!/usr/bin/env python3
"""Repack Xonotic pk3s into the phone pack set (docs/PHONE_ASSETS.md).

    scripts/repack-phone-assets.py [options] -o OUTDIR pack.pk3...

Each pack becomes OUTDIR/<stem>-phone.pk3, meant to be installed under the
name of the pack it came from (the manifest says so). Entry by entry:

  dds/*.dds   DXT1/3/5 textures become dds/*.ktx: ASTC in a KTX 1 file with
              the whole mip chain. The engine uploads it as it is when the GPU
              has GL_KHR_texture_compression_astc_ldr (touch_assets_ktx.h)
              instead of decoding DXT in software. --textures keep leaves them.
  *.ogg       Ogg Vorbis music (--audio music, the default) or all sound
              (--audio all) becomes Ogg Opus, under the same
              name: the QuakeC looks for sounds by their .ogg name, and the
              engine looks inside (touch_assets_opus.c).
  the rest    stored with zstd (zip method 93) when that saves 2% or more,
              else stored. An entry that would grow keeps its original
              compressed payload, name and bytes. The engine inflates zstd entries
              several times faster (touch_assets_fs.h). Ogg, mp3 and other
              compressed formats are stored as they are.

Writes OUTDIR/phone-manifest.json (one record per pack: file name, size,
sha256, the name to install it under and the source pack's name, size and
sha256) and OUTDIR/phone-sizes.md (before and after per pack and per kind).

Needs python 3.14 (compression.zstd), numpy, ffmpeg (with libopus for
--audio music or all) and astcenc 4.x or newer for --textures astc (ASTCENC=path,
else astcenc-avx2, astcenc-sse4.1, astcenc-sse2 or astcenc on PATH).
"""

import argparse
import fnmatch
import concurrent.futures
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
import zlib

try:
    from compression import zstd
except ImportError:
    sys.exit("python 3.14 or newer is needed (compression.zstd)")
import numpy as np

# compressed already: zstd would spend time for nothing
STORE_EXT = {"ogg", "opus", "mp3", "pk3", "zip", "gz", "webp", "avi", "ogv"}

ZIP_STORED, ZIP_ZSTD = 0, 93
KTX_ID = b"\xabKTX 11\xbb\r\n\x1a\n"
GL_RGB, GL_RGBA = 0x1907, 0x1908
ASTC_BLOCKS = [(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8),
               (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12)]
GL_ASTC_FIRST = 0x93B0  # GL_COMPRESSED_RGBA_ASTC_4x4_KHR

# --profile presets. Anything set explicitly on the command line still wins:
# these are only the defaults of that profile.
PROFILES = {
    # The desktop-sized phone set, i.e. the option defaults below.
    "full": {},
    # The set the app downloads: coarser blocks and half-size world textures.
    # A phone screen is small and held at arm's length, which is what buys the
    # bytes back; 2D graphics, the part read at 1:1, keep their size.
    # JPEGs: the official lightmaps are saved at quality ~99 and are a third of
    # the maps pack. Each re-encode keeps the source's chroma sampling and is
    # checked against the picture it replaces (recompress_jpeg). What a
    # running game never opens (map and model sources, QuakeC line tables, the
    # benchmark demo) is left out.
    "slim": {"world_blocks": (10, 10), "model_blocks": (8, 6), "norm_blocks": (6, 6),
             "max_size": 512, "music_rate": "64k",
             "jpeg_quality": 86, "lightmap_quality": 90,
             "drop": ["maps/*.map", "models/*.map", "*.ase", "*.lno", "demos/*.dem"]},
}


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def ext_of(name):
    base = name.rsplit("/", 1)[-1]
    return base.rsplit(".", 1)[-1].lower() if "." in base else ""


# ---------------------------------------------------------------- textures

def dds_header(data):
    """(width, height, fourcc) of a DXT1/3/5 DDS file, else None."""
    if len(data) < 128 or data[:4] != b"DDS " or struct.unpack_from("<I", data, 4)[0] != 124:
        return None
    height, width = struct.unpack_from("<II", data, 12)
    if not struct.unpack_from("<I", data, 80)[0] & 4:  # DDPF_FOURCC
        return None
    fourcc = data[84:88]
    if fourcc not in (b"DXT1", b"DXT3", b"DXT5") or not (0 < width <= 16384 and 0 < height <= 16384):
        return None
    return width, height, fourcc


def decode_dds(path, width, height):
    """Top level of a DDS file as an RGBA uint8 array (ffmpeg's dds decoder)."""
    raw = subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-threads", "1", "-i", path,
                          "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
                         check=True, capture_output=True).stdout
    if len(raw) != width * height * 4:
        raise ValueError("ffmpeg gave %d bytes for %dx%d" % (len(raw), width, height))
    return np.frombuffer(raw, np.uint8).reshape(height, width, 4)


def half(img):
    """Next mip level: 2x2 box filter, odd sizes keep their last row/column."""
    h, w = img.shape[:2]
    f = img.astype(np.uint16)
    if h > 1:
        if h & 1:
            f = np.concatenate([f, f[-1:]], 0)
        f = f[0::2] + f[1::2]
    else:
        f = f * 2
    if w > 1:
        if w & 1:
            f = np.concatenate([f, f[:, -1:]], 1)
        f = f[:, 0::2] + f[:, 1::2]
    else:
        f = f * 2
    return ((f + 2) >> 2).astype(np.uint8)


def write_tga(path, img):
    h, w = img.shape[:2]
    with open(path, "wb") as f:
        # uncompressed true colour, 32 bit, origin top left
        f.write(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 0x28))
        f.write(img[:, :, [2, 1, 0, 3]].tobytes())


class TextureEncoder:
    def __init__(self, astcenc, preset, blocks, max_size):
        self.astcenc, self.preset, self.blocks, self.max_size = astcenc, preset, blocks, max_size

    def blocks_for(self, name):
        """ASTC block size by kind: the 2D graphics and normal maps show
        block artefacts the most, model textures are seen up close, world
        textures are most of the bytes."""
        stem = name.rsplit(".", 1)[0]
        if name.startswith("dds/gfx/"):
            return self.blocks["gfx"]
        if stem.endswith("_norm"):
            return self.blocks["norm"]
        if name.startswith("dds/models/"):
            return self.blocks["model"]
        return self.blocks["world"]

    def encode(self, name, data):
        """A DDS file to a KTX 1 file of ASTC levels, or None to keep the DDS."""
        info = dds_header(data)
        if info is None:
            return None
        width, height, _ = info
        bw, bh = self.blocks_for(name)
        with tempfile.TemporaryDirectory(prefix="xt-ktx-") as tmp:
            src = os.path.join(tmp, "in.dds")
            with open(src, "wb") as f:
                f.write(data)
            img = decode_dds(src, width, height)
            # world and model textures above max_size start one level down;
            # 2D graphics keep their size, which is the size the HUD and
            # menu draw them at
            if self.max_size and not name.startswith("dds/gfx/"):
                while max(img.shape[:2]) > self.max_size:
                    img = half(img)
            height, width = img.shape[:2]
            alpha = bool((img[:, :, 3] < 255).any())
            avg = np.clip(img.reshape(-1, 4).mean(0) + 0.5, 0, 255).astype(np.uint8).tobytes()
            levels = []
            while True:
                h, w = img.shape[:2]
                tga, out = os.path.join(tmp, "l.tga"), os.path.join(tmp, "l.astc")
                write_tga(tga, img)
                # -cl: the engine uploads the linear ASTC format (sRGB only
                # where the DDS would have been sRGB too); normal maps need
                # their alpha (height for offset mapping) kept, so no -normal
                subprocess.run([self.astcenc, "-cl", tga, out, "%dx%d" % (bw, bh), self.preset,
                                "-silent", "-j", "1"], check=True, capture_output=True)
                with open(out, "rb") as f:
                    blocks = f.read()[16:]  # 16 byte .astc header
                if len(blocks) != -(-w // bw) * -(-h // bh) * 16:
                    raise ValueError("astcenc output size")
                levels.append(blocks)
                if w == 1 and h == 1:
                    break
                img = half(img)
        kv = b"DPAVGCOLOR\0" + avg
        kv = struct.pack("<I", len(kv)) + kv
        kv += b"\0" * (-len(kv) % 4)
        fmt = GL_ASTC_FIRST + ASTC_BLOCKS.index((bw, bh))
        out = [KTX_ID, struct.pack("<13I", 0x04030201, 0, 1, 0, fmt, GL_RGBA if alpha else GL_RGB,
                                   width, height, 0, 0, 1, len(levels), len(kv)), kv]
        for blocks in levels:
            out += [struct.pack("<I", len(blocks)), blocks, b"\0" * (-len(blocks) % 4)]
        return b"".join(out)


# ------------------------------------------------------------------- audio

def is_music(name):
    return name.startswith("sound/cdtracks/")


def to_opus(name, data, music_rate, sfx_rate):
    """Ogg Vorbis to Ogg Opus, or None to keep the Vorbis file."""
    rate = music_rate if is_music(name) else sfx_rate
    with tempfile.TemporaryDirectory(prefix="xt-opus-") as tmp:
        src, dst = os.path.join(tmp, "in.ogg"), os.path.join(tmp, "out.opus")
        with open(src, "wb") as f:
            f.write(data)
        try:
            # -map_metadata 0 keeps the LOOP_START and ReplayGain comments
            subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-threads", "1", "-i", src,
                            "-map", "0:a:0", "-map_metadata", "0", "-c:a", "libopus", "-b:a", rate,
                            "-vbr", "on", "-compression_level", "10",
                            # a Vorbis start a little below zero otherwise gives
                            # granule positions opusfile rejects (OP_EBADTIMESTAMP)
                            "-avoid_negative_ts", "make_zero", dst],
                           check=True, capture_output=True)
            with open(dst, "rb") as f:
                out = f.read()
        except (subprocess.CalledProcessError, OSError) as e:
            log("  %s: kept as Vorbis (%s)" % (name, e))
            return None
    return out if 0 < len(out) < len(data) else None


# -------------------------------------------------------------------- jpeg

def is_lightmap(name):
    return name.startswith("maps/") and "/lm_" in name


# A re-encode has to stay this close to the picture it replaces (PSNR in dB,
# over all channels), or it is not used.
JPEG_FLOOR = 38.0
# Lightmaps are smooth and forgiving. A deluxemap is not: it stores the light's
# direction as a colour, so a colour error turns a surface's shading, and it
# sits in the same folder under the odd numbers (lm_0001, lm_0003, ...).
LIGHTMAP_FLOOR = 38.0
DELUXEMAP_QUALITY = 95
DELUXEMAP_FLOOR = 41.0


def is_deluxemap(name, rgb):
    """An odd-numbered lightmap whose pixels are mostly unit vectors."""
    try:
        odd = int(name.rsplit("lm_", 1)[1][:4]) % 2 == 1
    except (IndexError, ValueError):
        return False
    if not odd or rgb.ndim != 3:
        return False
    v = rgb.astype(np.float32) / 127.5 - 1.0
    return float(np.mean(np.abs(np.sqrt((v * v).sum(-1)) - 1.0) < 0.12)) > 0.3


def recompress_jpeg(name, data, quality, lightmap=False):
    """The same picture as a smaller baseline JPEG, or None to keep the entry.

    Baseline with optimised Huffman tables, which every libjpeg reads, and the
    source's own chroma sampling: Pillow's default halves the colour resolution,
    which bleeds neighbouring charts of a lightmap atlas into each other. The
    result is decoded again and compared with the source; when it is further
    off than the floor for its kind, higher qualities are tried, and the entry is kept as it is
    if none is both close enough and a tenth smaller. Greyscale stays greyscale
    (the *_alpha.jpg masks); CMYK and anything Pillow cannot read is left alone.
    """
    if not quality or data[:2] != b"\xff\xd8":
        return None
    try:
        from PIL import Image, JpegImagePlugin
        import io
        im = Image.open(io.BytesIO(data))
        sampling = JpegImagePlugin.get_sampling(im) if im.mode == "RGB" else -1
        table = im.quantization.get(0) if im.quantization else None
        im.load()
        if im.mode not in ("RGB", "L") or (im.mode == "RGB" and sampling not in (0, 1, 2)):
            return None
        ref = np.asarray(im, dtype=np.int16)
        floor = JPEG_FLOOR
        if lightmap:
            floor = LIGHTMAP_FLOOR
            if is_deluxemap(name, ref):
                quality, floor = max(quality, DELUXEMAP_QUALITY), DELUXEMAP_FLOOR
        # Already at or below the target: re-encoding would only add loss.
        if table and sum(table) / len(table) >= _QTABLE_MEAN.get(quality, 0):
            return None
        for q in sorted({quality, max(quality, 93), max(quality, 96), max(quality, 97)}):
            out = io.BytesIO()
            opts = {"subsampling": sampling} if im.mode == "RGB" else {}
            im.save(out, "JPEG", quality=q, optimize=True, progressive=False, **opts)
            out = out.getvalue()
            if len(out) > len(data) * 0.9:
                return None  # a higher quality will only be bigger
            got = np.asarray(Image.open(io.BytesIO(out)), dtype=np.int16)
            mse = float(np.mean((got - ref) ** 2))
            if mse == 0 or 10 * np.log10(255.0 ** 2 / mse) >= floor:
                return out
    except Exception as e:  # a broken picture is the engine's to report, not ours
        log("  %s: kept (%s)" % (name, e))
    return None


def _qtable_means():
    """Mean of libjpeg's luminance table at each quality, to tell how hard a
    file was compressed already."""
    base = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
            14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
            49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
    means = {}
    for q in range(1, 101):
        scale = 5000 // q if q < 50 else 200 - q * 2
        means[q] = sum(min(255, max(1, (b * scale + 50) // 100)) for b in base) / 64
    return means


_QTABLE_MEAN = _qtable_means()


def dropped(name, patterns):
    """A pattern with a slash is matched against the whole entry name (and *
    crosses directories); one without, against the file name."""
    base = name.rsplit("/", 1)[-1]
    return any(fnmatch.fnmatchcase(name if "/" in p else base, p) for p in patterns)


# --------------------------------------------------------------------- zip

def pack_entry(name, data, level):
    """(method, payload) for one entry: zstd when it saves 2%, else stored."""
    if ext_of(name) in STORE_EXT or len(data) < 128:
        return ZIP_STORED, data
    comp = zstd.compress(data, level)
    if len(comp) <= len(data) * 0.98:
        return ZIP_ZSTD, comp
    return ZIP_STORED, data


class ZipWriter:
    """Just enough zip for pk3s: no zip64, sizes in the local headers, and
    the unix mode kept so symlink entries stay symlinks."""

    def __init__(self, path):
        self.f = open(path, "wb")
        self.central = []
        self.count = 0

    def add(self, name, method, payload, size, crc, attr, dostime):
        raw = name.encode("utf-8")
        flags = 0x800 if not name.isascii() else 0
        need = 63 if method == ZIP_ZSTD else 20 if method == 8 else 10
        off = self.f.tell()
        if off >= 1 << 32 or len(payload) >= 1 << 32 or size >= 1 << 32:
            raise OverflowError("pack needs zip64")
        self.f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, need, flags, method, dostime & 0xFFFF,
                                 dostime >> 16, crc, len(payload), size, len(raw), 0))
        self.f.write(raw)
        self.f.write(payload)
        self.central.append(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, (3 << 8) | need, need, flags,
                                        method, dostime & 0xFFFF, dostime >> 16, crc, len(payload), size,
                                        len(raw), 0, 0, 0, 0, attr, off) + raw)
        self.count += 1

    def close(self):
        start = self.f.tell()
        for c in self.central:
            self.f.write(c)
        size = self.f.tell() - start
        if self.count >= 0xFFFF or start >= 1 << 32:
            raise OverflowError("pack needs zip64")
        self.f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, self.count, self.count, size, start, 0))
        self.f.close()


def dostime_of(info):
    y, mo, d, h, mi, s = info.date_time
    return ((max(y, 1980) - 1980) << 25) | (mo << 21) | (d << 16) | (h << 11) | (mi << 5) | (s // 2)


def is_symlink(info):
    return (info.external_attr >> 16) & 0o170000 == 0o120000


def original_payload(src, info):
    """Read a validated zip entry's compressed bytes without recompressing it."""
    if info.flag_bits & 1 or info.compress_type not in (0, 8, ZIP_ZSTD):
        return None
    src.seek(info.header_offset)
    header = src.read(30)
    if len(header) != 30 or header[:4] != b"PK\x03\x04":
        raise ValueError("invalid local ZIP header: " + info.filename)
    name_len, extra_len = struct.unpack_from("<HH", header, 26)
    src.seek(name_len + extra_len, 1)
    payload = src.read(info.compress_size)
    if len(payload) != info.compress_size:
        raise ValueError("truncated ZIP entry: " + info.filename)
    return payload


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------ driver

def repack(args, pack, textures, stats):
    base = os.path.basename(pack)
    stem = base[:-4] if base.lower().endswith(".pk3") else base
    outname = stem + "-phone.pk3"
    outpath = os.path.join(args.outdir, outname)
    zout = ZipWriter(outpath + ".part")
    kinds = {k: [0, 0, 0] for k in ("textures", "audio", "jpeg", "zstd", "stored", "retained", "dropped")}
    seen = set()
    converted = set()  # dds names that became ktx
    lock = threading.Lock()
    t0 = time.time()

    def convert(info, data):
        name = info.filename
        ext = ext_of(name)
        newname, newdata, kind = name, data, None
        try:
            if ext == "dds" and textures and name.startswith("dds/"):
                ktx = textures.encode(name, data)
                if ktx is not None:
                    newname, newdata, kind = name[:-3] + "ktx", ktx, "textures"
            elif ext == "ogg" and (args.audio == "all" or (args.audio == "music" and is_music(name))):
                opus = to_opus(name, data, args.music_rate, args.sfx_rate)
                if opus is not None:
                    newname, newdata, kind = name, opus, "audio"
            elif ext in ("jpg", "jpeg"):
                lightmap = is_lightmap(name)
                quality = getattr(args, "lightmap_quality", 0) if lightmap else getattr(args, "jpeg_quality", 0)
                jpeg = recompress_jpeg(name, data, quality, lightmap)
                if jpeg is not None:
                    newname, newdata, kind = name, jpeg, "jpeg"
        except (subprocess.CalledProcessError, OSError, ValueError) as e:
            err = getattr(e, "stderr", b"") or b""
            log("  %s: kept (%s %s)" % (name, e, err.decode(errors="replace").strip()[:200]))
        level = args.zstd_level if len(newdata) <= 32 << 20 else min(args.zstd_level, 15)
        method, payload = pack_entry(newname, newdata, level)
        if kind is None:
            kind = "zstd" if method == ZIP_ZSTD else "stored"
        return newname, newdata, method, payload, kind

    def emit(info, name, data, method, payload, kind):
        if name in seen:
            log("  duplicate entry %s dropped" % name)
            return
        seen.add(name)
        zout.add(name, method, payload, len(data), zlib.crc32(data), info.external_attr, dostime_of(info))
        k = kinds[kind]
        k[0] += 1
        k[1] += info.compress_size
        k[2] += len(payload)

    with zipfile.ZipFile(pack) as zin, open(pack, "rb") as raw, concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        infos = [i for i in zin.infolist() if not i.is_dir()]
        if args.limit:
            infos = infos[:args.limit]
        drop = getattr(args, "drop", None) or []
        if drop:
            for i in infos:
                if dropped(i.filename, drop):
                    k = kinds["dropped"]
                    k[0] += 1
                    k[1] += i.compress_size
            infos = [i for i in infos if not dropped(i.filename, drop)]
        links = [i for i in infos if is_symlink(i)]
        files = [i for i in infos if not is_symlink(i)]

        def work(info):
            with lock:
                data = zin.read(info)
                original = original_payload(raw, info)
            result = convert(info, data)
            name, newdata, method, payload, kind = result
            # A faster decoder does not justify making the default download
            # larger. Keep both the original name and data if a conversion
            # loses; symlink rewriting below follows only accepted KTX files.
            # --allow-growth is about a *converted* entry (a KTX or Opus file
            # that came out bigger than its source), never about storing an
            # already-compressed (Ogg, JPG) entry uncompressed for nothing.
            old_size = info.compress_size + 2 * len(info.filename.encode("utf-8"))
            new_size = len(payload) + 2 * len(name.encode("utf-8"))
            if (original is not None and new_size > old_size
                    and (not args.allow_growth or kind not in ("textures", "audio"))):
                result = (info.filename, data, info.compress_type, original, "retained")
            return info, result

        # a bounded window of entries in flight keeps memory in check while
        # the output keeps the source order
        window = max(args.jobs * 3, 8)
        pending = []
        done = 0
        for info in files:
            pending.append(pool.submit(work, info))
            if len(pending) >= window:
                i, (name, data, method, payload, kind) = pending.pop(0).result()
                if kind == "textures":
                    converted.add(i.filename)
                emit(i, name, data, method, payload, kind)
                done += 1
                if done % 500 == 0:
                    log("  %s: %d/%d entries, %.0f s" % (base, done, len(files), time.time() - t0))
        for fut in pending:
            i, (name, data, method, payload, kind) = fut.result()
            if kind == "textures":
                converted.add(i.filename)
            emit(i, name, data, method, payload, kind)

        # A link follows its target to .ktx. A link may point at another link
        # (dds/foo.dds -> dds/textures/foo.dds -> ...), so what counts as moved
        # grows until nothing changes; renaming only the links that point
        # straight at a converted file left the outer ones dangling.
        resolved = {}
        for info in links:
            text = zin.read(info).decode("utf-8", "replace")
            resolved[info.filename] = (text, os.path.normpath(
                os.path.join(os.path.dirname(info.filename), text)).replace(os.sep, "/"))
        moved = set(converted)
        grew = True
        while grew:
            grew = False
            for name, (text, full) in resolved.items():
                if name not in moved and name.endswith(".dds") and text.endswith(".dds") and full in moved:
                    moved.add(name)
                    grew = True
        for info in links:
            name = info.filename
            text = resolved[name][0]
            target = text.encode("utf-8")
            if name in moved:
                name, target = name[:-3] + "ktx", (text[:-3] + "ktx").encode("utf-8")
            emit(info, name, target, ZIP_STORED, target, "stored")

    zout.close()
    os.replace(outpath + ".part", outpath)
    record = {
        "file": outname, "size": os.path.getsize(outpath), "sha256": sha256_of(outpath),
        "install_as": base, "source": {"file": base, "size": os.path.getsize(pack), "sha256": sha256_of(pack)},
        "textures": "astc-mixed" if textures else "dds", "audio": args.audio,
        "profile": getattr(args, "profile", "custom"),
        "size_policy": "allow-growth" if args.allow_growth else "retain-smaller-original",
    }
    stats[base] = kinds
    log("  %s: %.1f MB -> %.1f MB in %.0f s" % (base, record["source"]["size"] / 1e6, record["size"] / 1e6,
                                               time.time() - t0))
    for k, (n, before, after) in kinds.items():
        if n:
            log("    %-8s %6d entries %9.1f MB -> %9.1f MB" % (k, n, before / 1e6, after / 1e6))
    return record


def find_astcenc():
    if os.environ.get("ASTCENC"):
        return os.environ["ASTCENC"]
    for c in ("astcenc-avx2", "astcenc-sse4.1", "astcenc-sse2", "astcenc"):
        p = shutil.which(c)
        if p:
            return p
    return None


def blocks(s):
    try:
        b = tuple(int(x) for x in s.lower().split("x"))
    except ValueError:
        b = None
    if b not in ASTC_BLOCKS:
        raise argparse.ArgumentTypeError("not a 2D ASTC block size: " + s)
    return b


def _profile_of():
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument("--profile", choices=tuple(PROFILES), default="full")
    return pre.parse_known_args()[0].profile


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("packs", nargs="+")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--textures", choices=("astc", "keep"), default="astc")
    ap.add_argument("--allow-growth", action="store_true",
                    help="prefer converted formats even when an entry grows (default: keep the smaller original)")
    ap.add_argument("--audio", choices=("music", "all", "keep"), default="music",
                    help="what becomes Opus: music (streamed, so no load time cost; default), all sounds "
                         "(sound effects are decoded at map load, which Opus makes slower), or nothing")
    ap.add_argument("--world-blocks", type=blocks, default=(8, 8),
                    help="ASTC blocks of world textures (default %(default)s)")
    ap.add_argument("--model-blocks", type=blocks, default=(6, 6),
                    help="... of dds/models/ (default %(default)s)")
    ap.add_argument("--norm-blocks", type=blocks, default=(5, 5),
                    help="... of normal maps (default %(default)s)")
    ap.add_argument("--gfx-blocks", type=blocks, default=(4, 4),
                    help="... of the 2D graphics, dds/gfx/ (default %(default)s)")
    ap.add_argument("--max-size", type=int, default=1024,
                    help="world and model textures bigger than this start a mip level down "
                         "(default %(default)s; 0: no limit)")
    ap.add_argument("--preset", default="-medium", help="astcenc quality preset (-medium)")
    ap.add_argument("--zstd-level", type=int, default=19)
    ap.add_argument("--music-rate", default="96k")
    ap.add_argument("--sfx-rate", default="64k")
    ap.add_argument("--jpeg-quality", type=int, default=0,
                    help="re-encode JPEGs saved at a higher quality than this (0: leave them; slim: 86)")
    ap.add_argument("--lightmap-quality", type=int, default=0,
                    help="... for the maps' lightmaps, maps/*/lm_*.jpg (0: leave them; slim: 90)")
    ap.add_argument("--drop", action="append", default=[], metavar="GLOB",
                    help="leave out entries matching this glob (a pattern without / matches the file name; "
                         "repeatable)")
    ap.add_argument("--meta", action="append", default=[], metavar="KEY=VALUE",
                    help="extra field for the manifest's meta object (repeatable)")
    ap.add_argument("--limit", type=int, default=0, help="only the first N entries of each pack (tests)")
    # The preset has to be read before the rest, so its values become their defaults.
    profile = _profile_of()
    ap.add_argument("--profile", choices=tuple(PROFILES), default=profile,
                    help="quality/size preset: full (option defaults) or slim (phones; the app downloads this)")
    ap.set_defaults(**PROFILES[profile])
    args = ap.parse_args()

    if not shutil.which("ffmpeg") and (args.textures == "astc" or args.audio != "keep"):
        sys.exit("ffmpeg is needed")
    textures = None
    if args.textures == "astc":
        astcenc = find_astcenc()
        if not astcenc:
            sys.exit("astcenc is needed for --textures astc (set ASTCENC)")
        textures = TextureEncoder(astcenc, args.preset, {"world": args.world_blocks, "model": args.model_blocks,
                                                          "norm": args.norm_blocks, "gfx": args.gfx_blocks}, args.max_size)
    os.makedirs(args.outdir, exist_ok=True)

    records, stats = [], {}
    for pack in args.packs:
        log("%s:" % pack)
        records.append(repack(args, pack, textures, stats))

    with open(os.path.join(args.outdir, "phone-manifest.json"), "w") as f:
        meta = dict(m.split("=", 1) for m in args.meta if "=" in m)
        json.dump({"format": 1, "meta": meta, "packs": records}, f, indent=1)
        f.write("\n")
    with open(os.path.join(args.outdir, "phone-sizes.md"), "w") as f:
        f.write("| pack | official | phone | textures | audio | jpeg | zstd | stored | retained | dropped |\n")
        f.write("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for r in records:
            k = stats[r["install_as"]]
            cell = lambda n: "%.1f -> %.1f" % (k[n][1] / 1e6, k[n][2] / 1e6) if k[n][0] else "-"
            f.write("| %s | %.1f MB | %.1f MB | %s | %s | %s | %s | %s | %s | %s |\n" % (
                r["install_as"], r["source"]["size"] / 1e6, r["size"] / 1e6,
                cell("textures"), cell("audio"), cell("jpeg"), cell("zstd"), cell("stored"), cell("retained"),
                cell("dropped")))


if __name__ == "__main__":
    main()
