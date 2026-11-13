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
             "max_size": 512, "music_rate": "64k", "sfx": "opus", "bsp_zstd_level": 22,
             "jpeg_quality": 86, "lightmap_quality": 90,
             "drop": ["maps/*.map", "models/*.map", "*.ase", "*.lno", "demos/*.dem", "*.mp3"]},
}
# The set for GPUs with ASTC, by what it is (the app's "slim" set).
PROFILES["astc"] = PROFILES["slim"]
# The same set for GPUs without ASTC (desktop NVIDIA/AMD: the web build and
# desktop Linux pick it by GPU): everything above that does not depend on
# ASTC -- Opus sound effects and music, re-encoded JPEGs, zstd entries, the
# files a running game never opens left out -- with the official DXT
# textures kept as they are.
PROFILES["dxt"] = dict(PROFILES["slim"], textures="keep")


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


# --------------------------------------------------------- sound effects
#
# --sfx opus: every Vorbis sound effect is tried as Opus at a ladder of
# bitrates and the smallest one that stays measurably as close to the source
# as Opus at a generous rate does is kept; a sound none passes, or where Opus
# is not a tenth smaller, stays Vorbis. The engine decodes Opus at 48 kHz
# (touch_assets_opus.c), so the file is made from the PCM libvorbisfile (the
# engine's own Vorbis decoder) gives, resampled to 48 kHz with soxr, and its
# Ogg stream is patched so it decodes to exactly round(n * 48000 / rate)
# samples: ffmpeg's own Vorbis-in, Opus-out path came out up to 674 samples
# short or 1411 long. LOOP_* comments are rescaled to 48 kHz samples and the
# OpusHead input rate set to 48000 so the engine's scaling is 1. A sound that
# loops (a LOOP_START comment, or a looped target_speaker in a map) is encoded
# with 100 ms of its own end before it and of its start after it, which the
# pre-skip and end trim drop: the codec sees the loop as continuous, so the
# seam costs no more than the inside does.

SFX_MONO = (32, 40, 48, 56, 64)         # kbit/s tried for mono, smallest first
SFX_STEREO = (48, 56, 64, 72, 80)
SFX_REF = {1: 96, 2: 128}               # the yardstick: Opus at a generous rate
SFX_DUALMONO_DB = -30.0                 # side channel this far under mid: mono
# How much further than the yardstick a rung may be from the source:
SFX_MAX_BAND = 0.45   # mean |dB| over 25 critical bands, active frames
SFX_MAX_ENV = 0.35    # mean |dB| of the 2 ms energy envelope
SFX_MAX_LOUD = 0.25   # dB of total energy above 100 Hz
SFX_MAX_PRE = 1.5     # dB more energy than the source before the first onset
SFX_MAX_SEAM = 3.0    # dB: error at a loop's ends over the error inside

_vorbisfile = None


def _vf():
    """libvorbisfile through ctypes (VORBISFILE=path to choose one)."""
    global _vorbisfile
    if _vorbisfile is None:
        import ctypes as C
        import ctypes.util
        lib = C.CDLL(os.environ.get("VORBISFILE") or ctypes.util.find_library("vorbisfile") or "libvorbisfile.so.3")
        lib.ov_fopen.argtypes = [C.c_char_p, C.c_void_p]
        lib.ov_read.argtypes = [C.c_void_p, C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_int, C.c_void_p]
        lib.ov_read.restype = C.c_long
        lib.ov_info.restype = C.c_void_p
        lib.ov_info.argtypes = [C.c_void_p, C.c_int]
        lib.ov_comment.restype = C.c_void_p
        lib.ov_comment.argtypes = [C.c_void_p, C.c_int]
        lib.ov_clear.argtypes = [C.c_void_p]
        _vorbisfile = lib
    return _vorbisfile


