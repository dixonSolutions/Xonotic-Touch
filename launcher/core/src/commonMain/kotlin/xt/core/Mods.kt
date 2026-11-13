// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

/**
 * A Lua client mod (<userdir>/mods/<name>/) or server plugin
 * (<userdir>/plugins/<name>/), as the engine's mod manager
 * (engine/darkplaces/touch_lua.c) sees them: a folder with init.lua, an
 * optional mod.txt of `key: value` lines, and the folder's name listed in
 * enabled.txt next to the folders when it is on. A new folder starts off.
 */
data class ModEntry(
    val name: String,
    val title: String,
    val version: String,
    val author: String,
    val description: String,
    val enabled: Boolean,
    val hasInit: Boolean,
) {
    val subtitle: String
        get() = buildList {
            if (version.isNotEmpty()) add("v$version")
            if (author.isNotEmpty()) add(author)
            if (!hasInit) add("no init.lua")
        }.joinToString(" · ").let { meta ->
            when {
                description.isEmpty() -> meta
                meta.isEmpty() -> description
                else -> "$meta — $description"
            }
        }
}

object Mods {
    data class Info(val title: String, val version: String, val author: String, val description: String)

    fun parseInfo(text: String): Info {
        val m = mutableMapOf<String, String>()
        for (line in text.lineSequence()) {
            val colon = line.indexOf(':')
            if (colon <= 0) continue
            m[line.substring(0, colon).trim().lowercase()] = line.substring(colon + 1).trim()
        }
        return Info(m["title"].orEmpty(), m["version"].orEmpty(), m["author"].orEmpty(), m["description"].orEmpty())
    }

    fun parseEnabled(text: String): Set<String> =
        text.lineSequence().map { it.trim().substringBefore(' ').substringBefore('\t') }.filter { it.isNotEmpty() }.toSet()

    /** Apply one change to the complete on-disk list, retaining unavailable mods.
     * A failed read or write never reports success to the UI. */
    fun setEnabled(name: String, on: Boolean, read: () -> String?, write: (String) -> Boolean): Boolean {
        val current = read() ?: return false
        val names = parseEnabled(current).toMutableSet()
        if (on) names.add(name) else names.remove(name)
        return write(serializeEnabled(names))
    }

    /** enabled.txt as the engine writes it: one folder name per line. */
    fun serializeEnabled(names: Collection<String>): String = names.sorted().joinToString("") { "$it\n" }

    /** Folder names the engine accepts: no dots first, no path separators. */
    fun isModName(name: String) = name.isNotEmpty() && !name.startsWith(".") && '/' !in name && '\\' !in name

    fun entry(name: String, info: Info?, enabled: Set<String>, hasInit: Boolean) = ModEntry(
        name = name,
        title = info?.title?.takeIf { it.isNotEmpty() } ?: name,
        version = info?.version.orEmpty(),
        author = info?.author.orEmpty(),
        description = info?.description.orEmpty(),
        enabled = name in enabled,
        hasInit = hasInit,
    )
}

/** Reads a zip's central directory: enough to list a pk3's maps without unpacking it. */
object Zip {
    interface Source {
        val size: Long
        fun read(offset: Long, length: Int): ByteArray
    }

    fun entryNames(src: Source): List<String> {
        val tailLen = minOf(src.size, 65_557L).toInt()
        if (tailLen < 22) return emptyList()
        val tail = src.read(src.size - tailLen, tailLen)
        var eocd = -1
        for (i in tail.size - 22 downTo 0) {
            if (u32(tail, i) == 0x06054b50L) { eocd = i; break }
        }
        if (eocd < 0) return emptyList()
        val count = u16(tail, eocd + 10)
        val cdSize = u32(tail, eocd + 12)
        val cdOffset = u32(tail, eocd + 16)
        if (cdSize <= 0 || cdOffset + cdSize > src.size || cdSize > 64L * 1024 * 1024) return emptyList()
        val cd = src.read(cdOffset, cdSize.toInt())
        val names = ArrayList<String>(count)
        var p = 0
        while (p + 46 <= cd.size && u32(cd, p) == 0x02014b50L) {
            val nameLen = u16(cd, p + 28)
            val extraLen = u16(cd, p + 30)
            val commentLen = u16(cd, p + 32)
            if (p + 46 + nameLen > cd.size) break
            names += cd.decodeToString(p + 46, p + 46 + nameLen)
            p += 46 + nameLen + extraLen + commentLen
        }
        return names
    }

    /** "maps/stormkeep.bsp" -> "stormkeep". */
    fun mapNames(entries: Iterable<String>): List<String> = entries.mapNotNull { mapName(it) }

    fun mapName(entry: String): String? {
        val e = entry.lowercase()
        if (!e.startsWith("maps/") || !e.endsWith(".bsp")) return null
        val base = entry.substring(5, entry.length - 4)
        return base.takeIf { it.isNotEmpty() && '/' !in it }
    }

    private fun u16(b: ByteArray, i: Int) = (b[i].toInt() and 0xff) or ((b[i + 1].toInt() and 0xff) shl 8)
    private fun u32(b: ByteArray, i: Int): Long =
        (u16(b, i).toLong()) or (u16(b, i + 2).toLong() shl 16)
}
