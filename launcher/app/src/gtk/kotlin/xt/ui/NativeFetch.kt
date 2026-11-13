// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import kotlinx.cinterop.*
import platform.posix.*
import xt.core.fetch.*
import xtnative.CURLE_OK
import xtnative.CURLOPT_BUFFERSIZE
import xtnative.CURLOPT_CAINFO
import xtnative.CURLOPT_CONNECTTIMEOUT
import xtnative.CURLOPT_FOLLOWLOCATION
import xtnative.CURLOPT_HEADERDATA
import xtnative.CURLOPT_HEADERFUNCTION
import xtnative.CURLOPT_HTTPHEADER
import xtnative.CURLOPT_LOW_SPEED_LIMIT
import xtnative.CURLOPT_LOW_SPEED_TIME
import xtnative.CURLOPT_MAXREDIRS
import xtnative.CURLOPT_NOPROGRESS
import xtnative.CURLOPT_NOSIGNAL
import xtnative.CURLOPT_URL
import xtnative.CURLOPT_USERAGENT
import xtnative.CURLOPT_WRITEDATA
import xtnative.CURLOPT_WRITEFUNCTION
import xtnative.CURLOPT_XFERINFODATA
import xtnative.CURLOPT_XFERINFOFUNCTION
import xtnative.CURL_GLOBAL_DEFAULT
import xtnative.XT_Z_BUF_ERROR
import xtnative.XT_Z_OK
import xtnative.XT_Z_STREAM_END
import xtnative.curl_easy_cleanup
import xtnative.curl_easy_init
import xtnative.curl_easy_perform
import xtnative.curl_easy_strerror
import xtnative.curl_global_init
import xtnative.curl_off_t
import xtnative.curl_slist
import xtnative.curl_slist_append
import xtnative.curl_slist_free_all
import xtnative.xt_flock
import xtnative.xt_free_bytes
import xtnative.xt_inflate
import xtnative.xt_inflate_end
import xtnative.xt_inflate_init_raw
import xtnative.xt_response_code
import xtnative.xt_setopt_long
import xtnative.xt_setopt_ptr
import xtnative.xt_setopt_str
import platform.zlib.z_stream_s

/**
 * The pack downloader's platform in the GTK launcher: libcurl for HTTP(S),
 * POSIX pread/pwrite for the part files, zlib for inflate, pthreads. All of
 * it comes from the GNOME runtime the launcher runs on.
 */
object NativeFetch {
    private var initialized = false

    /** Call once on the main thread before any other thread uses curl. */
    fun platform(): FetchPlatform {
        if (!initialized) {
            curl_global_init(CURL_GLOBAL_DEFAULT.toLong())
            initialized = true
        }
        return FetchPlatform(CurlHttp, PosixFs, PosixThreads) { ZlibInflater() }
    }
}

// --- HTTP through libcurl ------------------------------------------------------

private class Transfer(
    val cancel: CancelToken,
    val head: (HttpHead) -> Unit,
    val body: (ByteArray, Int, Int) -> Unit,
) {
    var code = 0
    val headers = HashMap<String, String>()
    var delivered = false
    var error: Throwable? = null
    var buf = ByteArray(256 * 1024)

    fun deliverHead(): Boolean {
        if (delivered) return true
        delivered = true
        return try {
            head(HttpHead(code, headers))
            true
        } catch (t: Throwable) {
            error = t
            false
        }
    }
}

private val headerCb = staticCFunction { ptr: CPointer<ByteVar>?, size: size_t, n: size_t, ud: COpaquePointer? ->
    val len = (size * n).toInt()
    val t = ud!!.asStableRef<Transfer>().get()
    try {
        val line = ptr!!.readBytes(len).decodeToString().trimEnd('\r', '\n')
        when {
            line.startsWith("HTTP/") -> {
                // Each response in a redirect chain starts over.
                t.code = line.split(' ').getOrNull(1)?.toIntOrNull() ?: 0
                t.headers.clear()
            }
            line.isEmpty() -> {
                // End of one response's headers: the final one is not a redirect or a 1xx.
                if (t.code !in 100..199 && t.code !in 301..308 && !t.deliverHead()) return@staticCFunction 0.convert<size_t>()
            }
            else -> {
                val colon = line.indexOf(':')
                if (colon > 0) t.headers[line.substring(0, colon).trim()] = line.substring(colon + 1).trim()
            }
        }
        (size * n)
    } catch (e: Throwable) {
        t.error = e
        0.convert<size_t>()
    }
}

