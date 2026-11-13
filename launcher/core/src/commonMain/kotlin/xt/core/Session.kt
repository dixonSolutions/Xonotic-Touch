// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import xt.core.net.GameTypes
import xt.core.net.ServerAddress

/**
 * What the player asked the game to do. The app starts the engine straight
 * into it, in native front end mode (engine/darkplaces/touch_nativefe.c):
 * no main menu at start, and leaving the match ends the engine with a
 * result the app shows.
 */
sealed class GameAction {
    abstract val title: String

    data class Connect(val address: ServerAddress, val password: String? = null, val serverName: String = "") : GameAction() {
        override val title get() = "Join ${serverName.ifBlank { address.toString() }}"
    }

    data class Host(val s: HostSettings) : GameAction() {
        override val title get() = "Host ${s.name}"
    }

    data class BotMatch(val s: MatchSettings) : GameAction() {
        override val title get() = "${GameTypes.title(s.gametype)} on ${s.map}"
    }

    data class Campaign(val campaign: String, val index: Int, val map: String) : GameAction() {
        override val title get() = "Campaign level ${index + 1}"
    }

    /** The game's own menu, for the settings the app does not show. */
    object Menu : GameAction() {
        override val title get() = "Game menu"
    }
}

/** The Host screen's choices (dialog_touch_lan.qc and docs/LAN.md defaults). */
data class HostSettings(
    val name: String = "Godnotic server",
    val gametype: String = "dm",
    /** Empty: a random map with the mode. */
    val map: String = Launch.DEFAULT_MAP,
    val bots: Int = 4,
    val maxPlayers: Int = 8,
    val port: Int = 26000,
    val password: String = "",
    /** sv_touch_assist: 7 all, 5 no auto-shoot, 1 aim assist only, 0 none (dialog_touch_lan.qc). */
    val assist: Int = 7,
) {
    /** The bots take slots too: leave at least one for another player (the in-game Host screen's rule). */
    val slots get() = maxOf(maxPlayers, bots + 2).coerceIn(2, 64)
}

/** A local bot match (the Create dialog's settings). */
data class MatchSettings(
    val map: String = Launch.DEFAULT_MAP,
    val gametype: String = "dm",
    val bots: Int = 4,
    /** skill 0-10 (the Create dialog: Botlike to Godlike). */
    val skill: Int = 3,
    /** -1: the map's own (timelimit_override). */
    val timelimit: Int = -1,
    val fraglimit: Int = -1,
    val mutators: Set<String> = emptySet(),
) {
    fun serialize() = listOf(map, gametype, bots, skill, timelimit, fraglimit, mutators.sorted().joinToString(",")).joinToString("|")

    companion object {
        fun parse(s: String): MatchSettings? {
            val f = s.split('|')
            if (f.size < 7) return null
            val map = f[0].filter { it.isLetterOrDigit() || it in "_-." }
            if (map.isEmpty()) return null
            return MatchSettings(map, f[1].filter { it.isLetterOrDigit() }.ifEmpty { "dm" }, f[2].toIntOrNull() ?: 4, f[3].toIntOrNull() ?: 3,
                f[4].toIntOrNull() ?: -1, f[5].toIntOrNull() ?: -1,
                f[6].split(',').filter { it in Mutators.ids }.toSet())
        }
    }
}

object Mutators {
    data class Mutator(val id: String, val title: String, val description: String)

