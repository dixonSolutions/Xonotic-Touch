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
              else stored; never deflate ourselves. The engine inflates zstd
              entries several times faster (touch_assets_fs.h). Ogg, mp3 and
              other compressed formats are stored as they are. An unchanged
              entry that the source deflates 2% smaller than our own payload
              is copied compressed, so repacking cannot make a pack bigger.

Writes OUTDIR/phone-manifest.json (one record per pack: file name, size,
sha256, the name to install it under and the source pack's name, size and
sha256) and OUTDIR/phone-sizes.md (before and after per pack and per kind).

Needs python 3.14 (compression.zstd), numpy, ffmpeg (with libopus for
--audio music or all) and astcenc 4.x or newer for --textures astc (ASTCENC=path,
else astcenc-avx2, astcenc-sse4.1, astcenc-sse2 or astcenc on PATH).
"""

import argparse
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
STORE_EXT = {"ogg", "opus", "mp3", "pk3", "zip", "gz", "ktx", "webp", "avi", "ogv"}

ZIP_STORED, ZIP_ZSTD = 0, 93
KTX_ID = b"\xabKTX 11\xbb\r\n\x1a\n"
GL_RGB, GL_RGBA = 0x1907, 0x1908
ASTC_BLOCKS = [(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8),
               (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12)]
GL_ASTC_FIRST = 0x93B0  # GL_COMPRESSED_RGBA_ASTC_4x4_KHR


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
        need = 63 if method == ZIP_ZSTD else 10
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


def source_payload(pack, info):
    """The compressed bytes of an entry, as the source zip stores them."""
    with open(pack, "rb") as f:
        f.seek(info.header_offset)
        header = f.read(30)
        if len(header) != 30 or header[:4] != b"PK\x03\x04":
            raise ValueError("bad local header")
        name_len, extra_len = struct.unpack_from("<HH", header, 26)
        f.seek(info.header_offset + 30 + name_len + extra_len)
        payload = f.read(info.compress_size)
    if len(payload) != info.compress_size:
        raise ValueError("short entry")
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
    kinds = {k: [0, 0, 0] for k in ("textures", "audio", "zstd", "stored", "kept")}
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
        except (subprocess.CalledProcessError, OSError, ValueError) as e:
            err = getattr(e, "stderr", b"") or b""
            log("  %s: kept (%s %s)" % (name, e, err.decode(errors="replace").strip()[:200]))
        level = args.zstd_level if len(newdata) <= 32 << 20 else min(args.zstd_level, 15)
        method, payload = pack_entry(newname, newdata, level)
        if kind is None:
            kind = "zstd" if method == ZIP_ZSTD else "stored"
        # small text entries never reach zstd, and storing them raw can be
        # bigger than the deflate the source already had: copy that instead
        if newname == name and kind in ("zstd", "stored") and info.compress_type == zipfile.ZIP_DEFLATED \
                and info.compress_size < len(payload) * 0.98:
            try:
                payload = source_payload(pack, info)
            except (OSError, ValueError) as e:
                log("  %s: source bytes not reused (%s)" % (name, e))
            else:
                method, kind = zipfile.ZIP_DEFLATED, "kept"
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

    with zipfile.ZipFile(pack) as zin, concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        infos = [i for i in zin.infolist() if not i.is_dir()]
        if args.limit:
            infos = infos[:args.limit]
        links = [i for i in infos if is_symlink(i)]
        files = [i for i in infos if not is_symlink(i)]

        def work(info):
            with lock:
                data = zin.read(info)
            return info, convert(info, data)

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

        for info in links:
            target = zin.read(info)
            name = info.filename
            text = target.decode("utf-8", "replace")
            if name.endswith(".dds") and text.endswith(".dds"):
                full = os.path.normpath(os.path.join(os.path.dirname(name), text)).replace(os.sep, "/")
                if full in converted:
                    name, target = name[:-3] + "ktx", (text[:-3] + "ktx").encode()
            emit(info, name, target, ZIP_STORED, target, "stored")

    zout.close()
    os.replace(outpath + ".part", outpath)
    record = {
        "file": outname, "size": os.path.getsize(outpath), "sha256": sha256_of(outpath),
        "install_as": base, "source": {"file": base, "size": os.path.getsize(pack), "sha256": sha256_of(pack)},
        "textures": "astc" if textures else "dds", "audio": args.audio,
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("packs", nargs="+")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--textures", choices=("astc", "keep"), default="astc")
    ap.add_argument("--audio", choices=("music", "all", "keep"), default="music",
                    help="what becomes Opus: music (streamed, so no load time cost; default), all sounds "
                         "(sound effects are decoded at map load, which Opus makes slower), or nothing")
    ap.add_argument("--world-blocks", type=blocks, default=(8, 8), help="ASTC blocks of world textures (8x8)")
    ap.add_argument("--model-blocks", type=blocks, default=(6, 6), help="... of dds/models/ (6x6)")
    ap.add_argument("--norm-blocks", type=blocks, default=(5, 5), help="... of normal maps (5x5)")
    ap.add_argument("--gfx-blocks", type=blocks, default=(4, 4), help="... of the 2D graphics, dds/gfx/ (4x4)")
    ap.add_argument("--max-size", type=int, default=1024,
                    help="world and model textures bigger than this start a mip level down (1024; 0: no limit)")
    ap.add_argument("--preset", default="-medium", help="astcenc quality preset (-medium)")
    ap.add_argument("--zstd-level", type=int, default=19)
    ap.add_argument("--music-rate", default="96k")
    ap.add_argument("--sfx-rate", default="64k")
    ap.add_argument("--meta", action="append", default=[], metavar="KEY=VALUE",
                    help="extra field for the manifest's meta object (repeatable)")
    ap.add_argument("--limit", type=int, default=0, help="only the first N entries of each pack (tests)")
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
        f.write("| pack | official | phone | textures | audio | zstd | stored | kept |\n")
        f.write("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for r in records:
            k = stats[r["install_as"]]
            cell = lambda n: "%.1f -> %.1f" % (k[n][1] / 1e6, k[n][2] / 1e6) if k[n][0] else "-"
            f.write("| %s | %.1f MB | %.1f MB | %s | %s | %s | %s | %s |\n" % (
                r["install_as"], r["source"]["size"] / 1e6, r["size"] / 1e6,
                cell("textures"), cell("audio"), cell("zstd"), cell("stored"), cell("kept")))


if __name__ == "__main__":
    main()
