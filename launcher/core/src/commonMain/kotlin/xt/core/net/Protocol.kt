// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.net

/**
 * The DarkPlaces server-list protocol, as engine/darkplaces/netconn.c speaks
 * it, plus Godnotic's LAN additions from touch_lan.c / touch_lan_logic.h.
 * Every packet is connectionless: four 0xFF bytes, then text.
 *
 *  - To a master (dpmaster): `getservers Xonotic 3 empty full`, or on IPv6
 *    `getserversExt Xonotic 3 empty full ipv4 ipv6`. The reply is
 *    `getserversResponse` followed by `\` + 4 address bytes + 2 port bytes
 *    (big-endian) per server, `/` + 16 + 2 for IPv6 in the Ext form, and the
 *    end-of-transmission entry `\EOT\0\0\0`.
 *  - To a game server: `getinfo <challenge>` or `getstatus <challenge>`. The
 *    reply is `infoResponse\n` / `statusResponse\n` and an info string
 *    `\key\value...` that echoes the challenge; a status reply then lists one
 *    player per line: `<frags or qcstatus> <ping> [team] "<name>"`.
 *  - `qcstatus` is Xonotic's `<gametype>:<version>:P<impure>:S<free>:F<flags>:M<mod>::<score>`.
 *    A Godnotic server with a join password adds `:L1` before the `::`.
 */
object DpProtocol {
    const val GAME = "Xonotic"
    const val PROTOCOL_VERSION = 3
    const val DEFAULT_PORT = 26000
    const val MASTER_PORT = 27950

    /** The masters Xonotic asks (xonotic-common.cfg: sv_master1-4, sv_masterextra1-2). */
    val MASTERS = listOf(
        "dpm4.xonotic.xyz:27777",
        "dpm6.xonotic.xyz:27777",
        "master3.xonotic.org:27950",
        "master4.xonotic.org:42863",
        "master1.xonotic.org:42863",
        "master2.xonotic.org:27950",
    )

    /** The ports a LAN search asks (net_slist_lan_ports' default 26000-26004). */
    val LAN_PORTS = (26000..26004).toList()

    private val HEADER = byteArrayOf(-1, -1, -1, -1)

    fun packet(text: String): ByteArray = HEADER + text.encodeToByteArray()

    fun getServers(ext: Boolean, game: String = GAME, protocol: Int = PROTOCOL_VERSION): ByteArray =
        packet(if (ext) "getserversExt $game $protocol empty full ipv4 ipv6" else "getservers $game $protocol empty full")

    fun getInfo(challenge: String): ByteArray = packet("getinfo $challenge")
    fun getStatus(challenge: String): ByteArray = packet("getstatus $challenge")

    class MasterReply(val servers: List<ServerAddress>, val endOfTransmission: Boolean)

    /** A master's reply, or null when [data] is not one. */
    fun parseMasterReply(data: ByteArray): MasterReply? {
        if (!hasHeader(data)) return null
        val ext: Boolean
        var p: Int
        when {
            startsWith(data, 4, "getserversExtResponse") -> { ext = true; p = 4 + 21 }
            startsWith(data, 4, "getserversResponse") -> { ext = false; p = 4 + 18 }
            else -> return null
        }
        val out = mutableListOf<ServerAddress>()
        var eot = false
        while (data.size - p >= 7) {
            when (data[p].toInt().toChar()) {
                '\\' -> {
                    val port = u8(data, p + 5) * 256 + u8(data, p + 6)
                    val allFF = (1..4).all { u8(data, p + it) == 0xFF }
                    if (port == 0 && data[p + 1].toInt().toChar() == 'E' && data[p + 2].toInt().toChar() == 'O' &&
                        data[p + 3].toInt().toChar() == 'T' && data[p + 4].toInt() == 0) {
                        eot = true
                        break
                    }
                    if (port != 0 && !allFF) {
                        out += ServerAddress("${u8(data, p + 1)}.${u8(data, p + 2)}.${u8(data, p + 3)}.${u8(data, p + 4)}", port)
                    }
                    p += 7
                }
                '/' -> {
                    if (!ext || data.size - p < 19) break
                    val port = u8(data, p + 17) * 256 + u8(data, p + 18)
                    if (port != 0) {
                        val groups = (0 until 8).joinToString(":") { i ->
                            ((u8(data, p + 1 + i * 2) shl 8) or u8(data, p + 2 + i * 2)).toString(16)
                        }
                        out += ServerAddress(groups, port)
                    }
                    p += 19
                }
                else -> break
            }
        }
        return MasterReply(out, eot)
    }