    /** Mutators the Create dialog offers, by their cvar (mutators.cfg). */
    val list = listOf(
        Mutator("g_instagib", "InstaGib", "One shot kills; vaporizer only"),
        Mutator("g_nix", "NIX", "No items: everyone plays the same weapon, switching every so often"),
        Mutator("g_vampire", "Vampire", "Damage you deal heals you"),
        Mutator("g_grappling_hook", "Grappling hook", "Everyone spawns with the hook"),
        Mutator("g_jetpack", "Jetpack", "Everyone can fly"),
        Mutator("g_midair", "Midair", "Damage only lands on airborne players"),
        Mutator("g_pinata", "Piñata", "The dead drop every weapon they had"),
        Mutator("g_weapon_stay", "Weapons stay", "Picked-up weapons stay for others"),
        Mutator("g_nades", "Nades", "Offhand grenades"),
        Mutator("g_cloaked", "Cloaked", "Everyone mostly invisible"),
        Mutator("g_rocket_flying", "Rocket flying", "Easier rocket jumps"),
        Mutator("g_invincible_projectiles", "Invincible projectiles", "Shots cannot be shot down"),
        Mutator("g_dodging", "Dodging", "Quick double-tap dodges"),
    )
    val ids = list.map { it.id }.toSet()
}

/** What the engine tells the app when a session ends (touch_nativefe.c writes it). */
data class SessionResult(
    /** quit, left, kicked, connect_failed, password_required, password_wrong, error, crashed, not_started */
    val kind: String,
    val reason: String = "",
    val server: String = "",
    val exitCode: Int = 0,
    val logPath: String = "",
) {
    val isError get() = kind in setOf("kicked", "connect_failed", "error", "crashed", "not_started", "password_wrong")
    val wantsPassword get() = kind == "password_required" || kind == "password_wrong"

    /** One sentence for a toast or banner. Null when nothing needs saying (the player quit or left). */
    fun message(): String? = when (kind) {
        "quit", "left" -> null
        "kicked" -> if (reason.isNotBlank()) "Disconnected: ${clean(reason)}" else "The server disconnected you"
        "connect_failed" -> if (reason.isNotBlank()) "Could not join: ${clean(reason)}" else "Could not join the server"
        "password_required" -> "This game needs a password"
        "password_wrong" -> "Wrong password. Try again"
        "error" -> "The game stopped with an error: ${clean(reason)}"
        "crashed" -> "The game crashed (exit $exitCode)" + (if (logPath.isNotEmpty()) ". Log: $logPath" else "")
        "not_started" -> "The game could not start: ${clean(reason)}"
        else -> clean(reason).ifBlank { null }
    }

    private fun clean(s: String) = xt.core.net.Text.plain(s.removePrefix("Disconnect: ").removePrefix("Connect: "))
        .removePrefix(LanRulesMarkers.REQUIRED).removePrefix(LanRulesMarkers.WRONG)

    companion object {
        /**
         * The result file: `key=value` lines (kind, reason, server). A missing file
         * after the process ended means it never reached the point of writing one:
         * a crash, unless it exited 0 (a quit before native mode was set up).
         */
        fun parse(text: String?, exitCode: Int, signalled: Boolean, logPath: String = ""): SessionResult {
            if (text == null) {
                return if (exitCode == 0 && !signalled) SessionResult("quit", exitCode = 0)
                else SessionResult("crashed", exitCode = exitCode, logPath = logPath)
            }
            val m = HashMap<String, String>()
            for (line in text.lineSequence()) {
                val eq = line.indexOf('=')
                if (eq > 0) m[line.substring(0, eq).trim()] = line.substring(eq + 1).trim()
            }
            val kind = m["kind"]?.takeIf { it.isNotEmpty() } ?: "quit"
            return if (signalled && kind == "quit") SessionResult("crashed", exitCode = exitCode, logPath = logPath)
            else SessionResult(kind, m["reason"].orEmpty(), m["server"].orEmpty(), exitCode, logPath)
        }
    }
}

private object LanRulesMarkers {
    const val REQUIRED = "[pw-required] "
    const val WRONG = "[pw-wrong] "
}

/** A session ready to spawn: the program, its arguments and environment, and the secret for the fd. */
data class SessionPlan(
    val program: String,
    val args: List<String>,
    val env: Map<String, String>,
    /** Written to a pipe whose read end the engine gets as fd [Session.SECRET_FD]; never in argv or env. */
    val secret: String? = null,
    val cwd: String? = null,
)

