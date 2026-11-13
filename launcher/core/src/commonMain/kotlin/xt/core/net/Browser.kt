// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalAtomicApi::class)

package xt.core.net

import kotlin.concurrent.atomics.AtomicBoolean
import kotlin.concurrent.atomics.ExperimentalAtomicApi
import kotlin.random.Random

/** One received datagram. */
class Datagram(val host: String, val port: Int, val data: ByteArray)

/** A UDP socket; the platform's implementation is blocking with a timeout. */
interface UdpSocket {
    /** [host] is a numeric address. False when the send failed (unreachable network...). */
    fun send(host: String, port: Int, data: ByteArray): Boolean
    /** Waits up to [timeoutMs]; null on timeout. */
    fun receive(timeoutMs: Int): Datagram?
    fun close()
}

/** What the browser needs from the platform: sockets, name lookup, interfaces, a clock. */
interface NetPlatform {
    /** An IPv4 UDP socket on an ephemeral port; [broadcast] sets SO_BROADCAST. */
    fun openUdp(broadcast: Boolean): UdpSocket?
    /** Numeric IPv4 addresses for a name (or the literal itself). */
    fun resolve(host: String): List<String>
    fun interfaces(): List<LanRules.Interface>
    fun nowMs(): Long
}

/**
 * Asks the masters and the servers, or only the local network, and reports
 * each server as its answer arrives. [run] blocks; call it from a worker
 * thread and [cancel] from anywhere.
 *
 * Internet: every master gets `getservers`; each server address it returns
 * (and every cached or favourite address, at once) gets `getinfo` with a
 * challenge, at most [burst] per [burstGapMs]. A server that has not answered
 * after [retryAfterMs] is asked once more. The run ends when every master
 * sent its end marker (or [masterTimeoutMs] passed) and every server answered
 * or timed out.
 *
 * LAN (touch_lan.c's search): `getinfo` broadcast to each interface's
 * directed broadcast address and 255.255.255.255, on every LAN port, repeated
 * [lanRepeats] more times [lanRepeatDelayMs] apart, plus the cached LAN
 * servers asked directly. Ends [lanWaitMs] after the first send.
 *
 * Ping is the time from the last query sent to that server (matched by the
 * challenge it echoes) to its answer, as the engine measures it.
 */
