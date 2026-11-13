// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalAtomicApi::class)

package xt.core.fetch

import kotlin.concurrent.atomics.AtomicBoolean
import kotlin.concurrent.atomics.AtomicInt
import kotlin.concurrent.atomics.AtomicIntArray
import kotlin.concurrent.atomics.AtomicLong
import kotlin.concurrent.atomics.ExperimentalAtomicApi
import kotlin.concurrent.atomics.decrementAndFetch

/**
 * The game's pk3s, taken straight out of the release zip over HTTP Range
 * requests: a port of the Android app's PackFetch.java, with the same files on
 * disk. The zip itself is never stored. Its central directory comes from its
 * last bytes, and each pk3 is fetched from its own byte range into
 * `<name>.part`, checked against the CRC32 the directory gives, then renamed
 * into place. Peak storage is the pk3s themselves.
 *
 * A stored entry (all the big pk3s) is cut into [FetchConfig.chunk] pieces that
 * [FetchConfig.connections] connections fetch at once, each writing at its own
 * offset. How far each chunk got is saved beside the part
 * (`<name>.part.state`, about once a second and on stop), so a stop, a dropped
 * connection or a killed process resumes from the bytes on disk. A deflated
 * entry (the small font packs) is inflated as one stream and starts over after
 * a break.
 *
 * [META] pins the archive (ETag and size) while a setup runs and lists what it
 * wants and what it finished. A new build on the server discards the parts and
 * the packs this setup finished rather than mixing two builds, and every range
 * request carries `If-Range`, so a build that changes mid-download is caught on
 * the next request ([ArchiveChanged]).
 */
