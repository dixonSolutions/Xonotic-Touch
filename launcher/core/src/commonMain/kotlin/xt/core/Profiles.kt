// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

/**
 * A touch/profiles/<name>.cfg file. Layout presets (standard, left, ...) go to
 * start.sh as XONOTIC_TOUCH_PROFILE, performance ones (battery, thermal, ...)
 * as XONOTIC_TOUCH_PERF_PROFILE. helpers.cfg is a building block, not a choice.
 */
data class Profile(val id: String, val title: String, val summary: String, val kind: Kind) {
    enum class Kind { LAYOUT, PERFORMANCE, PART }
}

object Profiles {
    /** Shown order; anything else found on disk goes after these. */
    private val order = listOf("standard", "casual", "competitive", "minimal", "left",
        "battery", "thermal", "balanced", "quality")

    /** Used when no profile directory can be read (a dev build, a broken install). */
    val builtin: List<Profile> = listOf(
        Profile("standard", "Standard", "Arena two-thumb (default)", Profile.Kind.LAYOUT),
        Profile("casual", "Casual", "New players: larger targets, relaxed aim", Profile.Kind.LAYOUT),
        Profile("competitive", "Competitive", "Experienced touch FPS players", Profile.Kind.LAYOUT),
        Profile("minimal", "Minimal", "Small screens: stick, FIRE, HOP, nothing else", Profile.Kind.LAYOUT),
        Profile("left", "Left-handed", "Left-handed mirror of standard", Profile.Kind.LAYOUT),
        Profile("battery", "Battery", "Alias of thermal for tablets", Profile.Kind.PERFORMANCE),
        Profile("thermal", "Thermal", "Keep the device cool enough to play", Profile.Kind.PERFORMANCE),
        Profile("balanced", "Balanced", "60 fps with the battery profiles' savings", Profile.Kind.PERFORMANCE),
        Profile("quality", "Quality", "Higher fidelity where thermals allow", Profile.Kind.PERFORMANCE),
    )

    private val titles = mapOf("left" to "Left-handed")

    /** Reads one profile from its file name (without .cfg) and text. */
    fun parse(id: String, text: String): Profile {
        val kind = when {
            id == "helpers" -> Profile.Kind.PART
            text.lineSequence().any { it.trimStart().startsWith("touch_performance_profile") } -> Profile.Kind.PERFORMANCE
            else -> Profile.Kind.LAYOUT
        }
        // First line: "// touch/profiles/casual.cfg — New players: larger targets, relaxed aim."
        val first = text.lineSequence().firstOrNull { it.trimStart().startsWith("//") }.orEmpty()
        var summary = first.substringAfter("—", "").trim()
        if (summary.isEmpty()) summary = first.substringAfter(" - ", "").trim()
        summary = summary.removeSuffix(".").removePrefix("Performance only.").trim()
        val title = titles[id] ?: id.replaceFirstChar { it.uppercaseChar() }
        return Profile(id, title, summary, kind)
    }

    fun sort(list: List<Profile>): List<Profile> =
        list.sortedWith(compareBy({ order.indexOf(it.id).let { i -> if (i < 0) order.size else i } }, { it.id }))

    fun layouts(list: List<Profile>) = sort(list.filter { it.kind == Profile.Kind.LAYOUT })
    fun performance(list: List<Profile>) = sort(list.filter { it.kind == Profile.Kind.PERFORMANCE })
}