class ServerBrowser(
    private val net: NetPlatform,
    val masters: List<String> = DpProtocol.MASTERS,
    val lanPorts: List<Int> = DpProtocol.LAN_PORTS,
) {
    var burst = 24
    var burstGapMs = 40L
    var retryAfterMs = 1200L
    var serverTimeoutMs = 2500L
    var masterTimeoutMs = 3000L
    var lanRepeats = 2
    var lanRepeatDelayMs = 350L
    var lanWaitMs = 1500L
    /** Overrides the broadcast targets (tests); null = from the interfaces. */
    var broadcastOverride: List<String>? = null
    /** Overrides the VPN sweep targets (tests); null = from the interfaces. */
    var sweepOverride: List<String>? = null
    /** Hosts a LAN search always asks directly: favourites, remembered hosts, a VPN's peers. */
    var extraHosts: List<String> = emptyList()
    var sweepBurst = 64
    var sweepGapMs = 25L

    private val stopped = AtomicBoolean(false)
    fun cancel() = stopped.store(true)
    val cancelled get() = stopped.load()

    enum class Mode { INTERNET, LAN }

    class Stats(val mastersAsked: Int, val mastersAnswered: Int, val serversAsked: Int, val serversAnswered: Int, val ms: Long)

    interface Listener {
        fun onServer(info: ServerInfo)
        fun onMasterList(master: String, count: Int) {}
        fun onDone(stats: Stats)
    }

    private class Pending(val address: ServerAddress, var sentAt: Long, var tries: Int, val lanHint: Boolean)
    private class Query(val p: Pending, val sentAt: Long)

    fun run(mode: Mode, known: Collection<ServerAddress>, listener: Listener) {
        val t0 = net.nowMs()
        val sock = net.openUdp(broadcast = mode == Mode.LAN)
        if (sock == null) {
            listener.onDone(Stats(0, 0, 0, 0, 0))
            return
        }
        try {
            if (mode == Mode.LAN) runLan(sock, known, listener, t0) else runInternet(sock, known, listener, t0)
        } finally {
            sock.close()
        }
    }

    private val salt = Random.nextLong().toULong().toString(36)
    private var seq = 0
    private fun challenge() = "xt$salt${(seq++).toString(36)}"

    private fun runInternet(sock: UdpSocket, known: Collection<ServerAddress>, listener: Listener, t0: Long) {
        val byChallenge = HashMap<String, Query>()
        val byAddress = LinkedHashMap<String, Pending>()
        val queue = ArrayDeque<ServerAddress>()
        val answered = HashSet<String>()
        known.forEach { queue += it }

        // Masters: resolve each (DNS can be slow; one bad name must not stall the rest).
        val masterAddrs = HashMap<String, String>() // "ip:port" -> name
        for (m in masters) {
            if (cancelled) return
            val a = ServerAddress.parse(m, DpProtocol.MASTER_PORT) ?: continue
            val ips = net.resolve(a.host)
            for (ip in ips.take(2)) {
                if (sock.send(ip, a.port, DpProtocol.getServers(ext = false))) masterAddrs["$ip:${a.port}"] = m
            }
        }
        val mastersAsked = masterAddrs.values.toSet().size
        val masterDone = HashSet<String>()
        val masterSeen = HashSet<String>()
        var lastBurst = 0L

        fun pump(now: Long) {
            // New addresses, a burst at a time.
            if (now - lastBurst >= burstGapMs) {
                var n = 0
                while (n < burst && queue.isNotEmpty()) {
                    val a = queue.removeFirst()
                    val key = a.toString()
                    if (key in byAddress) continue
                    val ip = if (LanRules.ipv4ToLong(a.host) != null || a.isIPv6) a.host else net.resolve(a.host).firstOrNull() ?: continue
                    val p = Pending(ServerAddress(ip, a.port), now, 1, a.isLan)
                    byAddress[key] = p
                    val c = challenge()
                    byChallenge[c] = Query(p, p.sentAt)
                    if (!a.isIPv6) sock.send(ip, a.port, DpProtocol.getInfo(c))
                    n++
                }
                lastBurst = now
            }
            // One retry for the silent ones.
            for (p in byAddress.values) {
                if (p.tries == 1 && p.address.toString() !in answered && now - p.sentAt > retryAfterMs) {
                    p.tries = 2
                    p.sentAt = now
                    val c = challenge()
                    byChallenge[c] = Query(p, p.sentAt)
                    sock.send(p.address.host, p.address.port, DpProtocol.getInfo(c))
                }
            }
        }

        while (!cancelled) {
            val now = net.nowMs()
            pump(now)
            val mastersFinished = masterDone.size >= masterAddrs.size || now - t0 > masterTimeoutMs
            val serversFinished = queue.isEmpty() && byAddress.values.all {
                it.address.toString() in answered || (it.tries >= 2 && now - it.sentAt > serverTimeoutMs - retryAfterMs)
            }
            if (mastersFinished && serversFinished) break
            if (now - t0 > masterTimeoutMs + serverTimeoutMs + 4000) break // hard cap
            val d = sock.receive(if (queue.isNotEmpty()) burstGapMs.toInt() else 100) ?: continue
            val from = "${d.host}:${d.port}"
            val master = masterAddrs[from]
            if (master != null) {
                val r = DpProtocol.parseMasterReply(d.data) ?: continue
                masterSeen += master
                r.servers.forEach { queue += it }
                listener.onMasterList(master, r.servers.size)
                if (r.endOfTransmission) masterDone += from
                continue
            }
            handleReply(d, byChallenge, answered, listener, lan = false)
        }
        listener.onDone(Stats(mastersAsked, masterSeen.size, byAddress.size, answered.size, net.nowMs() - t0))
    }

    private fun handleReply(d: Datagram, byChallenge: Map<String, Query>, answered: MutableSet<String>, listener: Listener, lan: Boolean) {
        val r = DpProtocol.parseServerReply(d.data) ?: return
        val c = r.challenge ?: return
        val q = byChallenge[c] ?: return // not ours, or forged
        val p = q.p
        val addr = ServerAddress(d.host, d.port)
        val key = addr.toString()
        if (!lan && key != p.address.toString()) return // answered from somewhere we never asked
        if (key in answered) return
        answered += key
        val ping = (net.nowMs() - q.sentAt).toInt().coerceAtLeast(1)
        DpProtocol.toServerInfo(addr, r, ping, lan || p.lanHint)?.let(listener::onServer)
    }

    private fun runLan(sock: UdpSocket, known: Collection<ServerAddress>, listener: Listener, t0: Long) {
        val ifaces = if (broadcastOverride == null) net.interfaces() else emptyList()
        val targets = broadcastOverride ?: LanRules.broadcastTargets(ifaces)
        // Unicast: small VPN subnets, the VPN's own peer list, remembered hosts. Each on every LAN port.
        val unicast = LinkedHashSet<String>()
        unicast += sweepOverride ?: LanRules.sweepTargets(ifaces)
        unicast += extraHosts.filter { LanRules.ipv4ToLong(it) != null }
        val unicastQueue = ArrayDeque<ServerAddress>()
        for (h in unicast) for (port in lanPorts) unicastQueue += ServerAddress(h, port)
        val byChallenge = HashMap<String, Query>()
        val answered = HashSet<String>()
        var sent = 0
        fun ask(host: String, port: Int, now: Long) {
            val p = Pending(ServerAddress(host, port), now, 1, true)
            val c = challenge()
            byChallenge[c] = Query(p, p.sentAt)
            if (sock.send(host, port, DpProtocol.getInfo(c))) sent++
        }
        fun round(now: Long) {
            for (t in targets) for (port in lanPorts) ask(t, port, now)
            // Servers that answered last time, directly (net_slist_cache): found even where broadcasts are filtered.
            for (a in known) if (!a.isIPv6 && LanRules.ipv4ToLong(a.host) != null) ask(a.host, a.port, now)
        }
        round(net.nowMs())
        var repeats = 0
        var lastRound = net.nowMs()
        var lastSweep = 0L
        // The sweep stretches the wait so every address is asked and has time to answer.
        val sweepMs = (unicastQueue.size / sweepBurst + 1) * sweepGapMs
        val wait = lanWaitMs + if (unicastQueue.isEmpty()) 0 else sweepMs
        while (!cancelled) {
            val now = net.nowMs()
            if (repeats < lanRepeats && now - lastRound >= lanRepeatDelayMs) {
                repeats++
                lastRound = now
                round(now)
            }
            if (unicastQueue.isNotEmpty() && now - lastSweep >= sweepGapMs) {
                lastSweep = now
                repeat(minOf(sweepBurst, unicastQueue.size)) {
                    val a = unicastQueue.removeFirst()
                    ask(a.host, a.port, now)
                }
            }
            if (now - t0 > wait) break
            val d = sock.receive(if (unicastQueue.isEmpty()) 50 else sweepGapMs.toInt()) ?: continue
            handleReply(d, byChallenge, answered, listener, lan = true)
        }
        listener.onDone(Stats(0, 0, sent, answered.size, net.nowMs() - t0))
    }

    /**
     * Asks one server for its full status (player list). Null when it does not
     * answer within [timeoutMs]; it is asked twice.
     */
    fun status(address: ServerAddress, timeoutMs: Long = 1500): ServerInfo? {
        val sock = net.openUdp(broadcast = false) ?: return null
        try {
            val ip = if (LanRules.ipv4ToLong(address.host) != null) address.host else net.resolve(address.host).firstOrNull() ?: return null
            val t0 = net.nowMs()
            var sentAt = t0
            var c = challenge()
            sock.send(ip, address.port, DpProtocol.getStatus(c))
            var retried = false
            while (!cancelled) {
                val now = net.nowMs()
                if (now - t0 > timeoutMs) return null
                if (!retried && now - t0 > timeoutMs / 2) {
                    retried = true
                    c = challenge()
                    sentAt = now
                    sock.send(ip, address.port, DpProtocol.getStatus(c))
                }
                val d = sock.receive(50) ?: continue
                val r = DpProtocol.parseServerReply(d.data) ?: continue
                if (d.host != ip || d.port != address.port || r.challenge != c) continue
                return DpProtocol.toServerInfo(ServerAddress(ip, address.port), r, (net.nowMs() - sentAt).toInt().coerceAtLeast(1), address.isLan)
            }
            return null
        } finally {
            sock.close()
        }
    }
}

/** Is a UDP port free to host on? Binds it briefly (the engine's _touch_lan_portcheck does the same). */
interface PortProbe {
    fun isFree(port: Int): Boolean
}

object Ports {
    /** [want] if free, else the next free one up to 64 above, else null. */
    fun firstFree(want: Int, probe: PortProbe): Int? =
        (want..minOf(65535, want + 64)).firstOrNull { probe.isFree(it) }
}
