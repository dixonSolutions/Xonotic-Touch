// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import xt.core.net.ServerAddress

/**
 * What the loader starts by itself, before or without the UI bridge: the game
 * as it always started (its own menu), or a join from a `godnotic://` link or
 * a typed address. Everything else (hosting, bot matches, the server browser)
 * is described and run by the engine through the UI bridge.
 */
sealed class GameAction {
    abstract val title: String

    /** The game with its own menu (no native front end mode). */
    object Play : GameAction() {
        override val title get() = "Godnotic"
    }

    /** Straight into a server, in native front end mode; the password goes through the secret fd. */
    data class Connect(val address: ServerAddress, val password: String? = null) : GameAction() {
        override val title get() = "Join $address"
    }
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
        "error" -> "The game stopped with an error: ${clean(reason)}" + (if (logPath.isNotEmpty()) ". Log: $logPath" else "")
        "crashed" -> "The game crashed (exit $exitCode)" + (if (logPath.isNotEmpty()) ". Log: $logPath" else "")
        "not_started" -> "The game could not start: ${clean(reason)}"
        else -> clean(reason).ifBlank { null }
    }

    private fun clean(s: String) = xt.core.net.Text.plain(s.removePrefix("Disconnect: ").removePrefix("Connect: ").removePrefix("rejected, "))
        .removePrefix(LanRulesMarkers.REQUIRED.trim()).removePrefix(LanRulesMarkers.WRONG.trim()).trim()

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
            return when {
                signalled && kind == "quit" -> SessionResult("crashed", exitCode = exitCode, logPath = logPath)
                // Sys_Error exits non-zero through atexit (kind=quit): an error, not a quit.
                exitCode != 0 && kind == "quit" -> SessionResult("error", "exit status $exitCode", exitCode = exitCode, logPath = logPath)
                else -> SessionResult(kind, m["reason"].orEmpty(), m["server"].orEmpty(), exitCode, logPath)
            }
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

    fun actionArgs(a: GameAction): List<String> = when (a) {
        is GameAction.Connect -> listOf("+connect", a.address.toString())
        GameAction.Play -> emptyList()
    }

    /** The secret for an action: `join\t<address>\t<password>`; null when there is none. */
    fun secret(a: GameAction): String? = when (a) {
        is GameAction.Connect -> a.password?.takeIf { it.isNotEmpty() && '\n' !in it }?.let { "join\t${a.address}\t$it\n" }
        GameAction.Play -> null
    }

    /**
     * The whole session: start.sh (which prepares the session as always) with the
     * launcher's choices, native front end mode for a join, and the action.
     * [resultPath] is where the engine writes how the session ended.
     */
    fun plan(a: GameAction, cfg: LauncherConfig, startScript: String, resultPath: String): SessionPlan {
        val base = Launch.game(cfg, startScript)
        val env = LinkedHashMap(base.env)
        if (a != GameAction.Play) env[ENV_MODE] = "native"
        env[ENV_RESULT] = resultPath
        val secret = secret(a)
        if (secret != null) env[ENV_SECRET_FD] = "$SECRET_FD"
        return SessionPlan(base.program, base.args + actionArgs(a), env, secret)
    }
}