object Session {
    /** The fd number the engine reads the secret from (named in GODNOTIC_FE_SECRET_FD). */
    const val SECRET_FD = 3
    const val ENV_MODE = "GODNOTIC_FRONTEND"
    const val ENV_RESULT = "GODNOTIC_FE_RESULT"
    const val ENV_SECRET_FD = "GODNOTIC_FE_SECRET_FD"

    /** Every game type cvar MapInfo_SwitchGameType sets (Gametypes registry netnames). */
    val GAMETYPE_CVARS = listOf("dm", "tdm", "ctf", "ca", "ft", "duel", "lms", "ka", "kh", "dom", "race", "cts",
        "ons", "as", "inv", "nb", "tka", "mayhem", "tmayhem")

    /** Text that goes into a quoted console string: no quotes, newlines, backslashes or `$` expansion. */
    fun clean(s: String) = s.replace('"', '\'').replace('\n', ' ').replace('\r', ' ').replace('\\', '/').replace("$", "")

    private fun mapName(s: String) = s.filter { it.isLetterOrDigit() || it in "_-." }

    private fun mode(s: String) = s.lowercase().filter { it.isLetterOrDigit() }.takeIf { it in GAMETYPE_CVARS } ?: "dm"

    /** `+g_dm 0 +g_tdm 0 ... +g_<mode> 1`, as MapInfo_SwitchGameType leaves them. */
    fun gametypeArgs(mode: String): List<String> = GAMETYPE_CVARS.flatMap { listOf("+g_$it", if (it == mode(mode)) "1" else "0") }

    /**
     * The `+command` arguments for an action. Order matters: the server cvars
     * before `map` (maxplayers takes effect at the next map).
     */
    fun actionArgs(a: GameAction): List<String> = when (a) {
        is GameAction.Connect -> listOf("+connect", a.address.toString())
        is GameAction.Host -> {
            val s = a.s
            // What the in-game Host screen runs (dialog_touch_lan.qc TouchLan_Start).
            listOf(
                "+g_campaign", "0",
                "+maxplayers", "${s.slots}",
                "+port", "${s.port.coerceIn(1024, 65535)}",
                "+sv_public", "0",
                "+sv_status_privacy", "1",
                "+hostname", clean(s.name).ifBlank { "Godnotic server" },
                "+bot_number", "${s.bots.coerceIn(0, 32)}",
                "+set", "sv_touch_assist", "${s.assist.coerceIn(0, 7)}",
            ) + gametypeArgs(s.gametype) + listOf(
                // The password itself comes through the secret fd (touch_nativefe.c sets
                // touch_lan_host_password from it); apply makes it the server's, as the Host screen does.
                "+touch_lan_host_password_apply",
                "+map", mapName(s.map).ifEmpty { Launch.DEFAULT_MAP },
            )
        }
        is GameAction.BotMatch -> {
            val s = a.s
            val args = mutableListOf(
                "+g_campaign", "0",
                "+maxplayers", "${maxOf(s.bots + 1, 2).coerceAtMost(32)}",
                "+sv_public", "0",
                "+bot_number", "${s.bots.coerceIn(0, 31)}",
                "+skill", "${s.skill.coerceIn(0, 10)}",
                "+timelimit_override", "${s.timelimit.coerceIn(-1, 600)}",
                "+fraglimit_override", "${s.fraglimit.coerceIn(-1, 1000)}",
            )
            for (m in Mutators.list) args += listOf("+set", m.id, if (m.id in s.mutators) "1" else "0")
            args += gametypeArgs(s.gametype)
            args += listOf("+map", mapName(s.map).ifEmpty { Launch.DEFAULT_MAP })
            args
        }
        is GameAction.Campaign -> listOf(
            // campaign_setup.qc CampaignSetup: the server's campaign code reads these.
            "+set", "g_campaign", "1",
            "+set", "_campaign_name", clean(a.campaign).filter { it.isLetterOrDigit() || it == '_' },
            "+set", "_campaign_index", "${a.index.coerceAtLeast(0)}",
            "+maxplayers", "16",
            "+map", mapName(a.map),
        )
        GameAction.Menu -> emptyList()
    }