    /** What a game server answered to getinfo or getstatus. */
    class Reply(
        val status: Boolean,
        val info: Map<String, String>,
        val players: List<PlayerInfo>,
    ) {
        val challenge: String? get() = info["challenge"]
    }

    fun parseServerReply(data: ByteArray): Reply? {
        if (!hasHeader(data)) return null
        val status = when {
            startsWith(data, 4, "statusResponse") -> true
            startsWith(data, 4, "infoResponse") -> false
            else -> return null
        }
        // Xonotic sends UTF-8 (utf8_enable); an old server's odd bytes become U+FFFD.
        val text = data.decodeToString(4, data.size)
        val nl = text.indexOf('\n')
        if (nl < 0) return null
        val rest = text.substring(nl + 1)
        val infoEnd = rest.indexOf('\n').let { if (it < 0) rest.length else it }
        val info = parseInfoString(rest.substring(0, infoEnd))
        val players = if (status && infoEnd < rest.length) {
            rest.substring(infoEnd + 1).split('\n').mapNotNull { parsePlayer(it) }
        } else emptyList()
        return Reply(status, info, players)
    }

    /** `\key\value\key\value` (a leading backslash is optional). */
    fun parseInfoString(s: String): Map<String, String> {
        val parts = s.removePrefix("\\").split('\\')
        val out = LinkedHashMap<String, String>()
        var i = 0
        while (i + 1 < parts.size) {
            out[parts[i]] = parts[i + 1]
            i += 2
        }
        return out
    }

    /** One `<score> <ping> [team] "<name>"` line of a status reply. */
    fun parsePlayer(line: String): PlayerInfo? {
        val q = line.indexOf('"')
        if (q < 0) return null
        val name = line.substring(q + 1).removeSuffix("\"").let { if (it.endsWith('"')) it.dropLast(1) else it }
        val head = line.substring(0, q).trim().split(' ').filter { it.isNotEmpty() }
        if (head.size < 2) return null
        val score = head[0].substringBefore(':').toIntOrNull() ?: head[0].toIntOrNull() ?: 0
        val ping = head[1].toIntOrNull() ?: 0
        val team = head.getOrNull(2)?.toIntOrNull()
        return PlayerInfo(name, score, ping, team)
    }

    class QcStatus(
        val gametype: String,
        val version: String,
        val impure: Int,
        val freeSlots: Int?,
        val flags: Int,
        val mod: String,
        val locked: Boolean,
    )

    fun parseQcStatus(s: String?): QcStatus? {
        if (s.isNullOrEmpty()) return null
        val f = s.split(':')
        var impure = 0
        var free: Int? = null
        var flags = 0
        var mod = ""
        var locked = false
        for (j in 2 until f.size) {
            val v = f[j]
            if (v.isEmpty()) break // "::" starts the score part
            val rest = v.substring(1)
            when (v[0]) {
                'P' -> impure = rest.toIntOrNull() ?: 0
                'S' -> free = rest.toIntOrNull()
                'F' -> flags = rest.toIntOrNull() ?: 0
                'M' -> mod = rest.lowercase()
                'L' -> locked = rest == "1"
            }
        }
        return QcStatus(f[0], f.getOrElse(1) { "" }, impure, free, flags, mod, locked)
    }

    /** Builds a [ServerInfo] from a reply; null when it is not a Xonotic game. */
    fun toServerInfo(address: ServerAddress, reply: Reply, pingMs: Int, lan: Boolean): ServerInfo? {
        val i = reply.info
        val game = i["gamename"].orEmpty()
        if (game.isNotEmpty() && !game.equals(GAME, ignoreCase = true)) return null
        val qc = parseQcStatus(i["qcstatus"])
        val clients = i["clients"]?.toIntOrNull() ?: 0
        val bots = (i["bots"]?.toIntOrNull() ?: 0).coerceAtLeast(0)
        return ServerInfo(
            address = address,
            name = i["hostname"].orEmpty().ifBlank { address.toString() },
            map = i["mapname"].orEmpty(),
            gametype = qc?.gametype.orEmpty(),
            version = qc?.version.orEmpty(),
            players = clients,
            bots = bots,
            maxPlayers = i["sv_maxclients"]?.toIntOrNull() ?: 0,
            protocol = i["protocol"]?.toIntOrNull() ?: -1,
            locked = qc?.locked == true,
            mod = qc?.mod.orEmpty(),
            impure = qc?.impure ?: 0,
            pingMs = pingMs,
            lan = lan || address.isLan,
            playerList = reply.players,
            encrypted = i.containsKey("d0_blind_id"),
        )
    }

    fun hasHeader(d: ByteArray) = d.size >= 4 && d[0] == HEADER[0] && d[1] == HEADER[1] && d[2] == HEADER[2] && d[3] == HEADER[3]