private val writeCb = staticCFunction { ptr: CPointer<ByteVar>?, size: size_t, n: size_t, ud: COpaquePointer? ->
    val len = (size * n).toInt()
    val t = ud!!.asStableRef<Transfer>().get()
    try {
        if (!t.deliverHead() || t.error != null) return@staticCFunction 0.convert<size_t>()
        if (t.buf.size < len) t.buf = ByteArray(len)
        t.buf.usePinned { memcpy(it.addressOf(0), ptr, len.convert()) }
        t.body(t.buf, 0, len)
        (size * n)
    } catch (e: Throwable) {
        t.error = e
        0.convert<size_t>()
    }
}

/** curl calls this about once a second even on a silent connection: the stop check. */
private val progressCb = staticCFunction { ud: COpaquePointer?, _: curl_off_t, _: curl_off_t, _: curl_off_t, _: curl_off_t ->
    if (ud!!.asStableRef<Transfer>().get().cancel.cancelled) 1 else 0
}

private object CurlHttp : RangeHttp {
    /** The CA bundle when the libcurl in use was built for another distribution's layout. */
    private val caFile: String? by lazy {
        listOfNotNull(
            getenv("SSL_CERT_FILE")?.toKString(),
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/cert.pem",
            "/etc/ssl/certs/ca-bundle.crt",
        ).firstOrNull { access(it, R_OK) == 0 }
    }

    override fun get(
        url: String,
        headers: Map<String, String>,
        cancel: CancelToken,
        head: (HttpHead) -> Unit,
        body: (ByteArray, Int, Int) -> Unit,
    ) {
        cancel.check()
        val h = curl_easy_init() ?: throw FetchError("Could not start a network request")
        val t = Transfer(cancel, head, body)
        val ref = StableRef.create(t)
        var list: CPointer<curl_slist>? = null
        try {
            for ((k, v) in headers) list = curl_slist_append(list, "$k: $v")
            list = curl_slist_append(list, "Expect:")
            xt_setopt_str(h, CURLOPT_URL, url)
            xt_setopt_ptr(h, CURLOPT_HTTPHEADER, list)
            xt_setopt_str(h, CURLOPT_USERAGENT, "Godnotic-launcher")
            xt_setopt_long(h, CURLOPT_FOLLOWLOCATION, 1)
            xt_setopt_long(h, CURLOPT_MAXREDIRS, 10)
            xt_setopt_long(h, CURLOPT_NOSIGNAL, 1)
            xt_setopt_long(h, CURLOPT_CONNECTTIMEOUT, 20)
            // Under 1 byte/s for 30 s is a dead connection: fail and retry.
            xt_setopt_long(h, CURLOPT_LOW_SPEED_LIMIT, 1)
            xt_setopt_long(h, CURLOPT_LOW_SPEED_TIME, 30)
            xt_setopt_long(h, CURLOPT_BUFFERSIZE, 256L * 1024)
            caFile?.let { xt_setopt_str(h, CURLOPT_CAINFO, it) }
            xt_setopt_ptr(h, CURLOPT_HEADERFUNCTION, headerCb)
            xt_setopt_ptr(h, CURLOPT_HEADERDATA, ref.asCPointer())
            xt_setopt_ptr(h, CURLOPT_WRITEFUNCTION, writeCb)
            xt_setopt_ptr(h, CURLOPT_WRITEDATA, ref.asCPointer())
            xt_setopt_long(h, CURLOPT_NOPROGRESS, 0)
            xt_setopt_ptr(h, CURLOPT_XFERINFOFUNCTION, progressCb)
            xt_setopt_ptr(h, CURLOPT_XFERINFODATA, ref.asCPointer())
            val rc = curl_easy_perform(h)
            t.error?.let { throw it }
            cancel.check()
            if (rc != CURLE_OK) throw FetchError("Network error: ${curl_easy_strerror(rc)?.toKString() ?: rc.toString()}")
            // A response with no body (a 501, a 404) never reached the write callback.
            if (t.code == 0) t.code = xt_response_code(h).toInt()
            if (!t.deliverHead()) throw t.error!!
        } finally {
            curl_slist_free_all(list)
            curl_easy_cleanup(h)
            ref.dispose()
        }
    }
}

