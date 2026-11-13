// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.fetch

import java.io.File
import java.nio.file.Files
import java.util.zip.CRC32
import kotlin.concurrent.thread
import kotlin.random.Random
import kotlin.test.AfterTest
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

class PackFetchTest {
    private val p = JvmFetch.platform()
    /** Small chunks so a few hundred KB make many of them; quick back-off. */
    private val cfg = FetchConfig(connections = 4, chunk = 64 * 1024, slack = 0, attempts = 5, backoffMs = { 20L }, saveEveryMs = 200)
    private val dir: File = Files.createTempDirectory("packfetch").toFile()
    private val closing = mutableListOf<AutoCloseable>()

    private val packsA = listOf(
        RangeServer.Companion.Pack("xonotic-20261107-data.pk3", RangeServer.randomBytes(700_000, 1), false),
        RangeServer.Companion.Pack("xonotic-20261107-maps.pk3", RangeServer.randomBytes(900_000, 2), false),
        RangeServer.Companion.Pack("xonotic-20261107-music.pk3", RangeServer.randomBytes(300_000, 3), false),
        RangeServer.Companion.Pack("font-unifont-20261107.pk3", RangeServer.textBytes(250_000, 4), true),
        RangeServer.Companion.Pack("xonotic-20261107-xoncompat.pk3", RangeServer.randomBytes(1000, 5), false),
    )
    private val storedTotal = packsA.filter { !it.deflate }.sumOf { it.bytes.size.toLong() }

    @AfterTest fun cleanup() {
        closing.forEach { runCatching { it.close() } }
        dir.deleteRecursively()
    }

    private fun server(packs: List<RangeServer.Companion.Pack> = packsA) =
        RangeServer(RangeServer.zip(packs)).also { closing += it }

    private fun token() = CancelToken(p.threads.lock())

    private fun crc(b: ByteArray) = CRC32().apply { update(b) }.value

    private fun assertInstalled(packs: List<RangeServer.Companion.Pack>) {
        for (pk in packs) {
            val f = File(dir, pk.name)
            assertTrue(f.isFile, "${pk.name} installed")
            assertEquals(crc(pk.bytes), crc(f.readBytes()), "${pk.name} content")
        }
        val left = dir.list()!!.filter { it.endsWith(".part") || it.endsWith(".part.state") }
        assertEquals(emptyList(), left, "no parts left")
        assertFalse(File(dir, PackFetch.META).exists(), "setup meta cleared")
    }

    /** Runs a fetch on a thread and stops it once [stopAt] bytes are done. */
    private fun runAndStop(plan: Plan, stopAt: Long): Throwable? {
        val t = token()
        var err: Throwable? = null
        val th = thread {
            err = runCatching { PackFetch(p, dir.path, plan, cfg) { if (it.done >= stopAt) t.cancel() }.run(t) }.exceptionOrNull()
        }
        th.join(30_000)
        assertFalse(th.isAlive, "stopped in time")
        return err
    }

    @Test fun planReadsOnlyDataPacks() {
        val s = server()
        val plan = PackFetch.plan(p, s.url, token())
        assertEquals(packsA.map { it.name }.sorted(), plan.entries.map { it.fileName }.sorted())
        assertEquals("\"build-a\"", plan.etag)
        assertEquals(8, plan.entries.first { it.fileName.startsWith("font") }.method)
        // Size probe first, never a suffix range (GitHub answers those with 501).
        assertEquals("bytes=0-0", s.requests.first().first)
        assertTrue(s.requests.none { it.first.startsWith("bytes=-") })
        // Every request after the probe pins the build.
        assertTrue(s.requests.drop(1).all { it.second == "\"build-a\"" })
    }

    @Test fun serverRefusesSuffixRangesLikeGitHub() {
        val s = server()
        var code = 0
        p.http.get(s.url, mapOf("Range" to "bytes=-100"), token(), { code = it.code }) { _, _, _ -> }
        assertEquals(501, code)
    }

    @Test fun fullDownload() {
        val s = server()
        val plan = PackFetch.plan(p, s.url, token())
        var last: FetchProgress? = null
        PackFetch(p, dir.path, plan, cfg) { last = it }.run(token())
        assertInstalled(packsA)
        assertTrue(s.requests.drop(1).all { it.second == "\"build-a\"" }, "If-Range on every range request")
        val end = assertNotNull(last)
        assertEquals(end.total, end.done)
        DataSets.markInstalled(p.fs, dir.path, plan)
        assertTrue(File(dir, ".assets-ready").isFile)
        assertTrue(File(dir, "touch/music-ready.txt").isFile)
        // A second run has nothing to do and fetches no pack bytes.
        val before = s.served.get()
        PackFetch(p, dir.path, plan, cfg).run(token())
        assertEquals(before, s.served.get())
    }

