// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import xt.core.net.*
import kotlin.test.*

class NetProtocolTest {
    private fun bytes(vararg parts: Any): ByteArray {
        var out = byteArrayOf()
        for (p in parts) out += when (p) {
            is String -> p.encodeToByteArray()
            is ByteArray -> p
            is Int -> byteArrayOf(p.toByte())
            else -> error("$p")
        }
        return out
    }

    private val ff = byteArrayOf(-1, -1, -1, -1)

    @Test
    fun masterQueryPackets() {
        assertEquals("getservers Xonotic 3 empty full", DpProtocol.getServers(false).decodeToString(4))
        assertEquals("getserversExt Xonotic 3 empty full ipv4 ipv6", DpProtocol.getServers(true).decodeToString(4))
        assertTrue(DpProtocol.hasHeader(DpProtocol.getInfo("x")))
        assertEquals("getinfo abc", DpProtocol.getInfo("abc").decodeToString(4))
        assertEquals("getstatus abc", DpProtocol.getStatus("abc").decodeToString(4))
    }

    @Test
    fun masterReplyParsing() {
        val d = bytes(ff, "getserversResponse",
            "\\", 192, 168, 1, 23, 0x65, 0x90,      // 192.168.1.23:26000
            "\\", 255, 255, 255, 255, 0x65, 0x90,   // broadcast: skipped
            "\\", 1, 2, 3, 4, 0, 0,                 // port 0: skipped
            "\\", 'E'.code, 'O'.code, 'T'.code, 0, 0, 0)
        val r = assertNotNull(DpProtocol.parseMasterReply(d))
        assertEquals(listOf(ServerAddress("192.168.1.23", 26000)), r.servers)
        assertTrue(r.endOfTransmission)

        val ext = bytes(ff, "getserversExtResponse", "/", ByteArray(15), 1, 0x65, 0x91, "\\", 10, 0, 0, 1, 0x65, 0x90)
        val e = assertNotNull(DpProtocol.parseMasterReply(ext))
        assertEquals(listOf(ServerAddress("0:0:0:0:0:0:0:1", 26001), ServerAddress("10.0.0.1", 26000)), e.servers)
        assertFalse(e.endOfTransmission)
        assertEquals("[0:0:0:0:0:0:0:1]:26001", e.servers[0].toString())

        assertNull(DpProtocol.parseMasterReply(bytes(ff, "infoResponse\n")))
        assertNull(DpProtocol.parseMasterReply("getserversResponse".encodeToByteArray()))
    }

    @Test
    fun serverReplyParsing() {
        val text = "statusResponse\n\\gamename\\Xonotic\\modname\\data\\gameversion\\20000\\sv_maxclients\\16" +
            "\\clients\\5\\bots\\2\\mapname\\stormkeep\\hostname\\^1My ^7server\\protocol\\3" +
            "\\qcstatus\\ctf:0.8.6:P0:S11:F6:MXonotic:L1::score!!\\challenge\\xyz\n" +
            "0 0 1 \"[BOT]Hellfire\"\n15 52 2 \"Player\"\n"
        val r = assertNotNull(DpProtocol.parseServerReply(ff + text.encodeToByteArray()))
        assertTrue(r.status)
        assertEquals("xyz", r.challenge)
        assertEquals(2, r.players.size)
        assertEquals(PlayerInfo("Player", 15, 52, 2), r.players[1])
        val info = assertNotNull(DpProtocol.toServerInfo(ServerAddress("192.168.1.5", 26000), r, 12, lan = false))
        assertEquals("ctf", info.gametype)
        assertEquals("0.8.6", info.version)
        assertTrue(info.locked)
        assertTrue(info.lan, "a private address is a LAN game")
        assertEquals(3, info.humans)
        assertEquals("My server", Text.plain(info.name))

        val other = ff + "infoResponse\n\\gamename\\Nexuiz\\hostname\\x".encodeToByteArray()
        assertNull(DpProtocol.toServerInfo(ServerAddress("1.1.1.1"), DpProtocol.parseServerReply(other)!!, 1, false))
    }

    @Test
    fun qcstatusFieldsAndTheLockMarker() {
        val q = assertNotNull(DpProtocol.parseQcStatus("dm:0.8.6:P2:S5:F2:MXonotic::score!!"))
        assertEquals("dm", q.gametype)
        assertEquals(2, q.impure)
        assertEquals(5, q.freeSlots)
        assertEquals("xonotic", q.mod)
        assertFalse(q.locked)
        // touch_lan_logic.h inserts ":L1" before the "::".
        assertTrue(DpProtocol.parseQcStatus("dm:0.8.6:P0:S5:F2:MXonotic:L1::score")!!.locked)
        assertTrue(DpProtocol.parseQcStatus("dm:0.8.6:L1")!!.locked)
        // after "::" the score part is not read
        assertFalse(DpProtocol.parseQcStatus("dm:0.8.6:P0::L1")!!.locked)
        assertNull(DpProtocol.parseQcStatus(""))
    }