// --- files ---------------------------------------------------------------------

private object PosixFs : FileSystem {
    override fun list(dir: String) = Sys.list(dir)

    override fun isFile(path: String): Boolean = memScoped {
        val st = alloc<stat>()
        stat(path, st.ptr) == 0 && (st.st_mode.toInt() and S_IFMT) == S_IFREG
    }

    override fun size(path: String): Long = if (isFile(path)) Sys.size(path) else -1L
    override fun delete(path: String) = unlink(path) == 0 || errno == ENOENT
    override fun rename(from: String, to: String) = platform.posix.rename(from, to) == 0

    override fun mkdirs(dir: String): Boolean {
        Sys.mkdirs(dir)
        return Sys.isDir(dir)
    }

    override fun readBytes(path: String, limit: Int): ByteArray? {
        val fd = open(path, O_RDONLY)
        if (fd < 0) return null
        try {
            val out = ByteArray(limit)
            var got = 0
            out.usePinned { pin ->
                while (got < limit) {
                    val n = read(fd, pin.addressOf(got), (limit - got).convert()).toInt()
                    if (n <= 0) break
                    got += n
                }
            }
            return out.copyOf(got)
        } finally {
            close(fd)
        }
    }

    override fun writeAtomic(path: String, bytes: ByteArray): Boolean {
        val tmp = "$path.tmp"
        val fd = open(tmp, O_WRONLY or O_CREAT or O_TRUNC, 0x1a4u) // 0644
        if (fd < 0) return false
        var ok = true
        if (bytes.isNotEmpty()) bytes.usePinned { pin ->
            var at = 0
            while (at < bytes.size) {
                val n = write(fd, pin.addressOf(at), (bytes.size - at).convert()).toInt()
                if (n <= 0) { ok = false; break }
                at += n
            }
        }
        ok = close(fd) == 0 && ok
        if (ok && platform.posix.rename(tmp, path) == 0) return true
        unlink(tmp)
        return false
    }

    override fun open(path: String, truncate: Boolean): RandomFile {
        val fd = open(path, O_RDWR or O_CREAT or (if (truncate) O_TRUNC else 0), 0x1a4u)
        if (fd < 0) throw Fatal("Cannot write $path: ${strerror(errno)?.toKString()}")
        return object : RandomFile {
            override fun write(at: Long, buf: ByteArray, off: Int, len: Int) {
                if (len == 0) return
                buf.usePinned { pin ->
                    var done = 0
                    while (done < len) {
                        val n = pwrite(fd, pin.addressOf(off + done), (len - done).convert(), at + done).toInt()
                        if (n <= 0) {
                            val why = if (errno == ENOSPC) "the disk is full" else strerror(errno)?.toKString()
                            throw Fatal("Cannot write $path: $why")
                        }
                        done += n
                    }
                }
            }

            override fun read(at: Long, buf: ByteArray, off: Int, len: Int): Int {
                if (len == 0) return 0
                return buf.usePinned { pin -> pread(fd, pin.addressOf(off), len.convert(), at).toInt().coerceAtLeast(0) }
            }

            override fun sync() { fsync(fd) }
            override fun close() { close(fd) }
        }
    }

    override fun freeBytes(dir: String): Long {
        var d = dir
        while (!Sys.isDir(d) && d.length > 1) d = xt.core.PathResolver.parent(d)
        val n = xt_free_bytes(d)
        return if (n < 0) Long.MAX_VALUE else n
    }
}

// --- threads -------------------------------------------------------------------