def decode_vorbis(data):
    """(int16 PCM [n, channels], rate, comments) as the engine decodes it."""
    import ctypes as C

    class Info(C.Structure):
        _fields_ = [("version", C.c_int), ("channels", C.c_int), ("rate", C.c_long)]

    class Comment(C.Structure):
        _fields_ = [("user_comments", C.POINTER(C.c_char_p)), ("comment_lengths", C.POINTER(C.c_int)),
                    ("comments", C.c_int), ("vendor", C.c_char_p)]

    lib = _vf()
    with tempfile.NamedTemporaryFile(suffix=".ogg") as t:
        t.write(data)
        t.flush()
        vf = C.create_string_buffer(8192)  # OggVorbis_File is ~1 KB
        if lib.ov_fopen(t.name.encode(), vf) != 0:
            raise ValueError("not Ogg Vorbis")
        try:
            info = Info.from_address(lib.ov_info(vf, -1))
            channels, rate = info.channels, info.rate
            vc = Comment.from_address(lib.ov_comment(vf, -1))
            tags = [vc.user_comments[i].decode("utf-8", "replace") for i in range(vc.comments)]
            buf, bs, out = C.create_string_buffer(65536), C.c_int(0), []
            while True:
                n = lib.ov_read(vf, buf, 65536, 0, 2, 1, C.byref(bs))
                if n < 0:
                    raise ValueError("Vorbis decode error %d" % n)
                if n == 0:
                    break
                out.append(buf.raw[:n])
        finally:
            lib.ov_clear(vf)
    if channels not in (1, 2):
        raise ValueError("%d channels" % channels)
    return np.frombuffer(b"".join(out), np.int16).reshape(-1, channels), rate, tags


def to_48k(pcm, rate):
    """float32 [m, ch] at 48 kHz with m == round(n * 48000 / rate)."""
    n, ch = pcm.shape
    m = int(round(n * 48000 / rate))
    if rate == 48000:
        return pcm.astype(np.float32) / 32768.0
    raw = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-threads", "1", "-f", "s16le", "-ar", str(rate),
                          "-ac", str(ch), "-i", "-", "-af", "aresample=48000:resampler=soxr:precision=28",
                          "-f", "f32le", "-"], input=pcm.tobytes(), capture_output=True, check=True).stdout
    y = np.frombuffer(raw, np.float32).reshape(-1, ch)
    if len(y) >= m:
        return y[:m].copy()
    return np.concatenate([y, np.zeros((m - len(y), ch), np.float32)])


def _crc_table():
    t = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        t.append(r & 0xFFFFFFFF)
    return t


_OGG_CRC = _crc_table()


def _ogg_seal(page):
    """Fill in an Ogg page's CRC."""
    page[22:26] = b"\0\0\0\0"
    c = 0
    for b in page:
        c = ((c << 8) & 0xFFFFFFFF) ^ _OGG_CRC[((c >> 24) & 0xFF) ^ b]
    page[22:26] = struct.pack("<I", c)


def ogg_pages(data):
    pages, i = [], 0
    while i < len(data):
        if data[i:i + 4] != b"OggS" or i + 27 > len(data):
            raise ValueError("bad Ogg page at %d" % i)
        nseg = data[i + 26]
        size = 27 + nseg + sum(data[i + 27:i + 27 + nseg])
        pages.append(bytearray(data[i:i + size]))
        i += size
    return pages


