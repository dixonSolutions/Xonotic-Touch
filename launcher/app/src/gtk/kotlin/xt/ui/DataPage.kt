// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class, ExperimentalAtomicApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import xt.core.*
import xt.core.fetch.*
import kotlin.concurrent.atomics.AtomicBoolean
import kotlin.concurrent.atomics.AtomicReference
import kotlin.concurrent.atomics.ExperimentalAtomicApi

/**
 * The game data download: plans the archive (two small requests), then runs
 * [PackFetch] on its own thread. Everything here except [work] runs on the
 * GTK main loop; the worker hands progress over through [runOnMain], at most
 * one queued update at a time.
 */
class Download(private val ctx: Ctx) {
    enum class Phase { IDLE, PLANNING, READY, RUNNING, STOPPING, PAUSED, DONE, FAILED }

    var phase = Phase.IDLE
        private set
    var gpu: GpuInfo? = null
        private set
    var choice: DataSets.Choice? = null
        private set
    var plan: Plan? = null
        private set
    var need: Need? = null
        private set
    var free = -1L
        private set
    var progress: FetchProgress? = null
        private set
    var error: String? = null
        private set
    val meter = RateMeter(1500)
    /** --download: start as soon as the plan is read. */
    var autoStart = false

    private val platform by lazy { NativeFetch.platform() }
    private var token: CancelToken? = null
    private var thread: CPointer<GThread>? = null
    private var afterStop: (() -> Unit)? = null
    private val listeners = mutableListOf<() -> Unit>()
    private val latest = AtomicReference<FetchProgress?>(null)
    private val posted = AtomicBoolean(false)

    val dataDir get() = ctx.paths.userData
    val busy get() = phase == Phase.PLANNING || phase == Phase.RUNNING || phase == Phase.STOPPING
    /** Bytes already in part files when nothing is running. */
    val hasParts get() = Sys.list(dataDir).any { it.endsWith(".pk3.part") } || Sys.exists("$dataDir/${PackFetch.META}")

    fun onChange(f: () -> Unit) {
        listeners += f
        f()
    }

    private fun changed() = listeners.forEach { it() }

    private fun set(p: Phase, err: String? = null) {
        phase = p
        error = err
        changed()
    }

    /** Which archive, and why: the GPU probe runs here, on the main thread, against the window. */
    fun decide(): DataSets.Choice {
        val g = gpu ?: Gpu.probe(ctx.window).also { gpu = it }
        return DataSets.choose(ctx.cfg[Keys.DATA_SET], ctx.env[DataSets.ENV], g).also { choice = it }
    }

    /** Reads the archive's directory and works out what is left to fetch. */
    fun planNow() {
        if (busy) return
        val c = decide()
        plan = null
        need = null
        set(Phase.PLANNING)
        val dir = dataDir
        val p = platform
        work({ t ->
            val (used, pl) = DataSets.plan(p, c, t)
            val todo = PackFetch.todo(p.fs, dir, pl)
            Planned(used, pl, PackFetch.need(p.fs, dir, todo, pl), p.fs.freeBytes(dir), todo.size)
        }) { r ->
            r.onSuccess {
                choice = it.choice
                plan = it.plan
                need = it.need
                free = it.free
                if (it.todo == 0) {
                    DataSets.markInstalled(platform.fs, dir, it.plan)
                    set(Phase.DONE)
                    ctx.rescan()
                } else {
                    set(Phase.READY)
                    if (autoStart) { autoStart = false; start() }
                }
            }.onFailure { e -> set(Phase.FAILED, if (e is Cancelled) null else describe(e)) }
        }
    }

    private class Planned(val choice: DataSets.Choice, val plan: Plan, val need: Need, val free: Long, val todo: Int)

