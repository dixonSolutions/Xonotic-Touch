package io.github.dixonsolutions.xonotictouch

import android.app.Activity
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.text.format.Formatter
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView

/** Native download supervisor. The existing boot activity still handles updates and launch. */
class GodnoticBootActivity : Activity() {
    private lateinit var data: GameData
    private lateinit var status: TextView
    private lateinit var detail: TextView
    private lateinit var progress: ProgressBar
    private lateinit var music: CheckBox
    private lateinit var action: Button
    private lateinit var cancel: Button
    private var plan: PackFetch.Plan? = null
    private var worker: Thread? = null
    private var generation = 0
    private var cancelled = false

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        if (XonoticActivity.running) { continueToBoot(); return }
        data = GameData(this)
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(32, 24, 32, 24) }
        status = TextView(this).apply { textSize = 22f; text = "Preparing Godnotic" }
        detail = TextView(this).apply { textSize = 16f }
        progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply { max = 100; isIndeterminate = true }
        music = CheckBox(this).apply { text = "Include music"; visibility = View.GONE }
        action = Button(this).apply { text = "Download"; visibility = View.GONE }
        cancel = Button(this).apply { text = "Cancel" }
        box.addView(status); box.addView(detail); box.addView(progress); box.addView(music); box.addView(action); box.addView(cancel)
        setContentView(box)
        music.setOnCheckedChangeListener { _, _ -> updateChoice() }
        action.setOnClickListener { if (plan == null) prepare() else download() }
        cancel.setOnClickListener { cancelWork() }
        prepare()
    }

    private fun startWork(block: (Int) -> Unit) {
        if (worker != null) return
        val id = ++generation
        cancelled = false
        action.visibility = View.GONE
        music.visibility = View.GONE
        cancel.visibility = View.VISIBLE
        progress.visibility = View.VISIBLE
        progress.isIndeterminate = true
        worker = Thread({
            try { block(id) }
            catch (e: Exception) { post(id) { status.text = "Could not prepare game data"; detail.text = e.message ?: e.javaClass.simpleName; plan = null; showRetry() } }
            finally {
                runOnUiThread {
                    worker = null
                    if (id != generation && !isFinishing && !isDestroyed) {
                        status.text = "Download cancelled"
                        detail.text = "Partial downloads are kept for resume."
                        showRetry()
                    }
                }
            }
        }, "godnotic-setup").also { it.start() }
    }
    private fun post(id: Int, action: () -> Unit) = runOnUiThread {
        if (id == generation && !cancelled && !isFinishing && !isDestroyed) action()
    }
    private fun report(id: Int) = GameData.Progress { text, note, percent ->
        post(id) { status.text = text; detail.text = note; progress.isIndeterminate = percent < 0; if (percent >= 0) progress.progress = percent }
    }
    private fun archiveUrl(): String {
        val debug = (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE) != 0
        val override = if (debug) intent.getStringExtra("godnotic_data_url") else null
        return override ?: assets.open("godnotic-data-url.txt").bufferedReader().use { it.readText().trim() }
    }
    private fun prepare() {
        plan = null
        status.text = "Preparing Godnotic"
        detail.text = "Checking installed game data"
        startWork { id ->
            data.prepare(report(id))
            if (Thread.currentThread().isInterrupted) return@startWork
            if (!data.assetsMissing()) { post(id) { continueToBoot() }; return@startWork }
            post(id) { status.text = "Checking download"; detail.text = "Getting the game data list" }
            val found = try {
                PackFetch.plan(archiveUrl(), null, "Xonotic/data/")
            } catch (e: java.io.IOException) {
                // A pinned archive that is not published yet, or a mirror that
                // refuses range requests, must not block setup: the autobuild
                // carries the same pk3s and is the source Linux downloads from.
                android.util.Log.w("Godnotic", "Pinned data archive unusable; using the autobuild", e)
                data.planDownload()
            }
            post(id) { plan = found; status.text = "Download game data"; updateChoice() }
        }
    }
    private fun updateChoice() {
        val current = plan ?: return
        val need = PackFetch.need(data.dataDir(), PackFetch.todo(data.dataDir(), current, music.isChecked), current)
        val required = need.disk + PackFetch.SLACK_BYTES
        val free = PackFetch.freeBytes(data.dataDir())
        val size = { n: Long -> Formatter.formatShortFileSize(this, n) }
        detail.text = "Download ${size(need.fetch)}${if (need.resumed > 0) " (${size(need.resumed)} already downloaded)" else ""}\nStorage needed: ${size(required)} · Available: ${size(free)}"
        progress.visibility = View.GONE
        music.visibility = if (current.music() != null) View.VISIBLE else View.GONE
        action.visibility = View.VISIBLE
        action.text = if (need.resumed > 0) "Resume" else "Download"
        action.isEnabled = free >= required
        if (!action.isEnabled) detail.append("\nNot enough free storage.")
        cancel.visibility = View.VISIBLE
    }
    private fun download() {
        val current = plan ?: return
        val withMusic = music.isChecked
        status.text = "Downloading game data"
        startWork { id ->
            data.download(current, withMusic, report(id))
            if (!Thread.currentThread().isInterrupted) post(id) { continueToBoot() }
        }
    }
    private fun showRetry() {
        progress.visibility = View.GONE
        music.visibility = View.GONE
        action.text = "Retry"
        action.isEnabled = true
        action.visibility = View.VISIBLE
        cancel.visibility = View.GONE
    }
    private fun cancelWork() {
        if (worker == null) { finish(); return }
        cancelled = true
        generation++
        worker?.interrupt()
        status.text = "Cancelling…"
        cancel.visibility = View.GONE
    }
    private fun continueToBoot() {
        startActivity(Intent(this, BootActivity::class.java))
        finish()
    }
    override fun onDestroy() {
        generation++
        worker?.interrupt()
        super.onDestroy()
    }
}