def _ogg_page(header_type, granule, serial, seq, packet):
    lacing = [255] * (len(packet) // 255) + [len(packet) % 255]
    if len(lacing) > 255:
        raise ValueError("comment header too long")
    p = bytearray(b"OggS" + struct.pack("<BBqIII", 0, header_type, granule, serial, seq, 0)
                  + bytes([len(lacing)]) + bytes(lacing) + packet)
    _ogg_seal(p)
    return p


def patch_opus(data, extra_preskip, length, tags):
    """Raise the pre-skip by extra_preskip, end the stream so exactly `length`
    samples decode, set the input rate to 48000, and write `tags` as the
    comment header."""
    pages = ogg_pages(data)
    head = bytearray(pages[0][27 + pages[0][26]:])
    if head[:8] != b"OpusHead":
        raise ValueError("not Opus")
    pre = struct.unpack_from("<H", head, 10)[0] + extra_preskip
    if pre > 65535:
        raise ValueError("pre-skip")
    struct.pack_into("<H", head, 10, pre)
    struct.pack_into("<I", head, 12, 48000)
    serial = struct.unpack_from("<I", pages[0], 14)[0]
    k = 1  # header pages carry granule 0
    while k < len(pages) and struct.unpack_from("<q", pages[k], 6)[0] == 0 and not pages[k][5] & 4:
        k += 1
    vendor = b"Godnotic"
    comment = [b"OpusTags", struct.pack("<I", len(vendor)), vendor, struct.pack("<I", len(tags))]
    for t in tags:
        b = t.encode("utf-8")
        comment += [struct.pack("<I", len(b)), b]
    out = [_ogg_page(2, 0, serial, 0, bytes(head)), _ogg_page(0, 0, serial, 1, b"".join(comment))]
    end = pre + length
    audio = pages[k:]
    if not audio or struct.unpack_from("<q", audio[-1], 6)[0] < end:
        raise ValueError("stream shorter than wanted")
    for j, p in enumerate(audio):
        struct.pack_into("<I", p, 18, 2 + j)
        if struct.unpack_from("<q", p, 6)[0] >= end:
            # the page the end falls in becomes the last: its granule
            # position trims whatever decodes beyond the end
            struct.pack_into("<q", p, 6, end)
            p[5] |= 4
            _ogg_seal(p)
            out.append(p)
            break
        _ogg_seal(p)
        out.append(p)
    return b"".join(bytes(p) for p in out)


def encode_opus(pcm48, kbps, loop, tags):
    """Ogg Opus of float32 [m, ch] 48 kHz PCM that decodes to exactly m samples."""
    m, ch = pcm48.shape
    pad = min(m, 4800) if loop else 0
    x = np.concatenate([pcm48[m - pad:], pcm48, pcm48[:pad]]) if pad else pcm48
    r = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-threads", "1", "-f", "f32le", "-ar", "48000",
                        "-ac", str(ch), "-i", "-", "-c:a", "libopus", "-b:a", "%dk" % kbps, "-vbr", "on",
                        "-compression_level", "10", "-application", "audio", "-f", "ogg", "-"],
                       input=x.astype(np.float32).tobytes(), capture_output=True, check=True).stdout
    return patch_opus(r, pad, m, tags)


def decode_opus(data, ch):
    """float32 [m, ch] through libopus, the decoder the engine uses."""
    with tempfile.NamedTemporaryFile(suffix=".opus") as t:
        t.write(data)
        t.flush()
        raw = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-threads", "1", "-c:a", "libopus", "-i", t.name,
                              "-f", "f32le", "-ac", str(ch), "-ar", "48000", "-"],
                             capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32).reshape(-1, ch)


_BAND_EDGES = [0, 100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700, 3150,
               3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 20000]


def _bands(x):
    m = x.mean(1)
    n, hop = 1024, 256
    if len(m) < n:
        m = np.pad(m, (0, n - len(m)))
    frames = np.lib.stride_tricks.sliding_window_view(m, n)[::hop] * np.hanning(n)
    power = np.abs(np.fft.rfft(frames, axis=1)) ** 2
    f = np.fft.rfftfreq(n, 1 / 48000)
    return np.stack([power[:, (f >= a) & (f < b)].sum(1) for a, b in zip(_BAND_EDGES[:-1], _BAND_EDGES[1:])], 1)


def _envelope(x, w=96):
    e = (x ** 2).mean(1)
    k = len(e) // w
    if k == 0:
        return np.array([10 * np.log10(e.mean() + 1e-12)])
    return 10 * np.log10(e[:k * w].reshape(k, w).mean(1) + 1e-12)


def sound_distance(ref, got, loop):
    """How far a decoded sound is from its source, in the measures SFX_MAX_* bound."""
    n = min(len(ref), len(got))
    r, t = ref[:n], got[:n]
    br, bt = _bands(r), _bands(t)
    out = {"loud": 10 * np.log10((bt[:, 1:].sum() + 1e-12) / (br[:, 1:].sum() + 1e-12))}
    total = br.sum(1)
    active = total > total.max() * 1e-4 if total.max() > 0 else total > 0
    dr, dt = 10 * np.log10(br + 1e-10), 10 * np.log10(bt + 1e-10)
    mask = (dr > dr.max(1, keepdims=True) - 50) & active[:, None]
    out["band"] = float(np.abs(dr - dt)[mask].mean()) if mask.any() else 0.0
    er, et = _envelope(r), _envelope(t)
    emask = er > er.max() - 40
    out["env"] = float(np.abs(er - et)[emask].mean()) if emask.any() else 0.0
    peak = np.abs(r).max()
    onset = int(np.argmax(np.abs(r).max(1) > peak * 0.1)) if peak > 0 else 0
    out["pre"] = None
    if onset > 480:
        a, b = float((r[:onset] ** 2).sum()), float((t[:onset] ** 2).sum())
        out["pre"] = 10 * np.log10((b + 1e-12) / (a + 1e-12))
    out["seam"] = None
    e = 480
    if loop and n > 4 * e:
        err = ((t - r) ** 2).mean(1)
        out["seam"] = float(10 * np.log10((err[:e].mean() + err[-e:].mean()) / 2 + 1e-12)
                            - 10 * np.log10(err[e:-e].mean() + 1e-12))
    out["lendiff"] = len(got) - len(ref)
    return out