    fun start() {
        val pl = plan ?: return planNow()
        if (busy) return
        val dir = dataDir
        val p = platform
        progress = null
        meter.reset()
        set(Phase.RUNNING)
        work({ t ->
            val lock = FetchLock.tryTake(dir)
            if (lock == -1) throw FetchError("The game is downloading its data in another window. Close it or wait for it to finish.")
            try {
                PackFetch(p, dir, pl) { pr ->
                    latest.store(pr)
                    if (posted.compareAndSet(false, true)) runOnMain {
                        posted.store(false)
                        latest.load()?.let { onProgress(it) }
                    }
                }.run(t)
                DataSets.markInstalled(p.fs, dir, pl)
            } finally {
                FetchLock.release(lock)
            }
        }) { r ->
            r.onSuccess {
                set(Phase.DONE)
                ctx.rescan()
            }.onFailure { e ->
                when (e) {
                    is Cancelled -> set(Phase.PAUSED)
                    is ArchiveChanged -> { plan = null; set(Phase.FAILED, describe(e)) }
                    else -> set(Phase.FAILED, describe(e))
                }
                // Whatever happened, what is on disk changed: a fresh "to download".
                refreshNeed()
            }
            afterStop?.let { afterStop = null; it() }
        }
    }

    private fun onProgress(p: FetchProgress) {
        progress = p
        meter.sample(nowMs().toLong(), p.done)
        changed()
    }

    private fun refreshNeed() {
        val pl = plan ?: return
        val fs = platform.fs
        need = PackFetch.need(fs, dataDir, PackFetch.todo(fs, dataDir, pl), pl)
        free = fs.freeBytes(dataDir)
        changed()
    }

    /** Stops; the parts stay and Resume carries on from them. */
    fun pause(then: (() -> Unit)? = null) {
        if (phase != Phase.RUNNING && phase != Phase.PLANNING) { then?.invoke(); return }
        afterStop = then
        set(Phase.STOPPING)
        token?.cancel()
    }

    /** Removes the part files and the setup record; finished packs stay. */
    fun discardParts() {
        if (busy) return
        val dir = dataDir
        for (n in Sys.list(dir)) {
            if (n.endsWith(".pk3.part") || n.endsWith(".pk3.part.state") || n == PackFetch.META) platform.fs.delete("$dir/$n")
        }
        progress = null
        set(Phase.IDLE)
        planNow()
    }

    /** Quitting: stop the worker and wait for it, so the chunk state is saved. */
    fun shutdown() {
        token?.cancel()
        thread?.let { g_thread_join(it) }
        thread = null
    }

    /** Runs [job] on a worker thread with a fresh stop token; [done] gets its result on the main loop. */
    private fun <T> work(job: (CancelToken) -> T, done: (Result<T>) -> Unit) {
        val t = CancelToken(platform.threads.lock())
        token = t
        val body: () -> Unit = {
            val r = runCatching { job(t) }
            runOnMain {
                thread?.let { g_thread_join(it) }
                thread = null
                if (token === t) token = null
                done(r)
            }
        }
        thread = g_thread_new("xt-download", workTramp, StableRef.create(body).asCPointer())
    }
}

private val workTramp = staticCFunction { d: COpaquePointer? ->
    val ref = d!!.asStableRef<() -> Unit>()
    val f = ref.get()
    ref.dispose()
    f()
    null as COpaquePointer?
}

