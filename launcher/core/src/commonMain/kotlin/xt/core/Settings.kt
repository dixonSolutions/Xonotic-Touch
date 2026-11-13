// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

/**
 * The game settings the app shows, each one a cvar (or a launcher choice),
 * typed, with its range and default, and how the engine must receive it.
 *
 * How a change reaches the game without the app and the engine fighting
 * over config.cfg (which the engine rewrites from its archived cvars on every
 * clean quit, see cvar.c Cvar_WriteVariables):
 *
 *  1. The app never writes config.cfg. A change goes into
 *     `<userdir>/data/godnotic-frontend.cfg` (the *overrides*): `seta` lines,
 *     plus one-shot commands such as a quality preset's `exec`.
 *  2. Every game the app starts gets `+exec godnotic-frontend.cfg` as its last
 *     argument, after config.cfg, autoexec.cfg and start.sh's touch/startup.cfg,
 *     and before video starts (command-line commands run at init), so the
 *     override wins and even startup cvars (resolution, fullscreen) apply.
 *  3. The overrides are `seta`, so the engine archives them and writes them to
 *     config.cfg when it quits. When the app sees config.cfg rewritten after a
 *     game (it compares a fingerprint taken at launch), every override has been
 *     absorbed, or replaced by what the player changed in the game: the
 *     overrides file is emptied. If config.cfg was not rewritten (a crash, a
 *     kill), the overrides stay for the next start.
 *  4. What the app shows = config.cfg, with pending overrides on top, with the
 *     schema default for anything config.cfg leaves out (the engine omits
 *     cvars at their default).
 */
data class Setting(
    val key: String,
    val type: Type,
    val label: String,
    val description: String = "",
    val default: String = "",
    val min: Double = 0.0,
    val max: Double = 1.0,
    val step: Double = 1.0,
    val choices: List<Choice> = emptyList(),
    val page: String,
    val group: String,
    /** STARTUP: read before video starts (resolution...). GAME: any time; both are applied the same way. */
    val phase: Phase = Phase.GAME,
    /** Where it is kept: a cvar in config.cfg, or a key in launcher.conf. */
    val store: Store = Store.CVAR,
    /** For BOOL: the cvar value meaning "on" and "off" (some use strings, e.g. "local"/"off"). */
    val on: String = "1",
    val off: String = "0",
    /** Shown under the group, e.g. the ESP note. */
    val note: String = "",
) {
    enum class Type { BOOL, INT, FLOAT, STRING, CHOICE, COLOR }
    enum class Phase { STARTUP, GAME }
    enum class Store { CVAR, LAUNCHER }

    /** Normalises a value for this setting; null when it is not acceptable. */
    fun normalize(v: String): String? = when (type) {
        Type.BOOL -> when (v.trim().lowercase()) {
            on.lowercase(), "1", "true", "on", "yes" -> on
            off.lowercase(), "0", "false", "off", "no", "" -> off
            else -> if ((v.toDoubleOrNull() ?: 0.0) != 0.0) on else off
        }
        Type.INT, Type.COLOR -> v.trim().toDoubleOrNull()?.let { fmt(it.coerceIn(min, max).let { d -> kotlin.math.round(d) }) }
        Type.FLOAT -> v.trim().toDoubleOrNull()?.let { fmt(it.coerceIn(min, max)) }
        Type.STRING -> if ('\n' in v || '\r' in v) null else v
        Type.CHOICE -> v.takeIf { c -> choices.any { it.id == c } } ?: choices.firstOrNull { it.id == default }?.id
    }

    companion object {
        /** Numbers the way the engine prints them: no trailing zeros. */
        fun fmt(d: Double): String {
            if (d == kotlin.math.floor(d) && kotlin.math.abs(d) < 1e15) return d.toLong().toString()
            var s = (kotlin.math.round(d * 10000.0) / 10000.0).toString()
            if ('.' in s) s = s.trimEnd('0').trimEnd('.')
            return s
        }
    }
}

/** A parsed config file: the last value set for each cvar (names case-insensitive, as the engine treats them). */
class CvarFile(val values: Map<String, String>) {
    operator fun get(name: String): String? = values[name.lowercase()]

    companion object {
        /**
         * Reads `seta "name" "value"`, `set name value` and the engine's own `"name" "value"`
         * lines; other commands (bind, alias...) are skipped. Quoted strings undo the
         * engine's backslash escapes (Cmd_QuoteString: \" \\ \$).
         */
        fun parse(text: String): CvarFile {
            val out = HashMap<String, String>()
            for (raw in text.lineSequence()) {
                val t = tokenize(raw)
                if (t.isEmpty()) continue
                val (name, value) = when {
                    (t[0] == "seta" || t[0] == "set") && t.size >= 3 -> t[1] to t[2]
                    t[0] == "seta" || t[0] == "set" -> continue
                    raw.trimStart().startsWith("\"") && t.size >= 2 -> t[0] to t[1]
                    else -> continue
                }
                out[name.lowercase()] = value
            }
            return CvarFile(out)
        }

        /** Splits one line like the console: quotes group, `//` starts a comment outside quotes. */
        fun tokenize(line: String): List<String> {
            val out = mutableListOf<String>()
            var i = 0
            while (i < line.length) {
                val c = line[i]
                when {
                    c.isWhitespace() -> i++
                    c == '/' && i + 1 < line.length && line[i + 1] == '/' -> return out
                    c == ';' -> return out // a second command on the line: ignore it
                    c == '"' -> {
                        val sb = StringBuilder()
                        i++
                        while (i < line.length && line[i] != '"') {
                            if (line[i] == '\\' && i + 1 < line.length && line[i + 1] in "\"\\$") {
                                sb.append(line[i + 1]); i += 2
                            } else {
                                sb.append(line[i]); i++
                            }
                        }
                        i++
                        out += sb.toString()
                    }
                    else -> {
                        val start = i
                        while (i < line.length && !line[i].isWhitespace() && line[i] != ';' && line[i] != '"') i++
                        out += line.substring(start, i)
                    }
                }
            }
            return out
        }

        /** A quoted console string, escaped the way the engine writes config.cfg. */
        fun quote(s: String): String = buildString {
            append('"')
            for (c in s) {
                if (c == '"' || c == '\\' || c == '$') append('\\')
                append(c)
            }
            append('"')
        }
    }
}