    @Test fun withoutMusic() {
        val s = server()
        val plan = PackFetch.plan(p, s.url, token())
        PackFetch(p, dir.path, plan, cfg).run(token(), music = false)
        assertFalse(File(dir, "xonotic-20261107-music.pk3").exists())
        assertTrue(File(dir, "xonotic-20261107-maps.pk3").exists())
    }

    @Test fun resumesAfterStopWithoutRefetching() {
        val s = server()
        s.delayPer16k = 3
        val plan = PackFetch.plan(p, s.url, token())
        val err = runAndStop(plan, stopAt = storedTotal / 2)
        assertTrue(err is Cancelled, "stop reports Cancelled, got $err")
        assertTrue(dir.list()!!.any { it.endsWith(".part.state") }, "chunk state saved on stop")
        val need = PackFetch.need(p.fs, dir.path, PackFetch.todo(p.fs, dir.path, plan), plan, cfg)
        assertTrue(need.resumed > 0, "resumable bytes counted: ${need.resumed}")
        // Bytes in parts plus packs already installed: well under the whole set is left.
        assertTrue(need.fetch < storedTotal * 3 / 4, "left to fetch: ${need.fetch} of $storedTotal")

        s.delayPer16k = 0
        s.served.set(0)
        val plan2 = PackFetch.plan(p, s.url, token())
        val planBytes = s.served.get()
        PackFetch(p, dir.path, plan2, cfg).run(token())
        assertInstalled(packsA)
        val fetched = s.served.get() - planBytes
        val deflated = packsA.filter { it.deflate }.sumOf { it.bytes.size.toLong() } // upper bound of its compressed size
        // Only the missing bytes, the local headers, and maybe the font pack again.
        assertTrue(fetched <= need.fetch + deflated + 30L * packsA.size,
            "second run fetched $fetched, should be at most ${need.fetch} + slack")
    }

    @Test fun resumesAfterKillWithStaleState() {
        val s = server()
        s.delayPer16k = 3
        val plan = PackFetch.plan(p, s.url, token())
        runAndStop(plan, stopAt = storedTotal / 2)
        // A kill loses up to a second of progress: the state says less than the part holds.
        var claimed = 0L
        for (name in dir.list()!!.filter { it.endsWith(".part.state") }) {
            val f = File(dir, name)
            val b = f.readBytes()
            val count = java.nio.ByteBuffer.wrap(b, 12, 4).int
            val bb = java.nio.ByteBuffer.wrap(b)
            for (i in 0 until count) {
                val v = bb.getInt(16 + 4 * i) / 2
                bb.putInt(16 + 4 * i, v)
                claimed += v
            }
            f.writeBytes(b)
        }
        s.delayPer16k = 0
        val beforeRun = s.served.get()
        val plan2 = PackFetch.plan(p, s.url, token())
        PackFetch(p, dir.path, plan2, cfg).run(token())
        assertInstalled(packsA)
        assertTrue(claimed > 0)
        assertTrue(s.served.get() - beforeRun < plan2.size - claimed + 70_000, "kept the bytes the state vouched for")
    }

    @Test fun changedArchiveIsDetectedAndOldPartsDiscarded() {
        // Build B: same names, the maps pack changed (other size and bytes).
        val packsB = packsA.map {
            if (it.name.contains("maps")) RangeServer.Companion.Pack(it.name, RangeServer.randomBytes(950_000, 22), false) else it
        }
        val s = server()
        s.delayPer16k = 2
        val plan = PackFetch.plan(p, s.url, token())
        s.next = RangeServer.zip(packsB) to "\"build-b\""
        s.swapAfter = s.served.get() + storedTotal / 3
        val err = assertFailsWith<ArchiveChanged> { PackFetch(p, dir.path, plan, cfg).run(token()) }
        assertNotNull(err.message)
        assertEquals("\"build-b\"", s.etag)

        // Running the old plan again keeps failing: the build is pinned.
        assertFailsWith<ArchiveChanged> { PackFetch(p, dir.path, plan, cfg).run(token()) }

        s.delayPer16k = 0
        val plan2 = PackFetch.plan(p, s.url, token())
        assertEquals("\"build-b\"", plan2.etag)
        // The meta still pins build A until the next run discards its leftovers.
        assertTrue(File(dir, PackFetch.META).readText().startsWith("\"build-a\""))
        val need = PackFetch.need(p.fs, dir.path, PackFetch.todo(p.fs, dir.path, plan2), plan2, cfg)
        assertEquals(0, need.resumed, "parts of build A are not counted for build B")
        PackFetch(p, dir.path, plan2, cfg).run(token())
        assertInstalled(packsB)
    }