    /** The secret for an action: `join\t<address>\t<password>` or `host\t<password>`; null when there is none. */
    fun secret(a: GameAction): String? = when (a) {
        is GameAction.Connect -> a.password?.takeIf { it.isNotEmpty() }?.let { "join\t${a.address}\t$it\n" }
        is GameAction.Host -> a.s.password.takeIf { it.isNotEmpty() }?.let { "host\t$it\n" }
        else -> null
    }

    /**
     * The whole session: start.sh (which prepares the session as always) with the
     * launcher's choices, native front end mode, the action, and the settings
     * overrides last. [resultPath] is where the engine writes how the session ended.
     */
    fun plan(a: GameAction, cfg: LauncherConfig, startScript: String, resultPath: String, withOverrides: Boolean): SessionPlan {
        val base = Launch.game(cfg, startScript)
        val env = LinkedHashMap(base.env)
        if (a != GameAction.Menu) env[ENV_MODE] = "native"
        env[ENV_RESULT] = resultPath
        val secret = secret(a)
        if (secret != null) env[ENV_SECRET_FD] = "$SECRET_FD"
        val args = base.args + actionArgs(a) +
            (if (withOverrides) listOf("+exec", SettingsStore.OVERRIDES_FILE) else emptyList())
        return SessionPlan(base.program, args, env, secret)
    }
}

/** Campaign levels, from `maps/campaign<name>.txt` (campaign_file.qc's format). */
object CampaignFile {
    data class Level(val gametype: String, val map: String, val bots: Int, val skill: Int, val shortDesc: String, val longDesc: String)

    /**
     * One level per line: `gametype,map,bots,botskill,fraglimit,timelimit,mutators,"short",long...`
     * (comma separated, `//` comments). Only the fields the app shows are kept.
     */
    fun parse(text: String): List<Level> = text.lineSequence().mapNotNull { raw ->
        val line = raw.trim()
        if (line.isEmpty() || line.startsWith("//")) return@mapNotNull null
        val f = splitCsv(line)
        if (f.size < 8) return@mapNotNull null
        Level(f[0].trim(), f[1].trim(), f[2].trim().toIntOrNull() ?: 0, f[3].trim().toIntOrNull() ?: 0, f[7].trim(), f.drop(8).joinToString(",").trim())
    }.toList()

    private fun splitCsv(s: String): List<String> {
        val out = mutableListOf<String>()
        val cur = StringBuilder()
        var q = false
        for (c in s) when {
            c == '"' -> q = !q
            c == ',' && !q -> { out += cur.toString(); cur.clear() }
            else -> cur.append(c)
        }
        out += cur.toString()
        return out
    }
}

/** maps/<name>.mapinfo: the title and the game types a map supports. */
object MapInfoFile {
    data class Info(val name: String, val title: String, val author: String, val gametypes: Set<String>)

    fun parse(name: String, text: String): Info {
        var title = ""
        var author = ""
        val types = LinkedHashSet<String>()
        for (raw in text.lineSequence()) {
            val t = raw.trim().split(Regex("\\s+"), limit = 2)
            if (t.isEmpty()) continue
            val v = t.getOrElse(1) { "" }
            when (t[0].lowercase()) {
                "title" -> title = v
                "author" -> author = v
                "gametype", "type" -> v.split(' ').firstOrNull()?.lowercase()?.let { g ->
                    // Old names: "teamdm" etc. are not used by Xonotic's mapinfo; "dm" / "tdm" are.
                    if (g.isNotEmpty()) types += g
                }
            }
        }
        return Info(name, title.ifBlank { name }, author, types)
    }
}
