// Touch: compact, fast-loading assets for phones (see docs/PHONE_ASSETS.md).
//
// The phone pack set (scripts/repack-phone-assets.py) differs from the
// official packs in three ways, each read by one piece here:
//
//   - compressible pk3 entries are stored with zstd (zip method 93) instead of
//     deflate: touch_assets_fs.h, included by fs.c, decodes them,
//   - DXT textures (dds/foo.dds) also come as ASTC in a KTX 1 file
//     (dds/foo.ktx): touch_assets_ktx.h, included by gl_textures.c, uploads
//     them as they are when the GPU takes ASTC,
//   - Ogg Vorbis sound becomes Ogg Opus under the same name: touch_assets_opus.c
//     decodes it (and foo.opus when foo.ogg is missing).
//
// Everything falls back to the official formats, so the same engine runs the
// official packs and the phone packs.

#ifndef TOUCH_ASSETS_H
#define TOUCH_ASSETS_H

#include <stddef.h>

/// zip compression method of a zstd entry (APPNOTE 4.4.5)
#define TOUCH_ZIP_METHOD_ZSTD 93

/// Decode one whole zstd frame of srcsize bytes into dst, which holds exactly
/// dstsize bytes. Returns NULL on success, else a short reason.
const char *Touch_Zstd_Decompress(void *dst, size_t dstsize, const void *src, size_t srcsize);

/// Ogg Opus sound: "filename" is a .ogg the game asked for. When it holds
/// Opus, or is missing and the same name with .opus exists, and an Opus
/// decoder is around, load that into sfx and return true. OGG_LoadVorbisFile
/// calls it first (touch_assets_opus.c).
struct sfx_s;
qbool Touch_Opus_LoadFile(const char *filename, struct sfx_s *sfx);

#endif
