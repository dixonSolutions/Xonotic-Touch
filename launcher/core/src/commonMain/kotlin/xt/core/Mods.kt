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
    /** Scopes mod.txt asks for and the ones granted (docs/PLUGIN_STANDARD.md §5); empty when unknown. */
    val requested: List<String> = emptyList(),
    val granted: Set<String> = emptySet(),
) {
    val missing: List<String> get() = requested.filter { it !in granted }

    val subtitle: String
        get() = buildList {
            if (version.isNotEmpty()) add("v$version")
            if (author.isNotEmpty()) add(author)
            if (!hasInit) add("no init.lua")
            if (missing.isNotEmpty()) add("${missing.size} of ${requested.size} permissions not granted")
        }.joinToString(" · ").let { meta ->
            when {
                description.isEmpty() -> meta
                meta.isEmpty() -> description
                else -> "$meta — $description"
            }
        }
}

object Mods {
    data class Info(
        val title: String,
        val version: String,
        val author: String,
        val description: String,
        val api: String = "",
        val permissions: List<String> = emptyList(),
    )

    /** The permission scopes in the engine's bit order (touch_lua.c, tlua_permissions). */
    val SCOPES = listOf(
        "events", "commands", "cvar_read", "cvar_write", "exec", "storage",
        "display", "input", "players", "messages", "state", "hooks", "hooks_write",
    )

    /** What a mod with no api: line (legacy) is granted until the user grants more. */
    val LEGACY_DEFAULT = setOf("events", "commands", "cvar_read", "display", "input", "players", "state")

    /** The scopes a manifest asks for: its permissions: line, or the API 1 set for a legacy mod. */
    fun requested(info: Info?): List<String> =
        if (info == null || info.api.isEmpty()) SCOPES.take(11) else info.permissions.filter { it in SCOPES }

    /**
     * luastate/<realm>/grants/<name>.txt: "v1 <hex mask>" (or "legacy <hex mask>"). A missing
     * file means nothing (API 1+) or the HUD set (legacy); a malformed one means nothing.
     */
    fun parseGrants(text: String?, scoped: Boolean): Set<String> {
        if (text == null) return if (scoped) emptySet() else LEGACY_DEFAULT
        val parts = text.trim().split(Regex("\\s+"))
        if (parts.size != 2 || parts[0] != (if (scoped) "v1" else "legacy")) return emptySet()
        val mask = parts[1].toLongOrNull(16) ?: return emptySet()
        return SCOPES.filterIndexed { i, _ -> mask and (1L shl i) != 0L }.toSet()
    }

    /**
     * Asks the game to install a package: it reads <userdir>/luastate/incoming/request.txt once
     * at start, shows the review in its Mods tab, and installs nothing until the user confirms
     * (touch_lua_pkg.c). The launcher never unpacks or enables anything itself.
     */
    fun installRequest(plugin: Boolean, path: String): String? =
        if ('\n' in path || '\r' in path || '"' in path || ';' in path) null
        else (if (plugin) "plugin " else "mod ") + path + "\n"

    fun parseInfo(text: String): Info {
        val m = mutableMapOf<String, String>()
        for (line in text.lineSequence()) {
            val colon = line.indexOf(':')
            if (colon <= 0) continue
            m[line.substring(0, colon).trim().lowercase()] = line.substring(colon + 1).trim()
        }
        val perms = m["permissions"].orEmpty().split(',', ' ', '\t').map { it.trim() }.filter { it.isNotEmpty() }
        return Info(m["title"].orEmpty(), m["version"].orEmpty(), m["author"].orEmpty(), m["description"].orEmpty(),
            m["api"].orEmpty(), perms)
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

    /** Folder names the engine loads: a-z, 0-9 and _, up to 32 (touch_lua.c, TLua_ValidModName). */
    fun isModName(name: String) = name.length in 1..32 && name.all { it in 'a'..'z' || it in '0'..'9' || it == '_' }

    fun entry(name: String, info: Info?, enabled: Set<String>, hasInit: Boolean, grants: String? = null, withPermissions: Boolean = false) = ModEntry(
        name = name,
        title = info?.title?.takeIf { it.isNotEmpty() } ?: name,
        version = info?.version.orEmpty(),
        author = info?.author.orEmpty(),
        description = info?.description.orEmpty(),
        enabled = name in enabled,
        hasInit = hasInit,
        requested = if (withPermissions) requested(info) else emptyList(),
        granted = if (withPermissions) parseGrants(grants, info != null && info.api.isNotEmpty()) else emptySet(),
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