fun dataPage(ctx: Ctx): Widget {
    val dl = ctx.download

    // Header: the icon, what this is, and where it stands.
    val icon = Branding.picture(Branding.icon, 96, "Godnotic") ?: label("")
    val title = label("Game data", "title-1")
    val state = label("", "dim-label", "state-text", wrap = true)
    val head = vbox(8, icon, title, state).margins(8, 0, 0, 0)

    // What will be fetched, and from where.
    val sets = Choices.dataSets
    val setRow = comboRow("Data set", null, sets.map { it.title }, Choices.indexOf(sets, ctx.cfg.string(Keys.DATA_SET))) { i ->
        ctx.cfg[Keys.DATA_SET] = sets[i].id
        ctx.save()
        if (!dl.busy) dl.planNow()
    }
    val gpuRow = actionRow("Graphics", "…", "video-display-symbolic")
    val archiveRow = actionRow("Archive", "…", "package-x-generic-symbolic")
    val needRow = actionRow("To download", "…", "folder-download-symbolic")
    val freeRow = actionRow("Free space", "…", "drive-harddisk-symbolic")
    val folderRow = actionRow("Data folder", dl.dataDir, "folder-symbolic")
    folderRow.suffix(button(null, "folder-open-symbolic", "flat") { Sys.mkdirs(dl.dataDir); openFolder(ctx.window, dl.dataDir) }
        .align(v = GTK_ALIGN_CENTER))
    val info = group("Download", null, setRow, gpuRow, archiveRow, needRow, freeRow, folderRow)

    // Progress
    val bar = gtk_progress_bar_new()!!.css("xt-progress")
    val pct = label("", "title-2", xalign = 0f)
    val detail = label("", "numeric", xalign = 0f, wrap = false)
    gtk_label_set_wrap(detail.cast(), 1)
    val file = label("", "dim-label", "caption", xalign = 0f)
    gtk_label_set_ellipsize(file.cast(), PangoEllipsizeMode.PANGO_ELLIPSIZE_MIDDLE)
    val progressBox = vbox(10, pct, bar, detail, file).css("card", "progress-card")

    val errorLabel = label("", "error-text", wrap = true, xalign = 0f)
    gtk_label_set_justify(errorLabel.cast(), GtkJustification.GTK_JUSTIFY_LEFT)
    val errorBox = vbox(0, errorLabel).css("card", "error-card")

    // Actions: one main button that changes with the phase, Cancel beside it.
    lateinit var main: Widget
    val cancel = button("Cancel", "process-stop-symbolic", "big-action") {
        dl.pause { askKeepParts(ctx) }
    }
    val discard = button("Discard partial download", "user-trash-symbolic", "big-action", "flat") {
        askKeepParts(ctx)
    }
    main = button("Download", "folder-download-symbolic", "big-action", "suggested-action") {
        when (dl.phase) {
            Download.Phase.RUNNING -> dl.pause()
            Download.Phase.DONE -> ctx.play()
            Download.Phase.FAILED -> if (dl.plan == null) dl.planNow() else dl.start()
            Download.Phase.IDLE -> dl.planNow()
            else -> dl.start()
        }
    }
    val actions = hbox(12, main, cancel).align(h = GTK_ALIGN_CENTER)
    val actionsBox = vbox(8, actions, discard.align(h = GTK_ALIGN_CENTER))

    fun mainButton(text: String, iconName: String, suggested: Boolean) {
        val c = adw_button_content_new()!!
        adw_button_content_set_label(c.cast(), text)
        adw_button_content_set_icon_name(c.cast(), iconName)
        gtk_button_set_child(main.cast(), c)
        if (suggested) gtk_widget_add_css_class(main, "suggested-action") else gtk_widget_remove_css_class(main, "suggested-action")
    }

    dl.onChange {
        val plan = dl.plan
        val need = dl.need
        val c = dl.choice
        setRow.setSubtitle(c?.let { "${it.kind.title}: ${it.reason}" } ?: "Picked from what your GPU can draw")
        gpuRow.setSubtitle(dl.gpu?.let { g ->
            if (g.astc == null) "Not checked: ${g.error ?: "no OpenGL"}"
            else "${g.renderer ?: "Unknown GPU"} · ${g.api} · ASTC textures: ${if (g.astc == true) "yes" else "no"}"
        } ?: "…")
        archiveRow.setSubtitle(plan?.let { "${it.url.substringAfterLast('/')} · ${formatBytes(it.size)} · ${it.entries.size} packs" }
            ?: c?.url?.substringAfterLast('/') ?: "…")
        needRow.setSubtitle(when {
            need == null -> "…"
            need.fetch == 0L -> "Nothing: every pack is here"
            need.resumed > 0 -> "${formatBytes(need.fetch)} (${formatBytes(need.resumed)} already here)"
            else -> formatBytes(need.fetch)
        })
        val short = need != null && dl.free in 0 until need.disk + FetchConfig().slack
        freeRow.setSubtitle(when {
            dl.free < 0 -> "…"
            dl.free == Long.MAX_VALUE -> "Unknown"
            short -> "${formatBytes(dl.free)} free: not enough, the packs need ${formatBytes(need.disk + FetchConfig().slack)}"
            else -> "${formatBytes(dl.free)} free"
        })
        if (short) freeRow.css("warning-row") else gtk_widget_remove_css_class(freeRow, "warning-row")

        val p = dl.progress
        val running = dl.phase == Download.Phase.RUNNING || dl.phase == Download.Phase.STOPPING
        progressBox.visible(running || (dl.phase == Download.Phase.PAUSED && p != null))
        if (p != null && p.total > 0) {
            val f = p.done.toDouble() / p.total
            gtk_progress_bar_set_fraction(bar.cast(), f.coerceIn(0.0, 1.0))
            pct.setLabel("${(f * 100).toInt()} %")
            val rate = dl.meter.bytesPerSec
            detail.setLabel(buildString {
                append("${formatBytes(p.done)} of ${formatBytes(p.total)}")
                if (dl.phase == Download.Phase.RUNNING && rate > 0) {
                    append(" · ").append(RateMeter.formatRate(rate))
                    append(" · ").append(RateMeter.formatDuration(dl.meter.secondsLeft(p.done, p.total)))
                }
            })
            file.setLabel(when {
                dl.phase == Download.Phase.STOPPING -> "Stopping…"
                dl.phase == Download.Phase.PAUSED -> "Paused. Resume carries on from here."
                p.verifying && p.file != null -> "Checking ${p.file}"
                else -> "Fetching ${plan?.entries?.size ?: 0} packs over 8 connections"
            })
        } else if (running) {
            gtk_progress_bar_pulse(bar.cast())
            pct.setLabel("Starting…")
            detail.setLabel("")
            file.setLabel("")
        }

        errorBox.visible(dl.phase == Download.Phase.FAILED && dl.error != null)
        dl.error?.let { errorLabel.setLabel(it) }

        state.setLabel(when (dl.phase) {
            Download.Phase.IDLE -> if (ctx.data?.ready == true) "All game data is installed. Download checks the server for anything missing."
                else "Godnotic needs its game data (maps, textures, sounds) before the first match."
            Download.Phase.PLANNING -> "Looking at the archive on the server…"
            Download.Phase.READY -> if ((need?.resumed ?: 0) > 0) "A download was stopped part-way. Resume carries on from it."
                else "Ready to download. It can be paused and resumed at any time."
            Download.Phase.RUNNING -> "Downloading. You can pause, or close the launcher: it resumes from where it stopped."
            Download.Phase.STOPPING -> "Stopping…"
            Download.Phase.PAUSED -> "Paused. The downloaded part is kept."
            Download.Phase.DONE -> "All game data is installed."
            Download.Phase.FAILED -> "The download did not finish."
        })

        when (dl.phase) {
            Download.Phase.RUNNING -> mainButton("Pause", "media-playback-pause-symbolic", false)
            Download.Phase.PAUSED -> mainButton("Resume", "media-playback-start-symbolic", true)
            Download.Phase.DONE -> mainButton("Play", "media-playback-start-symbolic", true)
            Download.Phase.FAILED -> mainButton("Try Again", "view-refresh-symbolic", true)
            Download.Phase.READY -> mainButton(if ((need?.resumed ?: 0) > 0) "Resume" else "Download", "folder-download-symbolic", true)
            else -> mainButton("Download", "folder-download-symbolic", true)
        }
        main.sensitive(dl.phase != Download.Phase.PLANNING && dl.phase != Download.Phase.STOPPING && !(dl.phase == Download.Phase.READY && short))
        cancel.visible(dl.phase == Download.Phase.RUNNING || dl.phase == Download.Phase.PAUSED || dl.phase == Download.Phase.STOPPING)
        discard.visible(!dl.busy && dl.phase != Download.Phase.PAUSED && dl.phase != Download.Phase.DONE && dl.hasParts)
        setRow.sensitive(!dl.busy)
    }

    // Not installed: read the archive's directory as soon as the page is up
    // (on the next main-loop turn, when the window can be asked about its GPU).
    ctx.onData {
        val ready = ctx.data?.ready == true
        if ((!ready || dl.autoStart) && dl.phase == Download.Phase.IDLE) runOnMain { if (dl.phase == Download.Phase.IDLE) dl.planNow() }
    }

    // Progress and the buttons first, so a phone shows them without scrolling.
    val content = vbox(24, head, progressBox, errorBox, actionsBox, info).margins(12, 16, 32, 16)
    return scrolled(clamp(content, 720))
}