/**
 * The pending overrides (`godnotic-frontend.cfg`): cvar values the app
 * changed since the engine last absorbed them, and one-shot commands.
 */
class Overrides(val cvars: LinkedHashMap<String, String> = linkedMapOf(), val commands: MutableList<String> = mutableListOf()) {
    val isEmpty get() = cvars.isEmpty() && commands.isEmpty()

    fun serialize(): String = buildString {
        append("// Written by the Godnotic app: settings changed there, applied at the next start.\n")
        append("// The game absorbs them into config.cfg; the app empties this file afterwards.\n")
        // Commands first (a preset), so a cvar the player set by hand still wins over it.
        for (c in commands) append(c).append('\n')
        for ((k, v) in cvars) append("seta ").append(CvarFile.quote(k)).append(' ').append(CvarFile.quote(v)).append('\n')
    }

    companion object {
        private val commandPrefixes = listOf("exec ")

        fun parse(text: String): Overrides {
            val o = Overrides()
            for (raw in text.lineSequence()) {
                val line = raw.trim()
                if (line.isEmpty() || line.startsWith("//")) continue
                val t = CvarFile.tokenize(line)
                if (t.size >= 3 && t[0] == "seta") o.cvars[t[1]] = t[2]
                else if (commandPrefixes.any { line.startsWith(it) }) o.commands += line
            }
            return o
        }
    }
}

/** Reads and changes settings through the overrides mechanism. Pure: files are passed in. */
class SettingsStore(
    val schema: List<Setting>,
    var config: CvarFile,
    val overrides: Overrides,
    val launcher: LauncherConfig,
) {
    private val byKey = schema.associateBy { it.key }

    fun setting(key: String) = byKey[key]

    /** The value the next game will use. */
    fun value(s: Setting): String = when (s.store) {
        Setting.Store.LAUNCHER -> launcher[s.key] ?: s.default
        Setting.Store.CVAR -> overrides.cvars[s.key] ?: config[s.key] ?: s.default
    }

    fun value(key: String): String = byKey[key]?.let { value(it) } ?: overrides.cvars[key] ?: config[key].orEmpty()

    fun bool(key: String) = byKey[key]?.let { value(it) == it.on } ?: false
    fun double(key: String) = value(key).toDoubleOrNull() ?: 0.0

    /** Pending (not yet absorbed by a game)? */
    fun pending(key: String) = key in overrides.cvars

    /**
     * Sets a value. A cvar value equal to what config.cfg (or the default, when
     * config.cfg leaves it out) already gives drops the override instead.
     * Returns false when the value is not acceptable.
     */
    fun set(key: String, raw: String): Boolean {
        val s = byKey[key]
        val v = s?.normalize(raw) ?: if (s == null) raw else return false
        if (s?.store == Setting.Store.LAUNCHER) {
            launcher[key] = v
            return true
        }
        val current = config[key] ?: s?.default
        if (current != null && same(s, current, v)) overrides.cvars.remove(key) else overrides.cvars[key] = v
        return true
    }

    /** A one-shot command for the next start (a quality preset). Replaces an earlier one with the same verb and target prefix. */
    fun command(cmd: String, replacesPrefix: String) {
        overrides.commands.removeAll { it.startsWith(replacesPrefix) }
        overrides.commands += cmd
    }

    private fun same(s: Setting?, a: String, b: String): Boolean {
        if (a == b) return true
        if (s == null) return false
        val x = a.toDoubleOrNull()
        val y = b.toDoubleOrNull()
        return x != null && y != null && kotlin.math.abs(x - y) < 1e-9
    }

    /**
     * After a game: if config.cfg was rewritten ([configChanged]), the engine has
     * absorbed (or the player superseded) every override, so they are dropped.
     * Returns true when the overrides file must be rewritten.
     */
    fun afterGame(newConfig: CvarFile, configChanged: Boolean, atLaunch: Overrides = snapshot()): Boolean {
        config = newConfig
        if (!configChanged || overrides.isEmpty) return false
        // Only what the game saw: a change made in the app while the game ran stays pending.
        for ((k, v) in atLaunch.cvars) if (overrides.cvars[k] == v) overrides.cvars.remove(k)
        overrides.commands.removeAll { it in atLaunch.commands }
        return true
    }

    /** A copy of the overrides as they are now (taken when a game starts). */
    fun snapshot() = Overrides(LinkedHashMap(overrides.cvars), overrides.commands.toMutableList())

    companion object {
        const val OVERRIDES_FILE = "godnotic-frontend.cfg"
    }
}
