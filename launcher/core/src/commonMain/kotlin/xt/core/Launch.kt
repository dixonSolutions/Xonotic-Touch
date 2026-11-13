// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

/** A program to exec, its arguments and the environment it adds. */
data class LaunchPlan(val program: String, val args: List<String>, val env: Map<String, String>, val cwd: String? = null)

object Launch {
    /** A stock map every full data pack carries; used before a scan has listed any. */
    const val DEFAULT_MAP = "stormkeep"

    /**
     * Playing goes through start.sh like the desktop entry always has: it
     * syncs packs, runs the download wizard and picks screen metrics. The
     * launcher's choices reach it as the environment it already reads, and as
     * `+cvar value` arguments that start.sh appends after its own, so they win.
     */
    fun game(cfg: LauncherConfig, startScript: String): LaunchPlan {
        val env = linkedMapOf<String, String>()
        cfg[Keys.TOUCH_PROFILE]?.takeIf { it.isNotEmpty() }?.let { env["XONOTIC_TOUCH_PROFILE"] = it }
        // Set only when chosen: start.sh then applies it on every start, while
        // left unset the game applies its default once and leaves menu changes.
        cfg[Keys.PERF_PROFILE]?.takeIf { it.isNotEmpty() }?.let { env["XONOTIC_TOUCH_PERF_PROFILE"] = it }
        cfg[Keys.TOUCH_MODE]?.takeIf { it.isNotEmpty() }?.let { env["XONOTIC_TOUCH_MODE"] = it }
        cfg[Keys.USER_BASE]?.takeIf { it.isNotBlank() }?.let { env["XONOTIC_TOUCH_USER_BASE"] = it.trim() }

        val args = mutableListOf<String>()
        when (cfg[Keys.WINDOW_MODE]) {
            "fullscreen" -> args += listOf("+vid_fullscreen", "1")
            "windowed" -> args += listOf("+vid_fullscreen", "0")
        }
        parseResolution(cfg[Keys.RESOLUTION])?.let { (w, h) ->
            args += listOf("+vid_width", "$w", "+vid_height", "$h", "+vid_conheight", "$h")
        }
        onOff(cfg[Keys.AIM_ASSIST])?.let { args += listOf("+cl_touch_aimassist", it) }
        onOff(cfg[Keys.AUTO_SHOOT])?.let { args += listOf("+cl_touch_autoshoot", it) }
        args += splitArgs(cfg.string(Keys.EXTRA_ARGS))
        return LaunchPlan(startScript, args, env)
    }

    /**
     * A local dedicated server: the same engine binary with -dedicated, run
     * from the same folder start.sh uses so it finds the same packs.
     * Settings go in the order the engine needs: maxplayers before the map.
     */
    fun server(cfg: LauncherConfig, engine: String, paths: GamePaths): LaunchPlan {
        val args = mutableListOf(
            "-xonotic", "-dedicated",
            "-customgamename", "Xonotic Touch",
            "-customgamenetworkfiltername", "Xonotic",
            "+sys_colortranslation", "0",
            "+hostname", cfg.string(Keys.HOST_NAME, "Xonotic Touch server").ifBlank { "Xonotic Touch server" },
            "+port", cfg.int(Keys.HOST_PORT, 26000).coerceIn(1024, 65535).toString(),
            "+maxplayers", cfg.int(Keys.HOST_MAXPLAYERS, 8).coerceIn(2, 64).toString(),
            "+sv_public", if (cfg.bool(Keys.HOST_PUBLIC, false)) "1" else "0",
            "+bot_number", cfg.int(Keys.HOST_BOTS, 4).coerceIn(0, 32).toString(),
        )
        // `gametype` is a QuakeC command and needs a running server, so the
        // first map picks its mode from the g_<mode> cvars instead.
        val mode = cfg.string(Keys.HOST_GAMETYPE, "dm").ifBlank { "dm" }.filter { it.isLetterOrDigit() }
        if (mode != "dm") args += listOf("+g_dm", "0", "+g_$mode", "1")
        args += listOf("+map", cfg.string(Keys.HOST_MAP).ifBlank { DEFAULT_MAP })
        val env = linkedMapOf<String, String>()
        paths.appRoot?.let { env["LD_LIBRARY_PATH"] = "$it/lib" }
        return LaunchPlan(engine, args, env, paths.userBase)
    }

    fun parseResolution(s: String?): Pair<Int, Int>? {
        if (s.isNullOrBlank()) return null
        val parts = s.lowercase().split('x', '×').map { it.trim() }
        if (parts.size != 2) return null
        val w = parts[0].toIntOrNull() ?: return null
        val h = parts[1].toIntOrNull() ?: return null
        return if (w in 320..16384 && h in 200..16384) w to h else null
    }

    private fun onOff(v: String?) = when (v) { "on" -> "1"; "off" -> "0"; else -> null }

    /** Splits like a shell: spaces separate, quotes group, backslash escapes. */
    fun splitArgs(s: String): List<String> {
        val out = mutableListOf<String>()
        val cur = StringBuilder()
        var quote: Char? = null
        var any = false
        var i = 0
        while (i < s.length) {
            val c = s[i]
            when {
                quote != null && c == quote -> quote = null
                quote == '\'' -> cur.append(c)
                c == '\\' && i + 1 < s.length -> { i++; cur.append(s[i]); any = true }
                quote != null -> cur.append(c)
                c == '"' || c == '\'' -> { quote = c; any = true }
                c.isWhitespace() -> { if (any || cur.isNotEmpty()) out += cur.toString(); cur.clear(); any = false }
                else -> { cur.append(c); any = true }
            }
            i++
        }
        if (any || cur.isNotEmpty()) out += cur.toString()
        return out
    }
}

object Console {
    /** Drops DarkPlaces colour codes (^1, ^xF80) and terminal escapes for the plain-text console view. */
    fun stripColors(s: String): String {
        val sb = StringBuilder(s.length)
        var i = 0
        while (i < s.length) {
            val c = s[i]
            if (c == '\u001b') {
                // CSI: ESC [ params final-byte (sys_colortranslation 1 output)
                i++
                if (i < s.length && s[i] == '[') {
                    i++
                    while (i < s.length && s[i] !in '@'..'~') i++
                }
                i++
                continue
            }
            if (c == '^' && i + 1 < s.length) {
                val n = s[i + 1]
                if (n.isDigit()) { i += 2; continue }
                if (n == 'x' && i + 4 < s.length && (2..4).all { s[i + it].isHex() }) { i += 5; continue }
                if (n == '^') { sb.append('^'); i += 2; continue }
            }
            sb.append(c)
            i++
        }
        return sb.toString()
    }

    private fun Char.isHex() = this in '0'..'9' || this in 'a'..'f' || this in 'A'..'F'
}
