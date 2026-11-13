// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.net

/** A game server's address: an IPv4/IPv6 literal (or a host name typed by the player) and a UDP port. */
data class ServerAddress(val host: String, val port: Int = DpProtocol.DEFAULT_PORT) {
    val isIPv6 get() = ':' in host

    /** On a private or link-local network (touch_lan_logic.h's ranges; fe80::/10 and fc00::/7 for IPv6). */
    val isLan: Boolean
        get() = LanRules.ipv4ToLong(host)?.let { LanRules.isLanIPv4(it) }
            ?: host.lowercase().let { it.startsWith("fe8") || it.startsWith("fe9") || it.startsWith("fea") || it.startsWith("feb") || it.startsWith("fc") || it.startsWith("fd") || it == "::1" }

    /** What `connect` takes: `1.2.3.4:26000`, `[::1]:26000`. */
    override fun toString() = if (isIPv6) "[$host]:$port" else "$host:$port"

    companion object {
        /**
         * `1.2.3.4`, `1.2.3.4:26001`, `[fe80::1]:26000`, `::1`, `example.org:26000`.
         * Null for empty text, spaces, quotes or semicolons (it goes into a console command) or a bad port.
         */
        fun parse(text: String, defaultPort: Int = DpProtocol.DEFAULT_PORT): ServerAddress? {
            val s = text.trim()
            if (s.isEmpty() || s.any { it.isWhitespace() || it == '"' || it == ';' || it == '\\' || it == '$' }) return null
            if (s.startsWith("[")) {
                val end = s.indexOf(']')
                if (end < 0) return null
                val host = s.substring(1, end)
                val rest = s.substring(end + 1)
                val port = if (rest.isEmpty()) defaultPort else rest.removePrefix(":").toIntOrNull() ?: return null
                return if (host.isNotEmpty() && port in 1..65535) ServerAddress(host, port) else null
            }
            val colons = s.count { it == ':' }
            if (colons > 1) return ServerAddress(s, defaultPort) // bare IPv6
            val host = s.substringBefore(':')
            val port = if (colons == 1) s.substringAfter(':').toIntOrNull() ?: return null else defaultPort
            if (host.isEmpty() || port !in 1..65535) return null
            return ServerAddress(host, port)
        }
    }
}

data class PlayerInfo(val name: String, val score: Int, val ping: Int, val team: Int?) {
    val isBot get() = ping == 0
}

data class ServerInfo(
    val address: ServerAddress,
    val name: String,
    val map: String,
    val gametype: String,
    val version: String = "",
    val players: Int,
    val bots: Int,
    val maxPlayers: Int,
    val protocol: Int = 3,
    val locked: Boolean = false,
    val mod: String = "",
    val impure: Int = 0,
    /** -1 when not measured (a cached entry). */
    val pingMs: Int = -1,
    val lan: Boolean = false,
    val playerList: List<PlayerInfo> = emptyList(),
    val encrypted: Boolean = false,
    /** From the cache, not answered in this refresh yet. */
    val stale: Boolean = false,
) {
    val humans get() = (players - bots).coerceAtLeast(0)
    val full get() = maxPlayers in 1..players
    val empty get() = humans == 0
}

/** Game type ids (qcstatus' first field, the `g_<id>` cvars) and the names players read. */
object GameTypes {
    val names = linkedMapOf(
        "dm" to "Deathmatch", "tdm" to "Team Deathmatch", "ctf" to "Capture the Flag", "ca" to "Clan Arena",
        "ft" to "Freeze Tag", "duel" to "Duel", "lms" to "Last Man Standing", "ka" to "Keepaway",
        "kh" to "Key Hunt", "dom" to "Domination", "race" to "Race", "cts" to "Complete the Stage",
        "ons" to "Onslaught", "as" to "Assault", "inv" to "Invasion", "nb" to "Nexball", "tka" to "Team Keepaway",
        "mayhem" to "Mayhem", "tmayhem" to "Team Mayhem",
    )

    fun title(id: String): String = names[id.lowercase()] ?: id.uppercase()
}

/** Sorting and filtering for the browser list; pure so it is tested. */
object ServerList {
    enum class Sort { PING, PLAYERS, NAME, MAP, MODE }

