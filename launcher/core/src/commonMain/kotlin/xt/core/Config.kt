// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

/**
 * The launcher's settings, kept as `key = value` lines in
 * ~/.config/xonotic-touch/launcher.conf. A flat text file parses in
 * microseconds and needs no serialization library in the binary.
 * Unknown keys are kept, so an older launcher does not drop a newer one's.
 */
class LauncherConfig(private val values: MutableMap<String, String> = linkedMapOf()) {

    operator fun get(key: String): String? = values[key]

    fun string(key: String, default: String = ""): String = values[key] ?: default

    fun int(key: String, default: Int): Int = values[key]?.trim()?.toIntOrNull() ?: default

    fun bool(key: String, default: Boolean): Boolean = when (values[key]?.trim()?.lowercase()) {
        "1", "true", "yes", "on" -> true
        "0", "false", "no", "off" -> false
        else -> default
    }

    operator fun set(key: String, value: String) {
        if (value.isEmpty()) values.remove(key) else values[key] = value
    }

    operator fun set(key: String, value: Int) = set(key, value.toString())
    operator fun set(key: String, value: Boolean) = set(key, if (value) "1" else "0")

    fun serialize(): String = buildString {
        append("# Xonotic Touch launcher settings\n")
        for ((k, v) in values) append(k).append(" = ").append(v.replace('\n', ' ')).append('\n')
    }

    companion object {
        fun parse(text: String): LauncherConfig {
            val map = linkedMapOf<String, String>()
            for (raw in text.lineSequence()) {
                val line = raw.trim()
                if (line.isEmpty() || line.startsWith("#")) continue
                val eq = line.indexOf('=')
                if (eq <= 0) continue
                map[line.substring(0, eq).trim()] = line.substring(eq + 1).trim()
            }
            return LauncherConfig(map)
        }
    }
}

/** Config keys. Values are the choice ids, never display strings. */
object Keys {
    const val TOUCH_PROFILE = "touch_profile"   // standard, left, minimal, casual, competitive
    const val PERF_PROFILE = "perf_profile"     // "" = the game's own first-run choice
    const val WINDOW_MODE = "window_mode"       // auto, fullscreen, windowed
    const val RESOLUTION = "resolution"         // "" = automatic, else WxH
    const val EXTRA_ARGS = "extra_args"
    const val TOUCH_MODE = "touch_mode"         // "" game setting, auto, always, off
    const val AIM_ASSIST = "aim_assist"         // "" game setting, on, off
    const val AUTO_SHOOT = "auto_shoot"         // "" game setting, on, off
    const val USER_BASE = "user_base"           // "" = the package default
    const val SKIP_LAUNCHER = "skip_launcher"
    const val HOST_MAP = "host_map"
    const val HOST_GAMETYPE = "host_gametype"
    const val HOST_BOTS = "host_bots"
    const val HOST_MAXPLAYERS = "host_maxplayers"
    const val HOST_PORT = "host_port"
    const val HOST_NAME = "host_name"
    const val HOST_PUBLIC = "host_public"
}

/** A choice shown in a combo row: the stored id and what the player reads. */
data class Choice(val id: String, val title: String)

object Choices {
    val windowModes = listOf(
        Choice("auto", "Automatic"),
        Choice("fullscreen", "Fullscreen"),
        Choice("windowed", "Window"),
    )
    val resolutions = listOf(
        Choice("", "Automatic"),
        Choice("1280x720", "1280 × 720"),
        Choice("1600x900", "1600 × 900"),
        Choice("1920x1080", "1920 × 1080"),
        Choice("2560x1440", "2560 × 1440"),
        Choice("3840x2160", "3840 × 2160"),
    )
    val touchModes = listOf(
        Choice("", "Default"),
        Choice("auto", "Dynamic"),
        Choice("always", "Always on"),
        Choice("off", "Off"),
    )
    val onOff = listOf(
        Choice("", "Default"),
        Choice("on", "On"),
        Choice("off", "Off"),
    )
    val gametypes = listOf(
        Choice("dm", "Deathmatch"),
        Choice("tdm", "Team Deathmatch"),
        Choice("ctf", "Capture the Flag"),
        Choice("ca", "Clan Arena"),
        Choice("ft", "Freeze Tag"),
        Choice("duel", "Duel"),
        Choice("lms", "Last Man Standing"),
        Choice("ka", "Keepaway"),
        Choice("kh", "Key Hunt"),
        Choice("dom", "Domination"),
        Choice("race", "Race"),
        Choice("cts", "Complete the Stage"),
    )

    fun indexOf(list: List<Choice>, id: String?): Int =
        list.indexOfFirst { it.id == (id ?: "") }.coerceAtLeast(0)
}