    @Test
    fun addresses() {
        assertEquals(ServerAddress("192.168.1.23", 26000), ServerAddress.parse("192.168.1.23"))
        assertEquals(ServerAddress("192.168.1.23", 26001), ServerAddress.parse(" 192.168.1.23:26001 "))
        assertEquals(ServerAddress("fe80::1", 26000), ServerAddress.parse("[fe80::1]:26000"))
        assertEquals(ServerAddress("::1", 26000), ServerAddress.parse("::1"))
        assertEquals(ServerAddress("example.org", 26010), ServerAddress.parse("example.org:26010"))
        assertNull(ServerAddress.parse("1.2.3.4:99999"))
        assertNull(ServerAddress.parse("1.2.3.4; quit"))
        assertNull(ServerAddress.parse("a\"b"))
        assertNull(ServerAddress.parse(""))
        assertTrue(ServerAddress("10.1.2.3").isLan)
        assertTrue(ServerAddress("100.64.0.9").isLan)
        assertFalse(ServerAddress("8.8.8.8").isLan)
    }

    @Test
    fun lanRulesMatchTouchLanLogic() {
        // tests/lan/lan_test.c's cases
        assertEquals(LanRules.ipv4ToLong("192.168.1.255"), LanRules.directedBroadcast(LanRules.ipv4ToLong("192.168.1.23")!!, LanRules.ipv4ToLong("255.255.255.0")!!))
        assertEquals(0L, LanRules.directedBroadcast(LanRules.ipv4ToLong("127.0.0.1")!!, LanRules.ipv4ToLong("255.0.0.0")!!))
        assertEquals(0L, LanRules.directedBroadcast(LanRules.ipv4ToLong("10.0.0.1")!!, 0xFFFFFFFFL))
        assertEquals(0L, LanRules.directedBroadcast(LanRules.ipv4ToLong("10.0.0.1")!!, 0xFFFFFFFEL))
        assertEquals(3, LanRules.interfaceRank("wlan0", LanRules.IF_UP))
        assertEquals(3, LanRules.interfaceRank("enp3s0", LanRules.IF_UP))
        assertEquals(0, LanRules.interfaceRank("rmnet_data0", LanRules.IF_UP))
        // VPNs count (Tailscale, WireGuard, ZeroTier, tun): unlike the stock rule they are not skipped.
        assertEquals(2, LanRules.interfaceRank("tun0", LanRules.IF_UP))
        assertEquals(2, LanRules.interfaceRank("tailscale0", LanRules.IF_UP or LanRules.IF_POINTOPOINT))
        assertEquals(LanRules.Kind.VPN, LanRules.kind("ztabcdef", LanRules.IF_UP or LanRules.IF_BROADCAST))
        assertEquals(LanRules.Kind.VPN, LanRules.kind("wg0", LanRules.IF_UP or LanRules.IF_POINTOPOINT))
        assertEquals(0, LanRules.interfaceRank("lo", LanRules.IF_UP or LanRules.IF_LOOPBACK))
        assertEquals(0, LanRules.interfaceRank("wlan0", 0))
        assertEquals(1, LanRules.interfaceRank("docker0", LanRules.IF_UP))
        val up = LanRules.IF_UP or LanRules.IF_BROADCAST
        val ifs = listOf(
            LanRules.Interface("lo", "127.0.0.1", "255.0.0.0", LanRules.IF_UP or LanRules.IF_LOOPBACK),
            LanRules.Interface("docker0", "172.17.0.1", "255.255.0.0", up),
            LanRules.Interface("wlan0", "192.168.1.23", "255.255.255.0", up),
            LanRules.Interface("ztabc", "10.147.17.5", "255.255.255.0", up),
            LanRules.Interface("wg0", "10.8.0.2", "255.255.255.248", LanRules.IF_UP or LanRules.IF_POINTOPOINT),
            LanRules.Interface("tailscale0", "100.101.102.103", "255.255.255.255", LanRules.IF_UP or LanRules.IF_POINTOPOINT),
            LanRules.Interface("rmnet0", "10.200.1.1", "255.255.255.0", up),
        )
        // ZeroTier carries broadcasts; WireGuard and Tailscale (point-to-point, no broadcast flag) do not.
        assertEquals(listOf("192.168.1.255", "10.147.17.255", "172.17.255.255", "255.255.255.255"), LanRules.broadcastTargets(ifs))
        // Small VPN subnets are swept by unicast (ZeroTier's /24 and WireGuard's /29); Tailscale's /32 is not.
        val sweep = LanRules.sweepTargets(ifs)
        assertEquals(253 + 5, sweep.size)
        assertTrue("10.8.0.1" in sweep && "10.8.0.6" in sweep && "10.8.0.2" !in sweep && "10.147.17.5" !in sweep)
        assertTrue(sweep.none { it.startsWith("100.") || it.startsWith("192.168.") })
        assertEquals(
            listOf("Wi-Fi" to "192.168.1.23:26001", "VPN (ztabc)" to "10.147.17.5:26001",
                "VPN (wg0)" to "10.8.0.2:26001", "VPN (tailscale0)" to "100.101.102.103:26001", "Network (docker0)" to "172.17.0.1:26001"),
            LanRules.joinAddresses(ifs, 26001))
    }