/** After Cancel: keep the part files for later, or remove them. */
private fun askKeepParts(ctx: Ctx) {
    val dl = ctx.download
    if (!dl.hasParts) return
    val d = adw_alert_dialog_new("Keep the downloaded part?", "Kept, the next download carries on from it. Discarded, it starts from the beginning.")!!
    adw_alert_dialog_add_response(d.cast(), "keep", "Keep for Later")
    adw_alert_dialog_add_response(d.cast(), "discard", "Discard")
    adw_alert_dialog_set_response_appearance(d.cast(), "discard", AdwResponseAppearance.ADW_RESPONSE_DESTRUCTIVE)
    adw_alert_dialog_set_default_response(d.cast(), "keep")
    adw_alert_dialog_set_close_response(d.cast(), "keep")
    d.onString("response") { r -> if (r == "discard") dl.discardParts() }
    adw_dialog_present(d.cast(), ctx.window)
}

/**
 * `xt-launcher --fetch`: the same download with no window, for scripts and
 * for packages without a display. Progress goes to stdout once a second.
 * Returns the exit status.
 */
fun headlessFetch(c: Ctx): Int {
    val p = NativeFetch.platform()
    val dir = c.paths.userData
    val choice = DataSets.choose(c.cfg[Keys.DATA_SET], c.env[DataSets.ENV], GpuInfo(null, error = "no window to ask"))
    println("data folder: $dir")
    println("archive: ${choice.url} (${choice.reason})")
    xtnative.xt_catch_stop_signals()
    val token = CancelToken(p.threads.lock())
    var status = 0
    val meter = RateMeter(1000)
    var lastPrint = 0L
    val lock = FetchLock.tryTake(dir)
    if (lock == -1) {
        eprintln("the game is downloading its data already (touch/fetch.lock is held)")
        return 3
    }
    val worker = p.threads.start("xt-fetch") {
        try {
            val (used, plan) = DataSets.plan(p, choice, token)
            val need = PackFetch.need(p.fs, dir, PackFetch.todo(p.fs, dir, plan), plan)
            println("plan: ${plan.entries.size} packs, archive ${formatBytes(plan.size)}, to fetch ${formatBytes(need.fetch)}" +
                (if (need.resumed > 0) ", ${formatBytes(need.resumed)} resumed" else "") + " [${used.kind.title}]")
            PackFetch(p, dir, plan) { pr ->
                val now = p.threads.nowMs()
                meter.sample(now, pr.done)
                if (now - lastPrint >= 1000 || pr.verifying || pr.done == pr.total) {
                    lastPrint = now
                    val pct = if (pr.total > 0) pr.done * 100 / pr.total else 0
                    println("$pct% ${formatBytes(pr.done)} / ${formatBytes(pr.total)} ${RateMeter.formatRate(meter.bytesPerSec)}" +
                        " ${RateMeter.formatDuration(meter.secondsLeft(pr.done, pr.total))}" + (if (pr.verifying) " checking ${pr.file}" else ""))
                }
            }.run(token)
            DataSets.markInstalled(p.fs, dir, plan)
            println("done: all packs installed in $dir")
        } catch (t: Throwable) {
            eprintln(describe(t))
            status = if (t is Cancelled) 130 else 1
        }
    }
    val watcher = p.threads.start("xt-fetch-signal") {
        while (status == 0 && !token.cancelled) {
            if (xtnative.xt_stop_requested() != 0) { println("stopping; the parts are kept"); token.cancel() }
            p.threads.sleep(100)
        }
    }
    worker.join()
    token.cancel()
    watcher.join()
    FetchLock.release(lock)
    return status
}
