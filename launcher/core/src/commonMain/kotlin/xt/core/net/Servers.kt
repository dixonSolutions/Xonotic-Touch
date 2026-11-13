// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.net

/**
 * What the loader itself needs to know about servers: an address typed or
 * linked by the player, and `godnotic://join/<host>:<port>` links (the
 * desktop file registers the scheme). Finding servers, LAN/VPN discovery
 * and passwords belong to the engine (the UI bridge); this only validates
 * what comes in from outside so it can be handed to the engine safely.
 */
data class ServerAddress(val host: String, val port: Int = DEFAULT_PORT) {
    val isIPv6 get() = ':' in host

    /** What `connect` takes: `1.2.3.4:26000`, `[::1]:26000`. */
    override fun toString() = if (isIPv6) "[$host]:$port" else "$host:$port"

    companion object {
        const val DEFAULT_PORT = 26000

        /**
         * `1.2.3.4`, `1.2.3.4:26001`, `[fe80::1]:26000`, `::1`, `example.org:26000`.
         * Null for empty text, spaces, quotes, semicolons, backslashes or `$`
         * (it goes into a console command) or a bad port.
         */
        fun parse(text: String, defaultPort: Int = DEFAULT_PORT): ServerAddress? {
            val s = text.trim()
            if (s.isEmpty() || s.length > 255 || s.any { it.isWhitespace() || it == '"' || it == ';' || it == '\\' || it == '$' || it.code < 32 }) return null
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

/** `godnotic://join/<host>:<port>` links (docs/LAN.md, "Playing over a VPN"). */
object JoinLink {
    const val SCHEME = "godnotic"

    fun of(a: ServerAddress) = "$SCHEME://join/$a"

    /** A link, or a plain address; null when it is neither. */
    fun parse(text: String): ServerAddress? {
        val t = text.trim()
        val prefix = "$SCHEME://join/"
        if (t.startsWith("$SCHEME:", ignoreCase = true)) {
            if (!t.lowercase().startsWith(prefix)) return null
            return ServerAddress.parse(t.substring(prefix.length).trimEnd('/'))
        }
        return ServerAddress.parse(t)
    }
}

/** Quake text: colour codes out, the game font's private glyphs dropped. */
object Text {
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
            if (c.code < 32) sb.append(' ') else if (c !in ''..'' && c != '�') sb.append(c)
            i++
        }
        return sb.toString().trim()
    }

    private fun hex(c: Char) = c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F'
}