    fun filter(list: Collection<ServerInfo>, text: String, hideEmpty: Boolean = false, hideFull: Boolean = false): List<ServerInfo> {
        val words = text.trim().lowercase().split(' ').filter { it.isNotEmpty() }
        return list.filter { s ->
            (!hideEmpty || !s.empty) && (!hideFull || !s.full) && words.all { w ->
                Text.plain(s.name).lowercase().contains(w) || s.map.lowercase().contains(w) ||
                    s.gametype.lowercase() == w || GameTypes.title(s.gametype).lowercase().contains(w) ||
                    s.address.toString().contains(w)
            }
        }
    }

    /**
     * LAN games first, then favourites, then [sort]. Ties are broken by players (more first),
     * then address, so a refresh does not reshuffle equal rows.
     */
    fun sort(list: List<ServerInfo>, sort: Sort, favourites: Set<String> = emptySet()): List<ServerInfo> {
        val primary: Comparator<ServerInfo> = when (sort) {
            Sort.PING -> compareBy { if (it.pingMs < 0) Int.MAX_VALUE else it.pingMs }
            Sort.PLAYERS -> compareByDescending<ServerInfo> { it.humans }.thenByDescending { it.players }
            Sort.NAME -> compareBy { Text.plain(it.name).lowercase() }
            Sort.MAP -> compareBy { it.map.lowercase() }
            Sort.MODE -> compareBy { GameTypes.title(it.gametype) }
        }
        return list.sortedWith(
            compareByDescending<ServerInfo> { it.lan }
                .thenByDescending { it.address.toString() in favourites }
                .then(primary)
                .thenByDescending { it.humans }
                .thenBy { it.address.toString() },
        )
    }
}

/** Quake text: colour codes out, odd bytes made readable. */
object Text {
    /** Drops `^1` and `^xRGB` colour codes; `^^` is a caret. Control characters become spaces. */
    fun plain(s: String): String {
        val sb = StringBuilder(s.length)
        var i = 0
        while (i < s.length) {
            val c = s[i]
            if (c == '^' && i + 1 < s.length) {
                val n = s[i + 1]
                if (n.isDigit()) { i += 2; continue }
                if (n == 'x' && i + 4 < s.length && (2..4).all { hex(s[i + it]) }) { i += 5; continue }
                if (n == '^') { sb.append('^'); i += 2; continue }
            }
            // Control characters become spaces; the game font's private glyphs (U+E000-U+F8FF) and broken bytes are dropped.
            if (c.code < 32) sb.append(' ') else if (c !in '\uE000'..'\uF8FF' && c != '\uFFFD') sb.append(c)
            i++
        }
        return sb.toString().trim()
    }

    private fun hex(c: Char) = c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F'
}

/**
 * The last list, saved so the browser shows something the moment it opens:
 * one tab-separated line per server. Pings are not kept (they would be stale).
 */
object ServerCache {
    private const val VERSION = "xt-servers 1"

    fun serialize(list: Collection<ServerInfo>): String = buildString {
        append(VERSION).append('\n')
        for (s in list) {
            append(listOf(s.address.toString(), clean(s.name), clean(s.map), clean(s.gametype), s.players, s.bots,
                s.maxPlayers, if (s.locked) 1 else 0, if (s.lan) 1 else 0, clean(s.version), clean(s.mod)).joinToString("\t"))
            append('\n')
        }
    }

    fun parse(text: String): List<ServerInfo> {
        val lines = text.lines()
        if (lines.firstOrNull() != VERSION) return emptyList()
        return lines.drop(1).mapNotNull { line ->
            val f = line.split('\t')
            if (f.size < 9) return@mapNotNull null
            val addr = ServerAddress.parse(f[0]) ?: return@mapNotNull null
            ServerInfo(addr, f[1], f[2], f[3], f.getOrElse(9) { "" }, f[4].toIntOrNull() ?: 0, f[5].toIntOrNull() ?: 0,
                f[6].toIntOrNull() ?: 0, locked = f[7] == "1", lan = f[8] == "1", mod = f.getOrElse(10) { "" }, stale = true)
        }
    }

    private fun clean(s: String) = s.replace('\t', ' ').replace('\n', ' ')
}

/** Favourite servers, kept as a space-separated list in launcher.conf. */
object Favourites {
    fun parse(s: String?): LinkedHashSet<String> =
        s.orEmpty().split(' ').mapNotNull { ServerAddress.parse(it)?.toString() }.toCollection(LinkedHashSet())

    fun serialize(set: Collection<String>) = set.joinToString(" ")
}