class PackFetch(
    private val p: FetchPlatform,
    private val dir: String,
    private val plan: Plan,
    private val cfg: FetchConfig = FetchConfig(),
    private val listener: (FetchProgress) -> Unit = {},
) {
    private val fs get() = p.fs
    private val done = AtomicLong(0)
    private var total = 0L
    private val lastReport = AtomicLong(Long.MIN_VALUE / 2)

    // ------------------------------------------------------------------- run

    /**
     * Fetches the wanted packs; returns when all are installed. Cancel [token]
     * to stop: parts and chunk state stay for the next run, and this throws
     * [Cancelled].
     */
    fun run(token: CancelToken, music: Boolean = true) {
        fs.mkdirs(dir)
        var meta = Meta.read(fs, dir)
        if (plan.identity != meta.identity) {
            // A different build than the one the parts and finished packs came from.
            discard(meta)
            meta = Meta(plan.identity)
        }
        val todo = todo(fs, dir, plan, music)
        for (e in plan.entries) if (music || !e.isMusic) meta.want.add(e.role)
        if (todo.isEmpty()) {
            Meta.clear(fs, dir)
            return
        }
        val need = need(fs, dir, todo, plan, cfg)
        val free = fs.freeBytes(dir)
        if (free < need.disk + cfg.slack) throw NoSpace(need.disk + cfg.slack, free)
        // Packs finishing on several threads each rewrite the meta file: one at a time.
        meta.lock = p.threads.lock()
        if (!meta.write(fs, dir)) throw Fatal("Cannot write to $dir")

        total = need.fetch + need.resumed
        done.store(need.resumed)
        report(null, false)
        val stop = CancelToken(p.threads.lock())
        val unhook = token.register { stop.cancel() }
        val packs = ArrayList<Pack>()
        try {
            val jobs = ArrayList<() -> Unit>()
            for (e in todo) {
                val pack = Pack(e, meta, stop)
                packs += pack
                jobs += pack.jobs()
            }
            runJobs(jobs, packs, stop)
        } finally {
            unhook()
            for (pack in packs) pack.close()
        }
        if (token.cancelled) throw Cancelled()
        // Everything wanted is there now.
        Meta.clear(fs, dir)
        report(null, false)
    }

    private fun runJobs(jobs: List<() -> Unit>, packs: List<Pack>, stop: CancelToken) {
        val next = AtomicInt(0)
        val lock = p.threads.lock()
        var failure: Throwable? = null
        val finished = AtomicBoolean(false)
        val workers = (0 until minOf(cfg.connections, jobs.size)).map { n ->
            p.threads.start("xt-pack-$n") {
                while (!stop.cancelled) {
                    val i = next.fetchAndAdd(1)
                    if (i >= jobs.size) break
                    try {
                        jobs[i]()
                    } catch (t: Throwable) {
                        // The first real failure wins; the others are its echo (Cancelled).
                        lock.withLock { if (failure == null && t !is Cancelled) failure = t }
                        stop.cancel()
                        break
                    }
                }
            }
        }
        val saver = p.threads.start("xt-pack-state") {
            var last = p.threads.nowMs()
            while (!finished.load()) {
                p.threads.sleep(100)
                if (p.threads.nowMs() - last < cfg.saveEveryMs) continue
                last = p.threads.nowMs()
                for (pack in packs) pack.save()
                report(null, false, force = true)
            }
        }
        try {
            for (w in workers) w.join()
        } finally {
            finished.store(true)
            saver.join()
            for (pack in packs) pack.save()
        }
        lock.withLock { failure }?.let { throw it }
        if (stop.cancelled) throw Cancelled()
    }

    private fun report(file: String?, verifying: Boolean, force: Boolean = false) {
        listener(FetchProgress(done.load(), total, file, verifying))
        if (force) lastReport.store(done.load())
    }

    private fun advance(n: Int) {
        val now = done.addAndFetch(n.toLong())
        // One report per megabyte at most; the saver reports once a second too.
        val last = lastReport.load()
        if (now - last >= 1L shl 20 && lastReport.compareAndSet(last, now)) report(null, false)
    }

    /** Sleeps before attempt [attempt] + 1, waking early on a stop. */
    private fun pause(attempt: Int, stop: CancelToken) {
        val until = p.threads.nowMs() + cfg.backoffMs(attempt)
        while (p.threads.nowMs() < until) {
            stop.check()
            p.threads.sleep(minOf(100L, until - p.threads.nowMs()).coerceAtLeast(1))
        }
        stop.check()
    }

    /** One pk3 being fetched: its part file, chunk state and the jobs that fill it. */
    private inner class Pack(val e: Entry, val meta: Meta, val stop: CancelToken) {
        val part = partFile(dir, e)
        var state: ChunkState? = null
        var file: RandomFile? = null
        val left = AtomicInt(0)
        val lock = p.threads.lock()
        val headLock = p.threads.lock()

        init {
            when (e.method) {
                0 -> {
                    state = ChunkState.load(fs, stateFile(dir, e), part, e.csize, cfg.chunk, clean = true)
                    file = fs.open(part, truncate = false)
                }
                8 -> {}
                else -> throw Fatal("Unsupported compression in ${e.fileName}")
            }
        }

        fun jobs(): List<() -> Unit> {
            if (e.method == 8) {
                left.store(1)
                return listOf({ retrying { fetchDeflated() } })
            }
            val st = state!!
            val chunks = (0 until st.chunks).filter { st.pos(it) < st.sizeOf(it) }
            if (chunks.isEmpty()) {
                // Every byte arrived before the stop: only the check is left.
                left.store(1)
                return listOf({ verifyAndPublish() })
            }
            left.store(chunks.size)
            return chunks.map { i ->
                {
                    fetchChunk(i)
                    if (left.decrementAndFetch() == 0) verifyAndPublish()
                }
            }
        }

        /** Where the entry's bytes start: its local header is read once, by whichever chunk asks first. */
        fun dataStart(): Long = headLock.withLock {
            e.dataStart.load().takeIf { it >= 0 } ?: run {
                val lh = retrying { fetchRange(p, plan.url, plan.etag, e.offset, e.offset + 29, stop) }
                if (le32(lh, 0) != 0x04034b50L) throw Fatal("Bad local header for ${e.fileName}")
                val start = e.offset + 30 + le16(lh, 26) + le16(lh, 28)
                e.dataStart.store(start)
                start
            }
        }

        fun <T> retrying(job: () -> T): T {
            var attempt = 1
            while (true) {
                stop.check()
                try {
                    return job()
                } catch (t: Throwable) {
                    stop.check()
                    if (t is FetchError && !t.transient) throw t
                    if (attempt >= cfg.attempts) throw t
                    pause(attempt++, stop)
                }
            }
        }

        fun fetchChunk(i: Int) {
            val start = dataStart()
            val st = state!!
            val chunkStart = i * cfg.chunk
            val size = st.sizeOf(i)
            val f = file!!
            var failures = 0
            while (st.pos(i) < size) {
                stop.check()
                val from = chunkStart + st.pos(i)
                try {
                    p.http.get(plan.url, rangeHeaders(start + from, start + chunkStart + size - 1, plan.etag), stop,
                        { checkResponse(it, plan.etag) }) { buf, off, len ->
                        stop.check()
                        val room = (size - st.pos(i)).toInt()
                        val n = minOf(len, room)
                        if (n <= 0) return@get
                        f.write(chunkStart + st.pos(i), buf, off, n)
                        st.advance(i, n)
                        advance(n)
                        failures = 0
                    }
                    if (st.pos(i) < size) throw FetchError("Connection closed early")
                } catch (t: Throwable) {
                    stop.check()
                    if (t is FetchError && !t.transient) throw t
                    if (++failures >= cfg.attempts) throw t
                    pause(failures, stop)
                }
            }
        }

        fun fetchDeflated() {
            val start = dataStart()
            var got = 0L
            val crc = Crc32()
            val out = fs.open(part, truncate = true)
            var written = 0L
            val inflater = p.inflater()
            val obuf = ByteArray(128 * 1024)
            try {
                p.http.get(plan.url, rangeHeaders(start, start + e.csize - 1, plan.etag), stop,
                    { checkResponse(it, plan.etag) }) { buf, off, len ->
                    stop.check()
                    val n = minOf(len.toLong(), e.csize - got).toInt()
                    if (n <= 0) return@get
                    got += n
                    advance(n)
                    inflater.setInput(buf, off, n)
                    while (!inflater.finished) {
                        val m = inflater.inflate(obuf, 0, obuf.size)
                        if (m > 0) {
                            out.write(written, obuf, 0, m)
                            written += m
                            crc.update(obuf, 0, m)
                        } else if (inflater.needsInput) {
                            break
                        } else {
                            throw FetchError("Damaged download of ${e.fileName}")
                        }
                    }
                }
                if (!inflater.finished) throw FetchError("Connection closed early")
            } catch (t: Throwable) {
                // A deflated pack starts over: take back what it counted.
                done.addAndFetch(-got)
                throw t
            } finally {
                inflater.end()
                out.close()
            }
            if (written != e.usize || crc.value != e.crc) {
                fs.delete(part)
                done.addAndFetch(-got)
                throw FetchError("Damaged download of ${e.fileName}")
            }
            publish()
        }

        fun verifyAndPublish() {
            val st = state!!
            st.save(fs)
            file!!.sync()
            report(e.fileName, true)
            val crc = Crc32()
            val buf = ByteArray(256 * 1024)
            var at = 0L
            val f = file!!
            while (true) {
                stop.check()
                val n = f.read(at, buf, 0, buf.size)
                if (n <= 0) break
                crc.update(buf, 0, n)
                at += n
            }
            if (at != e.usize || crc.value != e.crc) {
                // Which chunk is bad is not known: the pack starts over.
                close()
                fs.delete(part)
                fs.delete(stateFile(dir, e))
                state = null
                done.addAndFetch(-e.csize)
                throw Damaged(e.fileName)
            }
            close()
            publish()
            fs.delete(stateFile(dir, e))
        }

        fun publish() {
            val dest = "$dir/${e.fileName}"
            if (!fs.rename(part, dest)) throw Fatal("Cannot move $part to $dest")
            meta.lock.withLock {
                meta.finished.add(e.fileName)
                meta.write(fs, dir)
            }
        }

        fun save() = lock.withLock {
            if (file != null) state?.save(fs)
        }

        fun close() = lock.withLock {
            if (file != null) {
                state?.save(fs)
                runCatching { file!!.close() }
                file = null
            }
        }
    }

    private fun checkResponse(h: HttpHead, etag: String?) {
        // If-Range did not match: the archive is a different build now.
        if (h.code == 200) throw ArchiveChanged()
        if (h.code != 206) throw HttpStatus(h.code, "HTTP ${h.code} from the download server")
        val got = h.header("ETag")
        if (etag != null && got != null && etag != got) throw ArchiveChanged()
    }

    /** Removes what a setup pinned to another build left: its parts and the packs it finished. */
    private fun discard(old: Meta) {
        for (n in fs.list(dir)) {
            if (n.endsWith(".pk3.part") || n.endsWith(".pk3.part.state")) fs.delete("$dir/$n")
        }
        if (old.identity == null) return
        val current = plan.entries.map { it.fileName + "\n" + it.usize }.toSet()
        for (name in old.finished) {
            val path = "$dir/$name"
            if (fs.isFile(path) && (name + "\n" + fs.size(path)) !in current) fs.delete(path)
        }
    }

    // ------------------------------------------------------------ the state

    /** [META]: the identity (two lines: ETag, size), then "want <role>" and "done <file>" lines. */
    class Meta(val identity: String?) {
        val want = LinkedHashSet<String>()
        val finished = LinkedHashSet<String>()
        internal var lock: Lock = NoLock

        fun write(fs: FileSystem, dir: String): Boolean {
            val text = StringBuilder(identity ?: "").append('\n')
            for (w in want) text.append("want ").append(w).append('\n')
            for (d in finished) text.append("done ").append(d).append('\n')
            return fs.writeAtomic("$dir/$META", text.toString().encodeToByteArray())
        }

        /** Roles a setup wants that are not installed yet. */
        fun missing(fs: FileSystem, dir: String) = want.filter { !installed(fs, dir, it) }

        companion object {
            fun read(fs: FileSystem, dir: String): Meta {
                val bytes = fs.readBytes("$dir/$META", 1 shl 20) ?: return Meta(null)
                val lines = bytes.decodeToString().split('\n')
                if (lines.size < 2) return Meta(null)
                val m = Meta(lines[0] + "\n" + lines[1])
                for (l in lines.drop(2)) when {
                    l.startsWith("want ") -> m.want.add(l.substring(5))
                    l.startsWith("done ") -> m.finished.add(l.substring(5))
                }
                return m
            }

            fun clear(fs: FileSystem, dir: String) {
                fs.delete("$dir/$META")
            }
        }
    }

    private object NoLock : Lock {
        override fun <T> withLock(f: () -> T): T = f()
    }

    /**
     * How many bytes of each fixed-size chunk of a stored entry are on disk,
     * saved beside the part (big-endian, as Java's DataOutputStream writes it,
     * so the Android app reads the same file): "XTPC", csize, count, then one
     * int per chunk. No state, or one that does not fit, starts over.
     */
    class ChunkState private constructor(
        private val file: String,
        private val csize: Long,
        private val chunk: Long,
        val chunks: Int,
    ) {
        private val done = AtomicIntArray(chunks)

        fun pos(i: Int): Long = done.loadAt(i).toLong()
        fun sizeOf(i: Int): Long = minOf(chunk, csize - i * chunk)
        fun advance(i: Int, n: Int) { done.addAndFetchAt(i, n) }
        fun total(): Long = (0 until chunks).sumOf { pos(it) }

        fun save(fs: FileSystem) {
            val b = ByteArray(16 + 4 * chunks)
            putInt(b, 0, MAGIC)
            putLong(b, 4, csize)
            putInt(b, 12, chunks)
            for (i in 0 until chunks) putInt(b, 16 + 4 * i, done.loadAt(i))
            fs.writeAtomic(file, b)
        }

        companion object {
            private const val MAGIC = 0x58545043 // "XTPC"

            /** @param clean delete a part and state that do not belong together (false: only look) */
            fun load(fs: FileSystem, file: String, part: String, csize: Long, chunk: Long, clean: Boolean): ChunkState {
                val count = maxOf(1L, (csize + chunk - 1) / chunk).toInt()
                if (fs.isFile(part)) {
                    val b = fs.readBytes(file, 16 + 4 * count + 1)
                    if (b != null && b.size == 16 + 4 * count && getInt(b, 0) == MAGIC &&
                        getLong(b, 4) == csize && getInt(b, 12) == count
                    ) {
                        val s = ChunkState(file, csize, chunk, count)
                        var ok = true
                        for (i in 0 until count) {
                            val v = getInt(b, 16 + 4 * i)
                            if (v < 0 || v > s.sizeOf(i)) { ok = false; break }
                            s.done.storeAt(i, v)
                        }
                        if (ok) return s
                    }
                }
                if (clean) {
                    fs.delete(part)
                    fs.delete(file)
                }
                return ChunkState(file, csize, chunk, count)
            }

            private fun putInt(b: ByteArray, o: Int, v: Int) {
                for (k in 0 until 4) b[o + k] = (v ushr (24 - 8 * k)).toByte()
            }
            private fun putLong(b: ByteArray, o: Int, v: Long) {
                for (k in 0 until 8) b[o + k] = (v ushr (56 - 8 * k)).toByte()
            }
            private fun getInt(b: ByteArray, o: Int): Int =
                (0 until 4).fold(0) { a, k -> (a shl 8) or (b[o + k].toInt() and 0xff) }
            private fun getLong(b: ByteArray, o: Int): Long =
                (0 until 8).fold(0L) { a, k -> (a shl 8) or (b[o + k].toLong() and 0xff) }
        }
    }

    companion object {
        const val META = ".packs-setup"

        /**
         * Reads the archive's central directory. [folder] is the zip folder
         * whose pk3s are wanted ("Xonotic/data/"); deeper entries are ignored.
         */
        fun plan(p: FetchPlatform, url: String, token: CancelToken, folder: String = "Xonotic/data/"): Plan {
            // The size first, then the tail by explicit offsets. A suffix range
            // ("bytes=-65557") would do both in one request, but GitHub's
            // release storage answers it with 501, and that is where the data lives.
            var length = -1L
            var etag: String? = null
            p.http.get(url, mapOf("Range" to "bytes=0-0", "Accept-Encoding" to "identity"), token, { h ->
                if (h.code != 206) {
                    if (h.code == 200) throw Fatal("The download server does not support partial downloads (HTTP 200)")
                    throw HttpStatus(h.code, if (h.code == 404) "The data archive is not on the server (HTTP 404): $url"
                    else "HTTP ${h.code} from the download server")
                }
                length = totalOf(h.header("Content-Range"))
                etag = h.header("ETag")
            }) { _, _, _ -> }
            if (length <= 0) throw Fatal("No size from $url")
            // End-of-central-directory record: 22 bytes plus a comment of at most 65535.
            val tailStart = maxOf(0L, length - 65557)
            val tail = fetchRange(p, url, etag, tailStart, length - 1, token)
            var eocd = -1
            for (i in tail.size - 22 downTo 0) {
                if (le32(tail, i) == 0x06054b50L) { eocd = i; break }
            }
            if (eocd < 0) throw Fatal("Not a zip file: $url")
            val cdSize = le32(tail, eocd + 12)
            val cdOffset = le32(tail, eocd + 16)
            if (cdSize == 0xFFFFFFFFL || cdOffset == 0xFFFFFFFFL || le16(tail, eocd + 10) == 0xFFFF) {
                throw Fatal("zip64 archives are not supported")
            }
            if (cdOffset + cdSize > length || cdSize > 64L shl 20) throw Fatal("Damaged zip directory in $url")
            val cd = if (cdOffset >= tailStart) {
                val from = (cdOffset - tailStart).toInt()
                tail.copyOfRange(from, from + cdSize.toInt())
            } else {
                fetchRange(p, url, etag, cdOffset, cdOffset + cdSize - 1, token)
            }
            val entries = ArrayList<Entry>()
            var q = 0
            while (q + 46 <= cd.size && le32(cd, q) == 0x02014b50L) {
                val n = le16(cd, q + 28)
                val x = le16(cd, q + 30)
                val k = le16(cd, q + 32)
                if (q + 46 + n > cd.size) break
                val name = cd.decodeToString(q + 46, q + 46 + n)
                if (name.startsWith(folder) && name.endsWith(".pk3")) {
                    val file = name.substring(folder.length)
                    // One level only, and never a name that could leave the folder.
                    if (file.isNotEmpty() && '/' !in file && '\\' !in file && !file.startsWith(".")) {
                        val csize = le32(cd, q + 20)
                        val usize = le32(cd, q + 24)
                        val off = le32(cd, q + 42)
                        if (csize == 0xFFFFFFFFL || usize == 0xFFFFFFFFL || off == 0xFFFFFFFFL) {
                            throw Fatal("zip64 archives are not supported")
                        }
                        entries += Entry(file, le16(cd, q + 10), le32(cd, q + 16), csize, usize, off)
                    }
                }
                q += 46 + n + x + k
            }
            if (entries.isEmpty()) throw Fatal("No pk3 in $url")
            return Plan(url, "$etag\n$length", etag, length, entries)
        }

        /** The wanted entries that no installed file plays yet. */
        fun todo(fs: FileSystem, dir: String, plan: Plan, music: Boolean = true): List<Entry> =
            plan.entries.filter { (music || !it.isMusic) && !installed(fs, dir, it.role) }

        /** What fetching [todo] costs in download and in storage, counting resumable parts. */
        fun need(fs: FileSystem, dir: String, todo: List<Entry>, plan: Plan, cfg: FetchConfig = FetchConfig()): Need {
            val same = plan.identity == Meta.read(fs, dir).identity
            var fetch = 0L
            var disk = 0L
            var resumed = 0L
            for (e in todo) {
                val r = if (same && e.method == 0)
                    ChunkState.load(fs, stateFile(dir, e), partFile(dir, e), e.csize, cfg.chunk, clean = false).total() else 0L
                fetch += e.csize - r
                disk += e.usize - r
                resumed += r
            }
            return Need(fetch, disk, resumed)
        }

        fun partFile(dir: String, e: Entry) = "$dir/${e.fileName}.part"
        fun stateFile(dir: String, e: Entry) = "$dir/${e.fileName}.part.state"

        private val DATE = Regex("\\d{8}(_\\d+)?")

        /** "xonotic-20261107-maps.pk3" and "xonotic-20261029_2-maps.pk3" -> "xonotic-*-maps.pk3". */
        fun role(fileName: String) = fileName.replace(DATE, "*")

        /** Whether a pk3 that plays [role] is in [dir], whatever its build date. */
        fun installed(fs: FileSystem, dir: String, role: String) =
            fs.list(dir).any { it.endsWith(".pk3") && role(it) == role }

        internal fun rangeHeaders(from: Long, to: Long, etag: String?): Map<String, String> = buildMap {
            put("Range", "bytes=$from-$to")
            put("Accept-Encoding", "identity")
            if (etag != null) put("If-Range", etag)
        }

        internal fun fetchRange(p: FetchPlatform, url: String, etag: String?, from: Long, to: Long, token: CancelToken): ByteArray {
            val want = (to - from + 1).toInt()
            val out = ByteArray(want)
            var got = 0
            p.http.get(url, rangeHeaders(from, to, etag), token, { h ->
                if (h.code == 200) throw ArchiveChanged()
                if (h.code != 206) throw HttpStatus(h.code, "HTTP ${h.code} for a range request")
            }) { buf, off, len ->
                if (got + len > want) throw FetchError("Response larger than expected")
                buf.copyInto(out, got, off, off + len)
                got += len
            }
            if (got != want) throw FetchError("Short range response")
            return out
        }

        internal fun totalOf(contentRange: String?): Long {
            val s = contentRange ?: return -1
            val slash = s.lastIndexOf('/')
            return if (slash < 0) -1 else s.substring(slash + 1).trim().toLongOrNull() ?: -1
        }

        internal fun le16(b: ByteArray, o: Int) = (b[o].toInt() and 0xff) or ((b[o + 1].toInt() and 0xff) shl 8)
        internal fun le32(b: ByteArray, o: Int): Long = (le16(b, o).toLong() or (le16(b, o + 2).toLong() shl 16)) and 0xFFFFFFFFL
    }
}

