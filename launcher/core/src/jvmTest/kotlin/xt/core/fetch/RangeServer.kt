// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.fetch

import com.sun.net.httpserver.HttpExchange
import com.sun.net.httpserver.HttpServer
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.InetSocketAddress
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicLong
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlin.random.Random

/**
 * A release-storage stand-in: serves one zip with Range, If-Range and ETag
 * the way GitHub's storage does, including 501 for suffix ranges. Knobs let a
 * test slow it down, corrupt a byte, fail requests, or publish a new build.
 */
class RangeServer(archive: ByteArray, etag: String = "\"build-a\"") : AutoCloseable {
    @Volatile var archive: ByteArray = archive
    @Volatile var etag: String = etag

    /** Body bytes sent, all responses. */
    val served = AtomicLong()
    val requests = CopyOnWriteArrayList<Pair<String, String?>>()

    /** Milliseconds to sleep after each 16 KB written. */
    @Volatile var delayPer16k = 0L
    /** An absolute archive offset whose byte is flipped the first time a response covers it. */
    @Volatile var corruptAt = -1L
    /** Answer this many coming range requests (not the size probe) with 503. */
    val fail503 = AtomicInteger(0)
    /** After this many body bytes, switch to [next] (a new build with a new ETag). */
    @Volatile var swapAfter = -1L
    @Volatile var next: Pair<ByteArray, String>? = null

    private val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
    val url: String

    init {
        server.executor = Executors.newCachedThreadPool { r -> Thread(r, "range-server").apply { isDaemon = true } }
        server.createContext("/") { ex -> runCatching { handle(ex) }; ex.close() }
        server.start()
        url = "http://127.0.0.1:${server.address.port}/Godnotic-data.zip"
    }

    private fun handle(ex: HttpExchange) {
        maybeSwap()
        if (ex.requestURI.path != "/Godnotic-data.zip") {
            ex.sendResponseHeaders(404, -1)
            return
        }
        val data = archive
        val tag = etag
        val range = ex.requestHeaders.getFirst("Range")
        val ifRange = ex.requestHeaders.getFirst("If-Range")
        requests += (range ?: "") to ifRange
        ex.responseHeaders.add("ETag", tag)
        ex.responseHeaders.add("Accept-Ranges", "bytes")
        if (range != null && range.startsWith("bytes=-")) {
            // GitHub's release storage refuses suffix ranges.
            ex.sendResponseHeaders(501, -1)
            return
        }
        val m = range?.let { Regex("bytes=(\\d+)-(\\d+)").matchEntire(it) }
        if (m == null || (ifRange != null && ifRange != tag)) {
            // No range, or If-Range for another build: the whole new file.
            send(ex, 200, data, 0, data.size.toLong())
            return
        }
        if (range != "bytes=0-0" && fail503.getAndUpdate { if (it > 0) it - 1 else 0 } > 0) {
            ex.sendResponseHeaders(503, -1)
            return
        }
        val from = m.groupValues[1].toLong()
        val to = minOf(m.groupValues[2].toLong(), data.size - 1L)
        if (from > to) {
            ex.sendResponseHeaders(416, -1)
            return
        }
        ex.responseHeaders.add("Content-Range", "bytes $from-$to/${data.size}")
        send(ex, 206, data, from, to - from + 1)
    }

    private fun send(ex: HttpExchange, code: Int, data: ByteArray, from: Long, len: Long) {
        ex.sendResponseHeaders(code, len)
        val out = ex.responseBody
        var at = from
        val end = from + len
        try {
            while (at < end) {
                val n = minOf(16 * 1024L, end - at).toInt()
                val piece = data.copyOfRange(at.toInt(), at.toInt() + n)
                val c = corruptAt
                if (c in at until at + n) {
                    piece[(c - at).toInt()] = (piece[(c - at).toInt()].toInt() xor 0x5a).toByte()
                    corruptAt = -1
                }
                out.write(piece)
                served.addAndGet(n.toLong())
                at += n
                if (delayPer16k > 0) Thread.sleep(delayPer16k)
                if (swapAfter in 0..served.get()) { maybeSwap(); if (archive !== data) break }
            }
            out.close()
        } catch (e: IOException) {
            // The client went away (a stop, or it saw a new build).
        }
    }

    private fun maybeSwap() {
        val s = swapAfter
        val n = next
        if (s >= 0 && n != null && served.get() >= s) synchronized(this) {
            if (next != null) {
                archive = n.first
                etag = n.second
                next = null
                swapAfter = -1
            }
        }
    }

    override fun close() = server.stop(0)

    companion object {
        class Pack(val name: String, val bytes: ByteArray, val deflate: Boolean)

        fun randomBytes(size: Int, seed: Int) = Random(seed).nextBytes(size)

        fun textBytes(size: Int, seed: Int): ByteArray {
            val r = Random(seed)
            val words = listOf("rocket ", "arena ", "frag ", "touch ", "godnotic ", "map ", "\n")
            val sb = StringBuilder()
            while (sb.length < size) sb.append(words[r.nextInt(words.size)])
            return sb.substring(0, size).encodeToByteArray()
        }

        /** A release archive: Xonotic/data/<pack> entries plus things the fetcher must skip. */
        fun zip(packs: List<Pack>): ByteArray {
            val bos = ByteArrayOutputStream()
            ZipOutputStream(bos).use { z ->
                z.putNextEntry(ZipEntry("Xonotic/README.txt"))
                z.write("hello".encodeToByteArray())
                for (p in packs) {
                    val e = ZipEntry("Xonotic/data/${p.name}")
                    if (!p.deflate) {
                        e.method = ZipEntry.STORED
                        e.size = p.bytes.size.toLong()
                        e.compressedSize = p.bytes.size.toLong()
                        e.crc = CRC32().apply { update(p.bytes) }.value
                    }
                    z.putNextEntry(e)
                    z.write(p.bytes)
                }
                for (bad in listOf("Xonotic/data/sub/deeper.pk3", "Xonotic/data/.hidden.pk3", "other/data/x.pk3")) {
                    z.putNextEntry(ZipEntry(bad))
                    z.write(1)
                }
            }
            return bos.toByteArray()
        }
    }
}
