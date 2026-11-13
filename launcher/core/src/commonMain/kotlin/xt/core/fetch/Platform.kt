// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalAtomicApi::class)

package xt.core.fetch

import kotlin.concurrent.atomics.AtomicBoolean
import kotlin.concurrent.atomics.ExperimentalAtomicApi

/**
 * What [PackFetch] needs from the platform, kept small so the algorithm stays
 * common Kotlin: one HTTP GET with headers, a few file calls, raw inflate and
 * threads. The JVM implementation (jvmMain) runs the tests; the GTK launcher
 * has a libcurl / POSIX / zlib / pthreads one.
 */
class FetchPlatform(
    val http: RangeHttp,
    val fs: FileSystem,
    val threads: Threads,
    val inflater: () -> RawInflater,
)

/** The final response's status and headers (names lower-case), after redirects. */
class HttpHead(val code: Int, headers: Map<String, String>) {
    private val h = headers.mapKeys { it.key.lowercase() }
    fun header(name: String): String? = h[name.lowercase()]
}

interface RangeHttp {
    /**
     * One GET. [head] runs once, before any body, with the final response
     * (redirects followed); [body] then gets the body in pieces, the array
     * valid only during the call. Either may throw to abort the request, and
     * that exception comes out of [get]. Network failures throw as well.
     * After [cancel] fires, [get] must return (throwing) within about a second
     * even when blocked on a silent connection.
     */
    fun get(
        url: String,
        headers: Map<String, String>,
        cancel: CancelToken,
        head: (HttpHead) -> Unit,
        body: (ByteArray, Int, Int) -> Unit,
    )
}

interface RandomFile {
    fun write(at: Long, buf: ByteArray, off: Int, len: Int)
    /** Bytes read, 0 at the end. */
    fun read(at: Long, buf: ByteArray, off: Int, len: Int): Int
    fun sync()
    fun close()
}

interface FileSystem {
    fun list(dir: String): List<String>
    fun isFile(path: String): Boolean
    /** -1 when there is no such file. */
    fun size(path: String): Long
    fun delete(path: String): Boolean
    /** Replaces [to] if it exists. */
    fun rename(from: String, to: String): Boolean
    fun mkdirs(dir: String): Boolean
    fun readBytes(path: String, limit: Int): ByteArray?
    /** Through a temporary file and a rename. */
    fun writeAtomic(path: String, bytes: ByteArray): Boolean
    /** Opens for reading and writing, creating it; [truncate] empties it first. */
    fun open(path: String, truncate: Boolean): RandomFile
    /** Bytes an unprivileged process may still write there; Long.MAX_VALUE when unknown. */
    fun freeBytes(dir: String): Long
}

/** zlib inflate of raw deflate data (no header), as a zip stores it. */
interface RawInflater {
    fun setInput(buf: ByteArray, off: Int, len: Int)
    /** Bytes produced; throws [FetchError] on damaged data. */
    fun inflate(out: ByteArray, off: Int, len: Int): Int
    val needsInput: Boolean
    val finished: Boolean
    fun end()
}

interface Lock {
    fun <T> withLock(f: () -> T): T
}

interface Joinable {
    fun join()
}

interface Threads {
    fun lock(): Lock
    fun start(name: String, body: () -> Unit): Joinable
    fun sleep(ms: Long)
    fun nowMs(): Long
}

/**
 * Stops a download. Hooks registered by the HTTP layer close what is
 * blocked; everything else polls [cancelled].
 */
class CancelToken(private val lock: Lock) {
    private val flag = AtomicBoolean(false)
    private val hooks = LinkedHashSet<() -> Unit>()

    val cancelled: Boolean get() = flag.load()

    fun cancel() {
        if (!flag.compareAndSet(false, true)) return
        val now = lock.withLock { hooks.toList() }
        for (h in now) runCatching(h)
    }

    /** Runs [hook] on cancel (at once if already cancelled); the result unregisters it. */
    fun register(hook: () -> Unit): () -> Unit {
        val late = lock.withLock {
            if (flag.load()) true else { hooks.add(hook); false }
        }
        if (late) runCatching(hook)
        return { lock.withLock { hooks.remove(hook) } }
    }

    fun check() {
        if (cancelled) throw Cancelled()
    }
}

/** Every failure the fetcher reports, in words a player can read. */
open class FetchError(message: String, cause: Throwable? = null) : Exception(message, cause) {
    /** Worth another attempt within the same run (network trouble). */
    open val transient: Boolean get() = true
}

class Cancelled : FetchError("Download stopped") {
    override val transient get() = false
}

/** The archive changed on the server while this run was using it. */
class ArchiveChanged : FetchError("The download server published a new build. Retry starts over with it.") {
    override val transient get() = false
}

class NoSpace(val needed: Long, val available: Long) :
    FetchError("Not enough free storage: needs ${needed shr 20} MB, ${available shr 20} MB free") {
    override val transient get() = false
}

class HttpStatus(val code: Int, message: String) : FetchError(message) {
    override val transient get() = code >= 500 || code == 408 || code == 429
}

/** A pack whose bytes do not match the archive's CRC32; the next run fetches it again. */
class Damaged(val file: String) : FetchError("Damaged download of $file; Retry fetches it again") {
    override val transient get() = false
}

/** Something no retry fixes: not a zip, zip64, a server without ranges, a write failure. */
class Fatal(message: String, cause: Throwable? = null) : FetchError(message, cause) {
    override val transient get() = false
}

/** Plain-language text for any exception a run can end with. */
fun describe(t: Throwable): String = when (t) {
    is FetchError -> t.message ?: "Download failed"
    else -> "Download failed: ${t.message ?: t::class.simpleName}"
}
