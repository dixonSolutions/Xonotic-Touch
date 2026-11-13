// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.net

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.Collections
import kotlin.concurrent.thread
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.assertNotNull

/**
 * The browser against fake UDP servers on 127.0.0.1: a dpmaster that lists
 * game servers in two packets with the end marker, game servers that answer
 * getinfo/getstatus as netconn.c builds the replies (one with the LAN lock,
 * one silent, one of another game), and the LAN search.
 */
class BrowserTest {
    private val sockets = Collections.synchronizedList(mutableListOf<DatagramSocket>())
    val seenQueries = Collections.synchronizedList(mutableListOf<String>())

    @AfterTest
    fun close() = sockets.forEach { it.close() }

    private fun serve(port: Int = 0, handler: (DatagramSocket, DatagramPacket, String) -> Unit): DatagramSocket {
        val s = DatagramSocket(port, InetAddress.getByName("127.0.0.1"))
        sockets += s
        thread(isDaemon = true) {
            val buf = ByteArray(2048)
            while (!s.isClosed) {
                val p = DatagramPacket(buf, buf.size)
                try { s.receive(p) } catch (_: Exception) { break }
                val text = String(p.data, p.offset + 4, (p.length - 4).coerceAtLeast(0), Charsets.ISO_8859_1)
                seenQueries += text
                handler(s, p, text)
            }
        }
        return s
    }

    private fun reply(s: DatagramSocket, to: DatagramPacket, bytes: ByteArray) {
        s.send(DatagramPacket(bytes, bytes.size, to.address, to.port))
    }

    /** A game server like netconn.c's NetConn_BuildStatusResponse. */
    private fun gameServer(name: String, map: String, qcstatus: String, clients: Int, bots: Int, max: Int, game: String = "Xonotic", delayMs: Long = 0): DatagramSocket =
        serve { s, p, text ->
            val parts = text.split(' ')
            val kind = parts[0]
            val challenge = parts.getOrNull(1)
            if (kind != "getinfo" && kind != "getstatus") return@serve
            if (delayMs > 0) Thread.sleep(delayMs)
            val status = kind == "getstatus"
            val head = (if (status) "statusResponse" else "infoResponse") + "\n" +
                "\\gamename\\$game\\modname\\data\\gameversion\\20000\\sv_maxclients\\$max\\clients\\$clients\\bots\\$bots" +
                "\\mapname\\$map\\hostname\\$name\\protocol\\3\\qcstatus\\$qcstatus" +
                (if (challenge != null) "\\challenge\\$challenge" else "") + (if (status) "\n" else "")
            val players = if (status) "12 0 \"[BOT]Lion\"\n5 48 \"^1Red^7Player\"\n" else ""
            reply(s, p, byteArrayOf(-1, -1, -1, -1) + (head + players).toByteArray(Charsets.ISO_8859_1))
        }

    private fun entry(port: Int): ByteArray = byteArrayOf('\\'.code.toByte(), 127, 0, 0, 1, (port shr 8).toByte(), (port and 255).toByte())
    private val eot = byteArrayOf('\\'.code.toByte(), 'E'.code.toByte(), 'O'.code.toByte(), 'T'.code.toByte(), 0, 0, 0)

    private fun master(ports: List<Int>): DatagramSocket = serve { s, p, text ->
        if (!text.startsWith("getservers Xonotic 3")) return@serve
        val half = ports.size / 2
        val hdr = byteArrayOf(-1, -1, -1, -1) + "getserversResponse".toByteArray()
        var a = hdr
        ports.take(half).forEach { a += entry(it) }
        reply(s, p, a)
        var b = hdr
        ports.drop(half).forEach { b += entry(it) }
        b += eot
        reply(s, p, b)
    }

    private class Collect : ServerBrowser.Listener {
        val servers = Collections.synchronizedList(mutableListOf<ServerInfo>())
        var stats: ServerBrowser.Stats? = null
        override fun onServer(info: ServerInfo) { servers += info }
        override fun onDone(stats: ServerBrowser.Stats) { this.stats = stats }
    }

