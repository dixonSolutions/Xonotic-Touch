// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

/**
 * Reads single files out of a pk3 (a zip) without unpacking it: the map
 * previews (`maps/<name>.jpg`), mapinfo, campaign and credits text. Entries
 * may be stored (0), deflated (8), or zstd (93, the slim data set's method,
 * touch_assets_zstd.c); the platform supplies the decoders.
 */
object Pk3 {
    data class Entry(val name: String, val method: Int, val compressedSize: Long, val size: Long, val localHeader: Long)

    interface Decoders {
        /** Raw deflate (no zlib header) to exactly [size] bytes; null on damage. */
        fun inflate(data: ByteArray, size: Int): ByteArray?
        /** A zstd frame to exactly [size] bytes; null when unsupported or damaged. */
        fun zstd(data: ByteArray, size: Int): ByteArray?
    }

    const val STORED = 0
    const val DEFLATE = 8
    const val ZSTD = 93

    /** The central directory, or empty when it is not a zip (or zip64, which pk3s never are). */
    fun entries(src: Zip.Source): List<Entry> {
        val tailLen = minOf(src.size, 65_557L).toInt()
        if (tailLen < 22) return emptyList()
        val tail = src.read(src.size - tailLen, tailLen)
        var eocd = -1
        for (i in tail.size - 22 downTo 0) if (u32(tail, i) == 0x06054b50L) { eocd = i; break }
        if (eocd < 0) return emptyList()
        val cdSize = u32(tail, eocd + 12)
        val cdOffset = u32(tail, eocd + 16)
        if (cdSize <= 0 || cdOffset + cdSize > src.size || cdSize > 64L * 1024 * 1024) return emptyList()
        val cd = src.read(cdOffset, cdSize.toInt())
        val out = ArrayList<Entry>()
        var p = 0
        while (p + 46 <= cd.size && u32(cd, p) == 0x02014b50L) {
            val method = u16(cd, p + 10)
            val csize = u32(cd, p + 20)
            val usize = u32(cd, p + 24)
            val nameLen = u16(cd, p + 28)
            val extraLen = u16(cd, p + 30)
            val commentLen = u16(cd, p + 32)
            val local = u32(cd, p + 42)
            if (p + 46 + nameLen > cd.size) break
            out += Entry(cd.decodeToString(p + 46, p + 46 + nameLen), method, csize, usize, local)
            p += 46 + nameLen + extraLen + commentLen
        }
        return out
    }

    /** One entry's bytes; null when damaged, too large ([limit]) or of an unknown method. */
    fun read(src: Zip.Source, e: Entry, dec: Decoders, limit: Int = 16 shl 20): ByteArray? {
        if (e.size > limit || e.compressedSize > limit) return null
        val lh = src.read(e.localHeader, 30)
        if (lh.size < 30 || u32(lh, 0) != 0x04034b50L) return null
        val start = e.localHeader + 30 + u16(lh, 26) + u16(lh, 28)
        if (start + e.compressedSize > src.size) return null
        val data = src.read(start, e.compressedSize.toInt())
        return when (e.method) {
            STORED -> data.takeIf { it.size.toLong() == e.size }
            DEFLATE -> dec.inflate(data, e.size.toInt())
            ZSTD -> dec.zstd(data, e.size.toInt())
            else -> null
        }
    }

    /** Case-insensitive lookup, as the engine's filesystem does. */
    fun find(entries: List<Entry>, name: String): Entry? = entries.firstOrNull { it.name.equals(name, ignoreCase = true) }

    private fun u16(b: ByteArray, i: Int) = (b[i].toInt() and 0xff) or ((b[i + 1].toInt() and 0xff) shl 8)
    private fun u32(b: ByteArray, i: Int): Long = (u16(b, i).toLong()) or (u16(b, i + 2).toLong() shl 16)
}

/**
 * Everything the app knows about a map: from the pk3 directory listing
 * (cheap, at start) and its mapinfo (read when the picker shows it).
 */
data class MapEntry(
    val name: String,
    /** The pk3 (or pk3dir) holding maps/<name>.bsp. */
    val pack: String,
    val hasPreview: Boolean,
    val info: MapInfoFile.Info? = null,
) {
    val title get() = info?.title?.takeIf { it.isNotBlank() && it != name } ?: name
    fun supports(gametype: String) = info == null || info.gametypes.isEmpty() || gametype.lowercase() in info.gametypes
}

/** Maps listed from the packs' directories: `maps/<x>.bsp`, with `maps/<x>.jpg` noted. */
object MapCatalog {
    fun fromEntries(pack: String, names: Iterable<String>): List<MapEntry> {
        val lower = names.map { it.lowercase() }.toSet()
        return names.mapNotNull { Zip.mapName(it) }.distinctBy { it.lowercase() }.map { m ->
            MapEntry(m, pack, "maps/${m.lowercase()}.jpg" in lower)
        }
    }

    /** Later packs win (the engine's search order puts the last-loaded pack first); then sorted by name. */
    fun merge(lists: List<List<MapEntry>>): List<MapEntry> {
        val by = LinkedHashMap<String, MapEntry>()
        for (l in lists) for (m in l) by[m.name.lowercase()] = m
        return by.values.sortedBy { it.name.lowercase() }
    }
}