/** Tunables; the defaults are the Android app's. Tests shrink the chunk. */
class FetchConfig(
    /** Parallel connections: 8 keep a fast link full; on a slow one they cost nothing over 4. */
    val connections: Int = 8,
    val chunk: Long = 8L shl 20,
    /** Room for the filesystem, later updates and the engine's own files. */
    val slack: Long = 64L shl 20,
    /** Consecutive failures on one chunk before the run gives up (about two minutes of no network). */
    val attempts: Int = 10,
    val backoffMs: (Int) -> Long = { minOf(15_000L, 2000L * it) },
    val saveEveryMs: Long = 1000,
)

/** One pk3 in the archive's central directory. */
@OptIn(ExperimentalAtomicApi::class)
class Entry(
    val fileName: String,
    val method: Int,
    val crc: Long,
    val csize: Long,
    val usize: Long,
    val offset: Long,
) {
    /** Where the data starts (after the local header); -1 until asked. */
    internal val dataStart = AtomicLong(-1)
    val isMusic get() = "-music" in fileName
    /** The name with its build date as a wildcard: which installed file plays this part. */
    val role get() = PackFetch.role(fileName)
}

/** The archive as a plan: every pk3 under its data/ folder, and its identity. */
class Plan(val url: String, val identity: String, val etag: String?, val size: Long, val entries: List<Entry>)

/** The space a run needs and what it would fetch. */
data class Need(val fetch: Long, val disk: Long, val resumed: Long)

data class FetchProgress(val done: Long, val total: Long, val file: String?, val verifying: Boolean)