    private fun startsWith(d: ByteArray, at: Int, s: String): Boolean {
        if (d.size - at < s.length) return false
        for (k in s.indices) if (d[at + k].toInt() != s[k].code) return false
        return true
    }

    private fun u8(d: ByteArray, i: Int) = d[i].toInt() and 0xFF

    fun latin1(d: ByteArray, from: Int, to: Int): String {
        val sb = StringBuilder(to - from)
        for (k in from until to) sb.append((d[k].toInt() and 0xFF).toChar())
        return sb.toString()
    }
}

/**
 * Godnotic's LAN rules (touch_lan_logic.h), widened for VPNs: the browser and
 * the Host page treat overlay networks (Tailscale/Headscale, ZeroTier,
 * WireGuard, Nebula, tun/tap) as places a game can be found and joined, and
 * skip only loopback and cellular modems (whose addresses only the carrier
 * sees).
 */
object LanRules {
    const val IF_UP = 1
    const val IF_LOOPBACK = 2
    const val IF_POINTOPOINT = 4
    const val IF_BROADCAST = 8

    enum class Kind(val label: String) {
        WIFI("Wi-Fi"), ETHERNET("Ethernet"), HOTSPOT("Hotspot"), VPN("VPN"), OTHER("Network"), CELLULAR("Mobile data"), LOOPBACK("This device")
    }

    /** The subnet-directed broadcast of an interface, or 0 when there is none worth sending to. */
    fun directedBroadcast(addr: Long, mask: Long): Long {
        val a = addr and 0xFFFFFFFFL
        val m = mask and 0xFFFFFFFFL
        if (a == 0L || (a ushr 24) == 127L) return 0
        if (m == 0xFFFFFFFFL || m == 0xFFFFFFFEL) return 0
        return ((a and m) or (m.inv() and 0xFFFFFFFFL)) and 0xFFFFFFFFL
    }

    fun isLanIPv4(a: Long): Boolean {
        val x = a and 0xFFFFFFFFL
        return (x ushr 24) == 10L || (x ushr 24) == 127L ||
            (x ushr 20) == ((172L shl 4) or 1L) ||
            (x ushr 16) == ((192L shl 8) or 168L) ||
            (x ushr 16) == ((169L shl 8) or 254L) ||
            (x ushr 22) == ((100L shl 2) or 1L)
    }

    private val cellular = listOf("rmnet", "ccmni", "pdp", "seth_", "v4-", "clat", "wwan", "umts", "ppp")
    private val vpn = listOf("tailscale", "ts", "zt", "wg", "nebula", "tun", "tap", "utun", "ipsec", "vpn", "headscale", "nordlynx", "proton", "mullvad", "gpd")
    private val wifi = listOf("wlan", "wl", "wlp")
    private val hotspot = listOf("swlan", "ap", "softap", "rndis", "usb", "ncm", "bt-pan", "bnep")
    private val ethernet = listOf("eth", "en")
    private val never = listOf("dummy", "ifb", "sit", "ip6", "gre", "erspan", "vti", "lo")

    fun kind(name: String?, flags: Int): Kind {
        if (flags and IF_LOOPBACK != 0) return Kind.LOOPBACK
        val n = name.orEmpty()
        return when {
            never.any { n.startsWith(it) } -> Kind.LOOPBACK
            cellular.any { n.startsWith(it) } -> Kind.CELLULAR
            vpn.any { n.startsWith(it) } || flags and IF_POINTOPOINT != 0 -> Kind.VPN
            hotspot.any { n.startsWith(it) } -> Kind.HOTSPOT
            wifi.any { n.startsWith(it) } -> Kind.WIFI
            ethernet.any { n.startsWith(it) } -> Kind.ETHERNET
            else -> Kind.OTHER
        }
    }

    /**
     * 0: not usable (down, loopback, cellular); 3 Wi-Fi/Ethernet/hotspot; 2 VPN; 1 anything else.
     * Unlike touch_lan_logic.h's TouchLAN_InterfaceRank, tunnels and point-to-point links count.
     */
    fun interfaceRank(name: String?, flags: Int): Int {
        if (flags and IF_UP == 0) return 0
        return when (kind(name, flags)) {
            Kind.LOOPBACK, Kind.CELLULAR -> 0
            Kind.WIFI, Kind.ETHERNET, Kind.HOTSPOT -> 3
            Kind.VPN -> 2
            Kind.OTHER -> 1
        }
    }

    fun ipv4ToLong(s: String): Long? {
        val p = s.split('.')
        if (p.size != 4) return null
        var v = 0L
        for (x in p) {
            val n = x.toIntOrNull() ?: return null
            if (n !in 0..255) return null
            v = (v shl 8) or n.toLong()
        }
        return v
    }