    @Test fun finishedPackOfOldBuildIsReplaced() {
        val s = server()
        val plan = PackFetch.plan(p, s.url, token())
        // Build A's maps pack finished, then the run stopped.
        PackFetch(p, dir.path, plan, cfg).run(token())
        val meta = PackFetch.Meta(plan.identity).apply { finished += "xonotic-20261107-maps.pk3"; want += "xonotic-*-data.pk3" }
        File(dir, "xonotic-20261107-data.pk3").delete()
        assertTrue(meta.write(p.fs, dir.path))
        val packsB = packsA.map {
            if (it.name.contains("maps")) RangeServer.Companion.Pack(it.name, RangeServer.randomBytes(950_000, 22), false) else it
        }
        s.archive = RangeServer.zip(packsB)
        s.etag = "\"build-b\""
        val plan2 = PackFetch.plan(p, s.url, token())
        PackFetch(p, dir.path, plan2, cfg).run(token())
        assertInstalled(packsB)
    }

    @Test fun crcFailureRemovesThePartAndRetryFetchesAgain() {
        val pk = packsA[0]
        val archive = RangeServer.zip(packsA)
        val s = RangeServer(archive).also { closing += it }
        val plan = PackFetch.plan(p, s.url, token())
        val entry = plan.entries.first { it.fileName == pk.name }
        // A byte in the middle of the data pack's bytes (its local header is 30 + name bytes).
        s.corruptAt = entry.offset + 30 + "Xonotic/data/${pk.name}".length + 1234
        val err = assertFailsWith<Damaged> { PackFetch(p, dir.path, plan, cfg).run(token()) }
        assertEquals(pk.name, err.file)
        assertFalse(File(dir, pk.name + ".part").exists(), "damaged part removed")
        assertFalse(File(dir, pk.name + ".part.state").exists(), "its state too")
        assertFalse(File(dir, pk.name).exists())

        val before = s.served.get()
        PackFetch(p, dir.path, PackFetch.plan(p, s.url, token()), cfg).run(token())
        assertInstalled(packsA)
        assertTrue(s.served.get() - before >= pk.bytes.size, "the damaged pack was fetched again in full")
    }

    @Test fun transientServerErrorsAreRetried() {
        val s = server()
        val plan = PackFetch.plan(p, s.url, token())
        s.fail503.set(3)
        PackFetch(p, dir.path, plan, cfg).run(token())
        assertInstalled(packsA)
        assertEquals(0, s.fail503.get())
    }

    @Test fun missingArchiveFailsAtOnceAndAutomaticSlimFallsBackToFull() {
        val s = server()
        val e = assertFailsWith<HttpStatus> { PackFetch.plan(p, s.url + ".missing", token()) }
        assertEquals(404, e.code)
        assertFalse(e.transient)
        val slim = DataSets.Choice(DataSets.Kind.SLIM, s.url + ".missing", "test", automatic = true)
        val (used, plan) = DataSets.plan(p, slim, token(), fullUrl = s.url)
        assertEquals(DataSets.Kind.FULL, used.kind)
        assertEquals(packsA.size, plan.entries.size)
        // Chosen by hand, a missing slim set is an error, not a silent switch.
        assertFailsWith<HttpStatus> { DataSets.plan(p, slim.copy(automatic = false), token(), fullUrl = s.url) }
    }

    @Test fun noSpaceStopsBeforeFetching() {
        val s = server()
        val plan = PackFetch.plan(p, s.url, token())
        val full = object : FileSystem by p.fs {
            override fun freeBytes(dir: String) = 100_000L
        }
        val tight = FetchPlatform(p.http, full, p.threads, p.inflater)
        val before = s.served.get()
        val e = assertFailsWith<NoSpace> { PackFetch(tight, dir.path, plan, cfg).run(token()) }
        assertEquals(100_000L, e.available)
        assertTrue(e.needed >= packsA.sumOf { it.bytes.size.toLong() })
        assertEquals(before, s.served.get(), "nothing fetched")
        assertTrue(dir.list()!!.none { it.endsWith(".pk3") || it.endsWith(".part") })
    }