def whole_loop(tags, n):
    """Whether LOOP_* comments loop the whole file (start 0, end at the end)."""
    t = {}
    for c in tags:
        k, _, v = c.partition("=")
        try:
            t[k.upper()] = int(v)
        except ValueError:
            pass
    if t.get("LOOP_START") != 0:
        return False
    end = t.get("LOOP_END", t.get("LOOP_LENGTH", n))
    return end >= n


def opus_tags(tags, rate):
    """The comments the engine reads, LOOP_* counted in 48 kHz samples."""
    keep = []
    for t in tags:
        k, _, v = t.partition("=")
        if k.upper() in ("LOOP_START", "LOOP_END", "LOOP_LENGTH"):
            try:
                keep.append("%s=%d" % (k, round(int(v) * 48000 / rate)))
            except ValueError:
                pass
        elif k.upper().startswith("REPLAYGAIN_"):
            keep.append(t)
    return keep


def music_to_opus(name, data, kbps):
    """A music track as exact-length Opus (streamed, so no ladder): the old
    ffmpeg path kept LOOP_* in source samples but wrote a 48000 input rate,
    which moved the loop points of every 44.1 kHz track (nanite: its last
    15 s never played)."""
    pcm, rate, tags = decode_vorbis(data)
    ref = to_48k(pcm, rate)
    opus = encode_opus(ref, kbps, whole_loop(tags, len(pcm)), opus_tags(tags, rate))
    got = decode_opus(opus, ref.shape[1])
    if len(got) != len(ref):
        raise ValueError("decodes to %+d samples" % (len(got) - len(ref)))
    return opus if len(opus) < len(data) else None


def sfx_to_opus(name, data, loops):
    """(Opus bytes or None to keep the Vorbis file, report dict)."""
    pcm, rate, tags = decode_vorbis(data)
    ref = to_48k(pcm, rate)
    ch = ref.shape[1]
    if ch == 2:
        mid, side = ref.mean(1), (ref[:, 0] - ref[:, 1]) / 2
        if 10 * np.log10(((side ** 2).mean() + 1e-12) / ((mid ** 2).mean() + 1e-12)) < SFX_DUALMONO_DB:
            ref, ch = ref.mean(1, keepdims=True), 1  # the engine plays L == R stereo exactly like mono
    loop = whole_loop(tags, len(pcm)) or name.lower() in loops
    keep = opus_tags(tags, rate)
    report = {"name": name, "rate": rate, "channels": pcm.shape[1], "out_channels": ch, "loop": loop,
              "vorbis": len(data), "kbps": None, "opus": None}

    def attempt(kbps):
        opus = encode_opus(ref, kbps, loop, keep)
        got = decode_opus(opus, ch)
        d = sound_distance(ref, got, loop)
        if d["lendiff"]:
            raise ValueError("decodes to %+d samples" % d["lendiff"])
        return opus, d

    _, yard = attempt(SFX_REF[ch])
    for kbps in (SFX_MONO if ch == 1 else SFX_STEREO):
        opus, d = attempt(kbps)
        if len(opus) > 0.9 * len(data):
            break  # higher rates are only bigger
        if (d["band"] - yard["band"] > SFX_MAX_BAND or d["env"] - yard["env"] > SFX_MAX_ENV
                or abs(d["loud"] - yard["loud"]) > SFX_MAX_LOUD
                or (d["pre"] is not None and yard["pre"] is not None and d["pre"] - yard["pre"] > SFX_MAX_PRE)
                or (d["seam"] is not None and d["seam"] > SFX_MAX_SEAM)):
            continue
        report.update(kbps=kbps, opus=len(opus), band=round(d["band"] - yard["band"], 2),
                      env=round(d["env"] - yard["env"], 2), loud=round(d["loud"] - yard["loud"], 2))
        return opus, report
    return None, report