    fun longToIpv4(v: Long) = "${(v ushr 24) and 255}.${(v ushr 16) and 255}.${(v ushr 8) and 255}.${v and 255}"

    fun prefixLength(mask: Long): Int = (0 until 32).count { (mask ushr (31 - it)) and 1L == 1L }

    /** A network interface as the platform reports it. */
    data class Interface(val name: String, val address: String, val netmask: String, val flags: Int) {
        val kind get() = kind(name, flags)
        val rank get() = interfaceRank(name, flags)
        /** "Wi-Fi 192.168.1.23", "VPN (tailscale0) 100.101.1.2". */
        fun label(): String = when (kind) {
            Kind.VPN, Kind.OTHER -> "${kind.label} ($name)"
            else -> kind.label
        }
    }

    /**
     * Broadcast targets for a LAN search: each usable interface's directed
     * broadcast (VPNs too: an L2 overlay such as ZeroTier or a tap carries
     * them), then 255.255.255.255.
     */
    fun broadcastTargets(ifaces: List<Interface>): List<String> {
        val out = LinkedHashSet<String>()
        for (i in ifaces.sortedByDescending { it.rank }) {
            if (i.rank == 0) continue
            if (i.flags and IF_POINTOPOINT != 0 && i.flags and IF_BROADCAST == 0) continue
            val a = ipv4ToLong(i.address) ?: continue
            val m = ipv4ToLong(i.netmask) ?: continue
            val b = directedBroadcast(a, m)
            if (b != 0L) out += longToIpv4(b)
        }
        out += "255.255.255.255"
        return out.toList()
    }

    /**
     * Unicast sweep targets: every other host address of a small VPN subnet
     * (a /24 or smaller), where a broadcast may never arrive (WireGuard, Nebula,
     * routed tun). Bigger overlays (Tailscale's 100.64/10) are covered by their
     * peer list and remembered hosts instead.
     */
    fun sweepTargets(ifaces: List<Interface>, maxPrefixHosts: Int = 254): List<String> {
        val out = LinkedHashSet<String>()
        for (i in ifaces) {
            if (i.rank == 0 || i.kind != Kind.VPN) continue
            val a = ipv4ToLong(i.address) ?: continue
            val m = ipv4ToLong(i.netmask) ?: continue
            val len = prefixLength(m)
            if (len < 24 || len >= 31) continue
            val hosts = (1L shl (32 - len)) - 2
            if (hosts > maxPrefixHosts) continue
            val net = a and m
            for (h in 1..hosts) {
                val ip = net + h
                if (ip != a) out += longToIpv4(ip)
            }
        }
        return out.toList()
    }

    /** The addresses others can join at, best first, with a label. */
    fun joinAddresses(ifaces: List<Interface>, port: Int): List<Pair<String, String>> =
        ifaces.filter { it.rank > 0 && ipv4ToLong(it.address) != null }
            .sortedByDescending { it.rank }
            .map { it.label() to "${it.address}:$port" }.distinctBy { it.second }

    // Reject reasons (touch_lan_logic.h): what the engine prints for a password problem.
    const val REJECT_REQUIRED = "[pw-required] "
    const val REJECT_WRONG = "[pw-wrong] "
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

/** Tailscale's local peer list (`tailscale status --json`): IPv4 addresses of online peers. */
object TailscalePeers {
    fun parse(json: String): List<String> {
        // Small purpose-built scan: "TailscaleIPs":["100.x.y.z","fd7a:..."] inside "Peer" entries, with "Online":true.
        val out = mutableListOf<String>()
        val peerAt = json.indexOf("\"Peer\"")
        if (peerAt < 0) return out
        var i = peerAt
        val ipsKey = "\"TailscaleIPs\""
        while (true) {
            val k = json.indexOf(ipsKey, i)
            if (k < 0) break
            val open = json.indexOf('[', k)
            val close = json.indexOf(']', open)
            if (open < 0 || close < 0) break
            val ips = Regex("\"([0-9.]+)\"").findAll(json.substring(open, close)).map { it.groupValues[1] }.toList()
            // The entry's Online flag lies within the same object; look for it up to the next ipsKey.
            val next = json.indexOf(ipsKey, close).let { if (it < 0) json.length else it }
            val objText = json.substring(maxOf(peerAt, json.lastIndexOf('{', k)), next)
            val online = !objText.contains("\"Online\": false") && !objText.contains("\"Online\":false")
            if (online) out += ips.filter { LanRules.ipv4ToLong(it) != null }
            i = close
        }
        return out.distinct()
    }
}