    @Test
    fun internetListFromMasterWithPingsLockAndFiltering() {
        val a = gameServer("^2Alpha", "stormkeep", "dm:0.8.6:P0:S5:F2:MXonotic::score!!", 3, 1, 8)
        val b = gameServer("Locked LAN", "afterslime", "ctf:0.8.6:P0:S3:F6:MXonotic:L1::score!!", 2, 0, 12)
        val silent = serve { _, _, _ -> } // never answers
        val other = gameServer("Other game", "dm1", "", 1, 0, 8, game = "Nexuiz")
        val m = master(listOf(a.localPort, b.localPort, silent.localPort, other.localPort))

        val br = ServerBrowser(JvmNet(), masters = listOf("127.0.0.1:${m.localPort}"))
        br.retryAfterMs = 300
        br.serverTimeoutMs = 700
        val c = Collect()
        val t0 = System.nanoTime()
        br.run(ServerBrowser.Mode.INTERNET, emptyList(), c)
        val ms = (System.nanoTime() - t0) / 1_000_000

        val stats = assertNotNull(c.stats)
        assertEquals(1, stats.mastersAsked)
        assertEquals(1, stats.mastersAnswered)
        assertEquals(4, stats.serversAsked)
        assertEquals(2, c.servers.size, "Nexuiz and the silent one are not listed: ${c.servers}")
        val alpha = c.servers.first { it.name == "^2Alpha" }
        assertEquals("stormkeep", alpha.map)
        assertEquals("dm", alpha.gametype)
        assertEquals(3, alpha.players)
        assertEquals(1, alpha.bots)
        assertEquals(2, alpha.humans)
        assertEquals(8, alpha.maxPlayers)
        assertTrue(alpha.pingMs in 1..500, "ping ${alpha.pingMs}")
        assertTrue(!alpha.locked)
        val locked = c.servers.first { it.name == "Locked LAN" }
        assertTrue(locked.locked, "qcstatus :L1 marks the password lock")
        assertEquals("ctf", locked.gametype)
        // The silent server was asked twice (one retry), then given up on, without waiting forever.
        val silentAsked = seenQueries.count { it.startsWith("getinfo") }
        assertTrue(silentAsked >= 5, "queries: $seenQueries")
        assertTrue(ms < 4000, "took $ms ms")
    }

    @Test
    fun cachedAndFavouriteServersAreAskedAtOnceWithoutAMaster() {
        val a = gameServer("Fav", "stormkeep", "dm:0.8.6:P0:S5:F2:MXonotic", 0, 0, 8)
        val br = ServerBrowser(JvmNet(), masters = emptyList())
        br.retryAfterMs = 200
        br.serverTimeoutMs = 400
        val c = Collect()
        br.run(ServerBrowser.Mode.INTERNET, listOf(ServerAddress("127.0.0.1", a.localPort)), c)
        assertEquals(listOf("Fav"), c.servers.map { it.name })
    }

    @Test
    fun forgedOrUnaskedRepliesAreIgnored() {
        // Answers with a challenge it made up: not ours, never listed.
        val forger = serve { s, p, _ ->
            reply(s, p, byteArrayOf(-1, -1, -1, -1) + "infoResponse\n\\gamename\\Xonotic\\hostname\\Forged\\challenge\\nope".toByteArray())
        }
        val br = ServerBrowser(JvmNet(), masters = emptyList())
        br.retryAfterMs = 150
        br.serverTimeoutMs = 300
        val c = Collect()
        br.run(ServerBrowser.Mode.INTERNET, listOf(ServerAddress("127.0.0.1", forger.localPort)), c)
        assertTrue(c.servers.isEmpty())
    }

