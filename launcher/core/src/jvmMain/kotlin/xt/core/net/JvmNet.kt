// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.net

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.SocketTimeoutException

/** [NetPlatform] on the JVM: the tests, and anything else that runs core on a JVM. */
class JvmNet : NetPlatform {
    override fun openUdp(broadcast: Boolean): UdpSocket? = runCatching {
        val s = DatagramSocket(InetSocketAddress("0.0.0.0", 0))
        s.broadcast = broadcast
        JvmUdp(s)
    }.getOrNull()

    override fun resolve(host: String): List<String> = runCatching {
        InetAddress.getAllByName(host).filterIsInstance<Inet4Address>().map { it.hostAddress }
    }.getOrDefault(emptyList())

    override fun interfaces(): List<LanRules.Interface> = runCatching {
        NetworkInterface.getNetworkInterfaces().toList().flatMap { ni ->
            var flags = 0
            if (ni.isUp) flags = flags or LanRules.IF_UP
            if (ni.isLoopback) flags = flags or LanRules.IF_LOOPBACK
            if (ni.isPointToPoint) flags = flags or LanRules.IF_POINTOPOINT
            ni.interfaceAddresses.filter { it.address is Inet4Address }.map { ia ->
                val len = ia.networkPrefixLength.toInt()
                val mask = if (len == 0) 0L else (0xFFFFFFFFL shl (32 - len)) and 0xFFFFFFFFL
                LanRules.Interface(ni.name, ia.address.hostAddress, LanRules.longToIpv4(mask), flags)
            }
        }
    }.getOrDefault(emptyList())

    override fun nowMs(): Long = System.nanoTime() / 1_000_000
}

class JvmUdp(val socket: DatagramSocket) : UdpSocket {
    private val buf = ByteArray(65536)

    override fun send(host: String, port: Int, data: ByteArray): Boolean = runCatching {
        socket.send(DatagramPacket(data, data.size, InetAddress.getByName(host), port))
    }.isSuccess

    override fun receive(timeoutMs: Int): Datagram? {
        socket.soTimeout = timeoutMs.coerceAtLeast(1)
        val p = DatagramPacket(buf, buf.size)
        return try {
            socket.receive(p)
            Datagram(p.address.hostAddress, p.port, p.data.copyOfRange(p.offset, p.offset + p.length))
        } catch (_: SocketTimeoutException) {
            null
        } catch (_: Exception) {
            null
        }
    }

    override fun close() = socket.close()
}

/** Binds a UDP port to see whether a server could use it. */
class JvmPortProbe : PortProbe {
    override fun isFree(port: Int): Boolean = runCatching { DatagramSocket(port).close() }.isSuccess
}