    @Test fun rolesMatchAnyBuildDate() {
        assertEquals("xonotic-*-maps.pk3", PackFetch.role("xonotic-20261107-maps.pk3"))
        assertEquals("xonotic-*-maps.pk3", PackFetch.role("xonotic-20261029_2-maps.pk3"))
        File(dir, "xonotic-20250101-maps.pk3").writeText("x")
        val s = server()
        val plan = PackFetch.plan(p, s.url, token())
        val todo = PackFetch.todo(p.fs, dir.path, plan).map { it.fileName }
        assertFalse("xonotic-20261107-maps.pk3" in todo, "an older maps pack plays the same role")
    }

    @Test fun chunkStateFileIsTheAndroidFormat() {
        val st = PackFetch.ChunkState.load(p.fs, "${dir.path}/a.part.state", "${dir.path}/a.part", 200_000, 65536, true)
        st.advance(1, 77)
        st.save(p.fs)
        val b = File(dir, "a.part.state").readBytes()
        val bb = java.nio.ByteBuffer.wrap(b) // big-endian, like DataOutputStream
        assertEquals(0x58545043, bb.int)
        assertEquals(200_000L, bb.long)
        assertEquals(4, bb.int)
        assertEquals(listOf(0, 77, 0, 0), List(4) { bb.int })
        File(dir, "a.part").writeText("")
        assertEquals(77, PackFetch.ChunkState.load(p.fs, "${dir.path}/a.part.state", "${dir.path}/a.part", 200_000, 65536, false).total())
    }

    @Test fun crc32MatchesJava() {
        val r = Random(9)
        for (n in listOf(0, 1, 3, 4, 5, 1000, 65537)) {
            val b = r.nextBytes(n + 7)
            val mine = Crc32().apply { update(b, 3, n) }.value
            val java = CRC32().apply { update(b, 3, n) }.value
            assertEquals(java, mine, "length $n")
        }
        val split = Crc32().apply { val b = "hello world".encodeToByteArray(); update(b, 0, 5); update(b, 5, 6) }.value
        assertEquals(CRC32().apply { update("hello world".encodeToByteArray()) }.value, split)
    }

    @Test fun cancelTokenRunsLateHooks() {
        val t = token()
        var n = 0
        t.register { n++ }
        t.cancel()
        t.cancel()
        t.register { n += 10 }
        assertEquals(11, n)
        assertFailsWith<Cancelled> { t.check() }
    }
}

class DataSetsTest {
    @Test fun choice() {
        val astc = GpuInfo(true, "Mali-G68", "GL")
        val none = GpuInfo(false, "AMD Radeon", "GL")
        assertEquals(DataSets.Kind.SLIM, DataSets.choose("", null, astc).kind)
        assertEquals(DataSets.Kind.FULL, DataSets.choose("auto", null, none).kind)
        assertEquals(DataSets.Kind.FULL, DataSets.choose(null, null, null).kind)
        assertEquals(DataSets.Kind.FULL, DataSets.choose(null, null, GpuInfo(null, error = "no GL")).kind)
        assertTrue(DataSets.choose(null, null, GpuInfo(null, error = "no GL")).reason.contains("no GL"))
        assertEquals(DataSets.Kind.FULL, DataSets.choose("full", null, astc).kind)
        assertEquals(DataSets.Kind.SLIM, DataSets.choose("slim", null, none).kind)
        assertEquals(DataSets.Kind.FULL, DataSets.choose("slim", "full", astc).kind, "the environment wins")
        val custom = DataSets.choose("", "http://127.0.0.1:1/x.zip", astc)
        assertEquals(DataSets.Kind.CUSTOM, custom.kind)
        assertEquals("http://127.0.0.1:1/x.zip", custom.url)
        assertTrue(DataSets.choose("", null, astc).automatic)
        assertFalse(DataSets.choose("slim", null, astc).automatic)
    }

    @Test fun rate() {
        val m = RateMeter(1000)
        m.sample(0, 0)
        m.sample(1000, 10L shl 20)
        assertEquals(10.0 * (1 shl 20), m.bytesPerSec)
        assertEquals(9, m.secondsLeft(10L shl 20, 100L shl 20))
        assertEquals("1 min 30 s left", RateMeter.formatDuration(90))
        assertEquals("10.0 MB/s", RateMeter.formatRate(m.bytesPerSec))
        assertEquals(-1, RateMeter().secondsLeft(0, 10))
    }
}