private class PosixLock : xt.core.fetch.Lock {
    private val m = nativeHeap.alloc<pthread_mutex_t>().also { pthread_mutex_init(it.ptr, null) }
    override fun <T> withLock(f: () -> T): T {
        pthread_mutex_lock(m.ptr)
        try { return f() } finally { pthread_mutex_unlock(m.ptr) }
    }
    // A handful per download, alive as long as it: not freed.
}

private val threadEntry = staticCFunction { d: COpaquePointer? ->
    val ref = d!!.asStableRef<() -> Unit>()
    val f = ref.get()
    ref.dispose()
    try { f() } catch (t: Throwable) { eprintln("thread failed: $t") }
    null as COpaquePointer?
}

private object PosixThreads : Threads {
    override fun lock(): xt.core.fetch.Lock = PosixLock()

    override fun start(name: String, body: () -> Unit): Joinable {
        val id = nativeHeap.alloc<pthread_tVar>()
        val rc = pthread_create(id.ptr, null, threadEntry, StableRef.create(body).asCPointer())
        if (rc != 0) throw Fatal("Could not start a download thread")
        return object : Joinable {
            override fun join() {
                pthread_join(id.value, null)
                nativeHeap.free(id)
            }
        }
    }

    override fun sleep(ms: Long) { usleep((ms * 1000).toUInt()) }
    override fun nowMs(): Long = xt.ui.nowMs().toLong()
}

// --- inflate -------------------------------------------------------------------

private class ZlibInflater : RawInflater {
    private val z = nativeHeap.alloc<z_stream_s>()
    private val input = nativeHeap.allocArray<ByteVar>(IN_SIZE)
    private var done = false
    private var ended = false

    init {
        memset(z.ptr, 0, sizeOf<z_stream_s>().convert())
        if (xt_inflate_init_raw(z.ptr) != XT_Z_OK) throw Fatal("zlib failed to start")
    }

    override fun setInput(buf: ByteArray, off: Int, len: Int) {
        val o = off
        var l = len
        // Larger than the native buffer: hand it over in pieces through inflate.
        if (l > IN_SIZE) l = IN_SIZE
        buf.usePinned { memcpy(input, it.addressOf(o), l.convert()) }
        z.next_in = input.reinterpret()
        z.avail_in = l.convert()
        pending = if (len > l) buf.copyOfRange(o + l, off + len) else null
    }

    private var pending: ByteArray? = null

    override fun inflate(out: ByteArray, off: Int, len: Int): Int {
        if (done) return 0
        if (z.avail_in == 0u) pending?.let { p -> pending = null; setInput(p, 0, p.size) }
        return out.usePinned { pin ->
            z.next_out = pin.addressOf(off).reinterpret()
            z.avail_out = len.convert()
            val rc = xt_inflate(z.ptr)
            val produced = len - z.avail_out.toInt()
            when (rc) {
                XT_Z_STREAM_END -> done = true
                XT_Z_OK, XT_Z_BUF_ERROR -> {}
                else -> throw FetchError("Damaged compressed data (zlib $rc)")
            }
            produced
        }
    }

    override val needsInput get() = z.avail_in == 0u && pending == null
    override val finished get() = done

    override fun end() {
        if (ended) return
        ended = true
        xt_inflate_end(z.ptr)
        nativeHeap.free(input)
        nativeHeap.free(z)
    }

    private companion object { const val IN_SIZE = 256 * 1024 }
}

// --- the shell downloader's lock ----------------------------------------------

/**
 * packaging/start.sh holds an exclusive flock on <data>/touch/fetch.lock while
 * its own downloader runs. Taking the same lock keeps the two from fetching
 * into one folder at once. Returns the fd to release, or -1 when it is held.
 */
object FetchLock {
    fun tryTake(dataDir: String): Int {
        Sys.mkdirs("$dataDir/touch")
        val fd = open("$dataDir/touch/fetch.lock", O_RDWR or O_CREAT, 0x1a4u)
        if (fd < 0) return -2 // cannot lock: go ahead rather than refuse
        if (xt_flock(fd, LOCK_EX or LOCK_NB) != 0) {
            close(fd)
            return -1
        }
        return fd
    }

    fun release(fd: Int) {
        if (fd >= 0) { xt_flock(fd, LOCK_UN); close(fd) }
    }
}