    @Test
    fun joinLinksAndTailscalePeers() {
        assertEquals("godnotic://join/100.101.1.2:26000", JoinLink.of(ServerAddress("100.101.1.2")))
        assertEquals(ServerAddress("100.101.1.2", 26001), JoinLink.parse("godnotic://join/100.101.1.2:26001"))
        assertEquals(ServerAddress("100.101.1.2", 26001), JoinLink.parse("godnotic://join/100.101.1.2:26001/"))
        assertEquals(ServerAddress("10.0.0.5", 26000), JoinLink.parse("10.0.0.5"))
        assertNull(JoinLink.parse("godnotic://other/1.2.3.4"))
        assertNull(JoinLink.parse("godnotic://join/1.2.3.4;quit"))
        val json = """{"Self":{"TailscaleIPs":["100.64.0.1"]},"Peer":{"k1":{"HostName":"a","TailscaleIPs":["100.64.0.2","fd7a::2"],"Online":true},
            "k2":{"HostName":"b","TailscaleIPs":["100.64.0.3"],"Online":false},"k3":{"TailscaleIPs":["100.64.0.4"],"Online":true}}}"""
        assertEquals(listOf("100.64.0.2", "100.64.0.4"), TailscalePeers.parse(json))
        assertTrue(TailscalePeers.parse("{}").isEmpty())
    }

    private fun srv(name: String, ping: Int, humans: Int, lan: Boolean = false, map: String = "m", gt: String = "dm", max: Int = 8) =
        ServerInfo(ServerAddress("1.2.3.${name.length}", 26000 + ping), name, map, gt, players = humans, bots = 0, maxPlayers = max, pingMs = ping, lan = lan)

    @Test
    fun sortAndFilter() {
        val a = srv("^1Alpha", 80, 2)
        val b = srv("Bravo", 20, 0, map = "afterslime")
        val c = srv("Charlie LAN", 5, 0, lan = true, gt = "ctf")
        val d = srv("Delta", 40, 8, max = 8)
        val all = listOf(a, b, c, d)
        assertEquals(listOf(c, b, d, a), ServerList.sort(all, ServerList.Sort.PING))
        assertEquals(listOf(c, d, a, b), ServerList.sort(all, ServerList.Sort.PLAYERS))
        assertEquals(listOf(c, a, b, d), ServerList.sort(all, ServerList.Sort.NAME))
        // favourites go right after LAN games
        assertEquals(listOf(c, a, b, d), ServerList.sort(all, ServerList.Sort.PING, setOf(a.address.toString())))
        assertEquals(listOf(a), ServerList.filter(all, "alpha"))
        assertEquals(listOf(b), ServerList.filter(all, "afterslime"))
        assertEquals(listOf(c), ServerList.filter(all, "capture"))
        assertEquals(listOf(a, d), ServerList.filter(all, "", hideEmpty = true))
        assertEquals(listOf(a, b, c), ServerList.filter(all, "", hideFull = true))
    }

    @Test
    fun cacheRoundTripAndFavourites() {
        val list = listOf(srv("A\tb", 10, 1, lan = true).copy(locked = true, version = "0.8.6"), srv("B", 20, 0))
        val back = ServerCache.parse(ServerCache.serialize(list))
        assertEquals(2, back.size)
        assertEquals("A b", back[0].name)
        assertTrue(back[0].locked && back[0].lan && back[0].stale)
        assertEquals(-1, back[0].pingMs)
        assertEquals(list[1].address, back[1].address)
        assertTrue(ServerCache.parse("garbage\n1.2.3.4:5\tx").isEmpty())
        val f = Favourites.parse("1.2.3.4:26000 bad;thing [::1]:26001")
        assertEquals(listOf("1.2.3.4:26000", "[::1]:26001"), f.toList())
        assertEquals("1.2.3.4:26000 [::1]:26001", Favourites.serialize(f))
    }

    @Test
    fun plainText() {
        assertEquals("Red Blue", Text.plain("^1Red ^x0F0Blue"))
        assertEquals("a^b", Text.plain("a^^b"))
        assertEquals("tab x", Text.plain("tab\u0001x").replace("  ", " "))
    }
}
