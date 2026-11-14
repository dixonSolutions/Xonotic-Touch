// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

/**
 * Where the game lives. Mirrors packaging/start.sh, which stays the one
 * place that prepares a session; the launcher only has to find it and read
 * the same folders.
 */
data class GamePaths(
    /** Install root holding bin/xonotic and bin/start.sh (/app in the Flatpak). */
    val appRoot: String?,
    val startScript: String?,
    val engine: String?,
    /** start.sh's USER_BASE: the engine's cwd, game packs in <userBase>/data. */
    val userBase: String,
    /** The engine's own userdir (~/.xonotic): config, Lua mods and plugins. */
    val engineUserDir: String,
    val flatpak: Boolean,
) {
    val userData get() = "$userBase/data"
    val modsDir get() = "$engineUserDir/mods"
    val pluginsDir get() = "$engineUserDir/plugins"

    /** Folders the engine searches for packs, highest priority first. */
    val dataDirs: List<String>
        get() = listOfNotNull("$engineUserDir/data", userData, appRoot?.let { "$it/data" }).distinct()

    /** Where touch/profiles/<name>.cfg can be read, best first. */
    val profileDirs: List<String>
        get() = listOfNotNull("$userData/touch/profiles", appRoot?.let { "$it/data/touch/profiles" },
            appRoot?.let { "$it/../touch/profiles" }).distinct()
}

object PathResolver {
    /**
     * @param env the process environment
     * @param exeDir the directory of the running launcher binary
     * @param exists file test, injected so this stays testable and platform-free
     * @param userBaseOverride the launcher setting, empty for the default
     */
    fun resolve(env: Map<String, String>, exeDir: String?, exists: (String) -> Boolean, userBaseOverride: String = ""): GamePaths {
        val home = env["HOME"] ?: "/tmp"
        val flatpak = !env["FLATPAK_ID"].isNullOrEmpty()

        val explicitStart = env["GODNOTIC_START"]?.takeIf { it.isNotEmpty() && exists(it) }
        val rootCandidates = listOfNotNull(
            env["GODNOTIC_APP_ROOT"]?.takeIf { it.isNotEmpty() },
            "/app".takeIf { flatpak },
            exeDir?.let { parent(it) },
            "/app",
        )
        val appRoot = rootCandidates.firstOrNull { exists("$it/bin/start.sh") || exists("$it/bin/xonotic") }
        val start = explicitStart ?: appRoot?.let { "$it/bin/start.sh" }?.takeIf(exists)
        val engine = appRoot?.let { "$it/bin/xonotic" }?.takeIf(exists)

        val userBase = when {
            userBaseOverride.isNotBlank() -> userBaseOverride.trim().trimEnd('/')
            !env["GODNOTIC_USER_BASE"].isNullOrEmpty() -> env.getValue("GODNOTIC_USER_BASE")
            flatpak && !env["XDG_DATA_HOME"].isNullOrEmpty() -> env.getValue("XDG_DATA_HOME") + "/godnotic"
            else -> "$home/.local/share/godnotic"
        }
        return GamePaths(appRoot, start, engine, userBase, "$home/.xonotic", flatpak)
    }

    fun parent(path: String): String {
        val p = path.trimEnd('/')
        val i = p.lastIndexOf('/')
        return if (i <= 0) "/" else p.substring(0, i)
    }
}

/** What a scan of the data folders found. */
data class DataStatus(
    val ready: Boolean,
    val packs: Int,
    val bytes: Long,
    val mainPack: String?,
) {
    val summary: String
        get() = when {
            ready && packs > 0 -> "$packs packs, ${formatBytes(bytes)}"
            ready -> "Ready"
            else -> "Not downloaded yet"
        }

    companion object {
        /** The full game data pack, as fetch-assets names it: xonotic-<ver>-data.pk3. */
        fun isMainPack(name: String) = name.startsWith("xonotic-") && name.endsWith("-data.pk3")
    }
}

fun formatBytes(b: Long): String = when {
    b >= 1L shl 30 -> "${(b * 10 / (1L shl 30)) / 10.0} GB"
    b >= 1L shl 20 -> "${b / (1L shl 20)} MB"
    b >= 1L shl 10 -> "${b / (1L shl 10)} KB"
    else -> "$b B"
}
