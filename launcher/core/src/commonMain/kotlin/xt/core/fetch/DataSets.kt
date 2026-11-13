// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.fetch

/** What the launcher learnt about the GPU before the engine ran. */
data class GpuInfo(
    /** null: no GL context could be made, so nothing is known. */
    val astc: Boolean?,
    val renderer: String? = null,
    val api: String? = null,
    val error: String? = null,
)

/**
 * Which archive the pk3s come from. Both are releases of Godnotic-data: the
 * full set is the pinned upstream build; the slim set is the same packs
 * repacked with ASTC textures, about 40 % of the size, which only a GPU with
 * GL_KHR_texture_compression_astc_ldr can draw (the engine checks the same
 * extension before it loads a .ktx). Without that, the full set.
 */
object DataSets {
    const val SLIM_URL = "https://github.com/dixonSolutions/Godnotic-data/releases/download/data-slim/Godnotic-data.zip"
    const val FULL_URL = "https://github.com/dixonSolutions/Godnotic-data/releases/download/data-20261107/Godnotic-data-20261107.zip"
    const val ASTC_EXTENSION = "GL_KHR_texture_compression_astc_ldr"

    /** Environment override for tests: slim, full, or an archive URL. */
    const val ENV = "XT_DATA_SET"

    /** The marker packaging/start.sh and fetch-assets-posix.sh treat as "the packs are installed". */
    const val READY_MARKER = ".assets-ready"
    /** The menu's "music is there" flag (scripts/lib/asset-fetch.sh). */
    const val MUSIC_MARKER = "touch/music-ready.txt"

    enum class Kind(val title: String) { SLIM("Slim (ASTC textures)"), FULL("Full"), CUSTOM("Custom archive") }

    data class Choice(val kind: Kind, val url: String, val reason: String, val automatic: Boolean)

    /**
     * @param setting the launcher's data_set value: "", "auto", "slim" or "full"
     * @param env the XT_DATA_SET environment value, which wins
     */
    fun choose(setting: String?, env: String?, gpu: GpuInfo?): Choice {
        val e = env?.trim().orEmpty()
        when {
            e.equals("slim", true) -> return Choice(Kind.SLIM, SLIM_URL, "$ENV=slim", false)
            e.equals("full", true) -> return Choice(Kind.FULL, FULL_URL, "$ENV=full", false)
            e.startsWith("http://") || e.startsWith("https://") -> return Choice(Kind.CUSTOM, e, "$ENV is set", false)
        }
        when (setting?.trim()?.lowercase()) {
            "slim" -> return Choice(Kind.SLIM, SLIM_URL, "Chosen in Settings", false)
            "full" -> return Choice(Kind.FULL, FULL_URL, "Chosen in Settings", false)
        }
        val gpuName = gpu?.renderer?.let { " ($it)" }.orEmpty()
        return when (gpu?.astc) {
            true -> Choice(Kind.SLIM, SLIM_URL, "Your GPU$gpuName draws ASTC textures", true)
            false -> Choice(Kind.FULL, FULL_URL, "Your GPU$gpuName has no ASTC texture support", true)
            null -> Choice(Kind.FULL, FULL_URL,
                "GPU not checked${gpu?.error?.let { ": $it" }.orEmpty()}; the full set works everywhere", true)
        }
    }

    /**
     * Plans [choice]; an automatic slim choice falls back to the full set when
     * the slim archive is not published (404), so a player is never stuck.
     */
    fun plan(p: FetchPlatform, choice: Choice, token: CancelToken, fullUrl: String = FULL_URL): Pair<Choice, Plan> = try {
        choice to PackFetch.plan(p, choice.url, token)
    } catch (e: HttpStatus) {
        if (e.code != 404 || !choice.automatic || choice.kind != Kind.SLIM) throw e
        val full = Choice(Kind.FULL, fullUrl, "The slim set is not on the server yet; using the full set", true)
        full to PackFetch.plan(p, fullUrl, token)
    }

    /**
     * After a run: the marker the shell scripts look for, so a package that
     * starts without the launcher sees the packs as installed, and the menu's
     * music flag when the music pack is there.
     */
    fun markInstalled(fs: FileSystem, dir: String, plan: Plan) {
        if (plan.entries.any { !PackFetch.installed(fs, dir, it.role) }) return
        fs.writeAtomic("$dir/$READY_MARKER", ByteArray(0))
        if (plan.entries.any { it.isMusic }) {
            fs.mkdirs("$dir/touch")
            fs.writeAtomic("$dir/$MUSIC_MARKER", ByteArray(0))
        }
    }

    /** A setup that stopped part-way: what it wants that is not installed yet. */
    fun unfinished(fs: FileSystem, dir: String): List<String> = PackFetch.Meta.read(fs, dir).let {
        if (it.identity == null) emptyList() else it.missing(fs, dir)
    }
}

/**
 * Speed and time left, smoothed so the numbers do not jump around: an
 * exponential average of the rate over samples at least [windowMs] apart.
 */
class RateMeter(private val windowMs: Long = 1000) {
    private var markMs = -1L
    private var markBytes = 0L
    var bytesPerSec = 0.0
        private set

    fun reset() { markMs = -1L; bytesPerSec = 0.0 }

    fun sample(nowMs: Long, done: Long) {
        if (markMs < 0 || done < markBytes) { markMs = nowMs; markBytes = done; return }
        val dt = nowMs - markMs
        if (dt < windowMs) return
        val r = (done - markBytes) * 1000.0 / dt
        bytesPerSec = if (bytesPerSec == 0.0) r else bytesPerSec * 0.7 + r * 0.3
        markMs = nowMs
        markBytes = done
    }

    /** Seconds left, or -1 when there is no rate yet. */
    fun secondsLeft(done: Long, total: Long): Long =
        if (bytesPerSec < 1.0 || total <= done) -1 else ((total - done) / bytesPerSec).toLong()

    companion object {
        fun formatDuration(s: Long): String = when {
            s < 0 -> "estimating time left"
            s < 60 -> "${s} s left"
            s < 3600 -> "${s / 60} min ${s % 60} s left"
            else -> "${s / 3600} h ${(s % 3600) / 60} min left"
        }

        fun formatRate(bps: Double): String = when {
            bps >= 1 shl 20 -> "${(bps / (1 shl 20) * 10).toLong() / 10.0} MB/s"
            bps >= 1 shl 10 -> "${(bps / (1 shl 10)).toLong()} KB/s"
            else -> "${bps.toLong()} B/s"
        }
    }
}