def looped_speakers(packs):
    """Sounds that maps in these packs play as looped target_speakers (the
    engine loops them from the start, whatever the file says)."""
    import re
    loops = set()
    for pack in packs:
        with zipfile.ZipFile(pack) as z:
            for name in z.namelist():
                if not (name.startswith("maps/") and name.endswith(".bsp")):
                    continue
                d = z.read(name)
                if d[:4] != b"IBSP" or len(d) < 16:
                    continue
                off, ln = struct.unpack_from("<ii", d, 8)
                for blk in re.findall(r"\{[^{}]*\}", d[off:off + ln].decode("latin1")):
                    kv = dict(re.findall(r'"([^"]+)"\s+"([^"]*)"', blk))
                    try:
                        flags = int(float(kv.get("spawnflags", "0") or 0))
                    except ValueError:
                        flags = 0
                    if kv.get("classname") == "target_speaker" and flags & 3 and kv.get("noise"):
                        s = kv["noise"].lower()
                        s = s if s.startswith("sound/") else "sound/" + s
                        loops.add(s.rsplit(".", 1)[0] + ".ogg")
    return loops


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
    sfx_reports = stats.setdefault("_sfx", [])

    def convert(info, data):
        name = info.filename
        ext = ext_of(name)
        newname, newdata, kind = name, data, None
        try:
            if ext == "dds" and textures and name.startswith("dds/"):
                ktx = textures.encode(name, data)
                if ktx is not None:
                    newname, newdata, kind = name[:-3] + "ktx", ktx, "textures"
            elif ext == "ogg" and not is_music(name) and getattr(args, "sfx", "keep") == "opus":
                opus, report = sfx_to_opus(name, data, getattr(args, "loops", set()))
                with lock:
                    sfx_reports.append(report)
                if opus is not None:
                    newname, newdata, kind = name, opus, "audio"
            elif ext == "ogg" and is_music(name) and args.audio in ("music", "all"):
                opus = music_to_opus(name, data, int(args.music_rate.rstrip("kK")))
                if opus is not None:
                    newname, newdata, kind = name, opus, "audio"
            elif ext == "ogg" and args.audio == "all":
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
        if newname.endswith(".bsp") and getattr(args, "bsp_zstd_level", 0):
            level = args.bsp_zstd_level
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
    ap.add_argument("--sfx-rate", default="64k", help="bitrate of sound effects for --audio all")
    ap.add_argument("--sfx", choices=("keep", "opus"), default="keep",
                    help="opus: each Vorbis sound effect becomes Opus at the lowest bitrate that measures as close "
                         "to the source as Opus at a generous rate, or stays Vorbis (slim: opus)")
    ap.add_argument("--bsp-zstd-level", type=int, default=0, help="zstd level for maps/*.bsp (0: --zstd-level)")
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
    args.loops = set()
    if args.sfx == "opus":
        _vf()  # fail early without libvorbisfile
        args.loops = looped_speakers(args.packs)

    records, stats = [], {}
    for pack in args.packs:
        log("%s:" % pack)
        records.append(repack(args, pack, textures, stats))

    with open(os.path.join(args.outdir, "phone-manifest.json"), "w") as f:
        meta = dict(m.split("=", 1) for m in args.meta if "=" in m)
        json.dump({"format": 1, "meta": meta, "packs": records}, f, indent=1)
        f.write("\n")
    sfx = stats.pop("_sfx", [])
    if sfx:
        with open(os.path.join(args.outdir, "phone-sfx.tsv"), "w") as f:
            cols = ("name", "rate", "channels", "out_channels", "loop", "vorbis", "kbps", "opus", "band", "env", "loud")
            f.write("\t".join(cols) + "\n")
            for r in sorted(sfx, key=lambda r: r["name"]):
                f.write("\t".join("" if r.get(c) is None else str(r.get(c)) for c in cols) + "\n")
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