    @Test
    fun lanSearchBroadcastsOnEveryPortAndRepeats() {
        val a = gameServer("LAN host", "stormkeep", "dm:0.8.6:P0:S5:F2:MXonotic:L1::score", 1, 0, 8)
        val b = gameServer("Second host", "afterslime", "tdm:0.8.6:P0:S5:F2:MXonotic::score", 0, 4, 8)
        val br = ServerBrowser(JvmNet(), masters = listOf("must.not.be.asked:1"), lanPorts = listOf(a.localPort, b.localPort))
        br.broadcastOverride = listOf("127.0.0.1")
        br.lanWaitMs = 900
        val c = Collect()
        br.run(ServerBrowser.Mode.LAN, emptyList(), c)
        assertEquals(setOf("LAN host", "Second host"), c.servers.map { it.name }.toSet())
        assertTrue(c.servers.all { it.lan })
        assertTrue(c.servers.first { it.name == "LAN host" }.locked)
        // 1 + 2 repeats per target and port; each server saw three queries, and no master was asked.
        val perServer = seenQueries.count { it.startsWith("getinfo") }
        assertEquals(6, perServer, "$seenQueries")
        assertTrue(seenQueries.none { it.startsWith("getservers") })
    }

    @Test
    fun vpnSweepAndRememberedHostsFindGamesNoBroadcastReaches() {
        // A WireGuard-style link: the broadcast goes nowhere, the unicast sweep and a remembered host find the games.
        val a = gameServer("Over VPN", "stormkeep", "dm:0.8.6:P0:S5:F2:MXonotic::score", 1, 0, 8)
        val br = ServerBrowser(JvmNet(), masters = emptyList(), lanPorts = listOf(a.localPort))
        br.broadcastOverride = listOf("127.0.0.255")
        br.sweepOverride = listOf("127.0.0.3", "127.0.0.1")
        br.extraHosts = listOf("127.0.0.1")
        br.lanRepeats = 0
        br.lanWaitMs = 500
        val c = Collect()
        br.run(ServerBrowser.Mode.LAN, emptyList(), c)
        assertEquals(listOf("Over VPN"), c.servers.map { it.name })
        assertTrue(c.servers[0].lan)
    }

    @Test
    fun statusQueryReturnsPlayers() {
        val a = gameServer("Status", "stormkeep", "dm:0.8.6:P0:S5:F2:MXonotic", 2, 1, 8)
        val br = ServerBrowser(JvmNet())
        val info = assertNotNull(br.status(ServerAddress("127.0.0.1", a.localPort)))
        assertEquals(2, info.playerList.size)
        assertTrue(info.playerList[0].isBot)
        assertEquals("^1Red^7Player", info.playerList[1].name)
        assertEquals(48, info.playerList[1].ping)
    }

    @Test
    fun portProbeSeesATakenPort() {
        val taken = DatagramSocket(0).also { sockets += it }
        val probe = JvmPortProbe()
        assertTrue(!probe.isFree(taken.localPort))
        val next = Ports.firstFree(taken.localPort, probe)
        assertNotNull(next)
        assertTrue(next > taken.localPort)
    }

    /** One read-only query against the real masters; only with XT_NET_LIVE=1 (not in CI). */
    @Test
    fun liveMasters() {
        if (System.getenv("XT_NET_LIVE") != "1") return
        val br = ServerBrowser(JvmNet())
        val c = object : ServerBrowser.Listener {
            val servers = Collections.synchronizedList(mutableListOf<ServerInfo>())
            val masters = Collections.synchronizedList(mutableListOf<String>())
            override fun onServer(info: ServerInfo) { servers += info }
            override fun onMasterList(master: String, count: Int) { masters += "$master:$count" }
            override fun onDone(stats: ServerBrowser.Stats) {
                println("LIVE: masters asked ${stats.mastersAsked}, answered ${stats.mastersAnswered}, servers asked ${stats.serversAsked}, answered ${stats.serversAnswered}, ${stats.ms} ms")
            }
        }
        br.run(ServerBrowser.Mode.INTERNET, emptyList(), c)
        println("LIVE master packets: ${c.masters}")
        ServerList.sort(c.servers, ServerList.Sort.PLAYERS).take(10).forEach {
            println("LIVE ${it.pingMs} ms ${it.humans}+${it.bots}/${it.maxPlayers} ${it.gametype} ${it.map} ${Text.plain(it.name)} ${it.address}")
        }
        assertTrue(c.servers.isNotEmpty(), "no server answered")
    }
}
