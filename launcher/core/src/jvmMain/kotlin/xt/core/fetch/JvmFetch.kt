// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.fetch

import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.net.HttpURLConnection
import java.net.URI
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.locks.ReentrantLock
import java.util.zip.DataFormatException
import java.util.zip.Inflater

/** The JVM platform: HttpURLConnection, RandomAccessFile, java.util.zip. The tests run on it. */
object JvmFetch {
    fun platform() = FetchPlatform(JvmHttp, JvmFs, JvmThreads) { JvmInflater() }
}

object JvmHttp : RangeHttp {
    override fun get(
        url: String,
        headers: Map<String, String>,
        cancel: CancelToken,
        head: (HttpHead) -> Unit,
        body: (ByteArray, Int, Int) -> Unit,
    ) {
        cancel.check()
        val c = URI(url).toURL().openConnection() as HttpURLConnection
        for ((k, v) in headers) c.setRequestProperty(k, v)
        c.connectTimeout = 20_000
        c.readTimeout = 30_000
        c.instanceFollowRedirects = true
        // A read blocked in a socket does not see a flag: closing the connection ends it.
        val unhook = cancel.register { c.disconnect() }
        try {
            val code = try {
                c.responseCode
            } catch (e: IOException) {
                cancel.check()
                throw FetchError("Network error: ${e.message}", e)
            }
            head(HttpHead(code, c.headerFields.filterKeys { it != null }.mapValues { it.value.lastOrNull().orEmpty() }))
            val buf = ByteArray(256 * 1024)
            try {
                (if (code >= 400) c.errorStream else c.inputStream)?.use { input ->
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        cancel.check()
                        if (n > 0) body(buf, 0, n)
                    }
                }
            } catch (e: IOException) {
                cancel.check()
                throw FetchError("Network error: ${e.message}", e)
            }
        } finally {
            unhook()
            c.disconnect()
        }
    }
}

object JvmFs : FileSystem {
    override fun list(dir: String): List<String> = File(dir).list()?.sorted() ?: emptyList()
    override fun isFile(path: String) = File(path).isFile
    override fun size(path: String) = File(path).let { if (it.isFile) it.length() else -1L }
    override fun delete(path: String) = File(path).let { !it.exists() || it.delete() }
    override fun rename(from: String, to: String) = try {
        Files.move(File(from).toPath(), File(to).toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        true
    } catch (e: IOException) {
        false
    }
    override fun mkdirs(dir: String) = File(dir).let { it.isDirectory || it.mkdirs() }
    override fun readBytes(path: String, limit: Int): ByteArray? = try {
        File(path).takeIf { it.isFile }?.inputStream()?.use { it.readNBytes(limit) }
    } catch (e: IOException) {
        null
    }

    override fun writeAtomic(path: String, bytes: ByteArray): Boolean = try {
        val tmp = File("$path.tmp")
        tmp.writeBytes(bytes)
        rename(tmp.path, path)
    } catch (e: IOException) {
        false
    }

    override fun open(path: String, truncate: Boolean): RandomFile {
        val raf = try {
            RandomAccessFile(path, "rw").also { if (truncate) it.setLength(0) }
        } catch (e: IOException) {
            throw Fatal("Cannot write $path: ${e.message}", e)
        }
        val ch: FileChannel = raf.channel
        return object : RandomFile {
            override fun write(at: Long, buf: ByteArray, off: Int, len: Int) {
                val bb = ByteBuffer.wrap(buf, off, len)
                var pos = at
                try {
                    while (bb.hasRemaining()) pos += ch.write(bb, pos)
                } catch (e: IOException) {
                    throw Fatal("Cannot write $path: ${e.message}", e)
                }
            }

            override fun read(at: Long, buf: ByteArray, off: Int, len: Int): Int =
                ch.read(ByteBuffer.wrap(buf, off, len), at).coerceAtLeast(0)

            override fun sync() = ch.force(false)
            override fun close() = raf.close()
        }
    }

    override fun freeBytes(dir: String): Long =
        generateSequence(File(dir).absoluteFile) { it.parentFile }.firstOrNull { it.exists() }?.usableSpace ?: Long.MAX_VALUE
}

object JvmThreads : Threads {
    override fun lock(): Lock = object : Lock {
        private val l = ReentrantLock()
        override fun <T> withLock(f: () -> T): T {
            l.lock()
            try { return f() } finally { l.unlock() }
        }
    }

    override fun start(name: String, body: () -> Unit): Joinable {
        val t = Thread(body, name).apply { isDaemon = true; start() }
        return object : Joinable { override fun join() = t.join() }
    }

    override fun sleep(ms: Long) = Thread.sleep(ms)
    override fun nowMs(): Long = System.nanoTime() / 1_000_000
}

class JvmInflater : RawInflater {
    private val inf = Inflater(true)
    override fun setInput(buf: ByteArray, off: Int, len: Int) = inf.setInput(buf, off, len)
    override fun inflate(out: ByteArray, off: Int, len: Int): Int = try {
        inf.inflate(out, off, len)
    } catch (e: DataFormatException) {
        throw FetchError("Damaged compressed data", e)
    }
    override val needsInput get() = inf.needsInput()
    override val finished get() = inf.finished()
    override fun end() = inf.end()
}
