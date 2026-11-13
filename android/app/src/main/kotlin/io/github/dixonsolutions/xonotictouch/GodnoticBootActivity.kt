package io.github.dixonsolutions.xonotictouch

import android.app.Activity
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.text.format.Formatter
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView

/**
 * Native download supervisor. Music is part of the game data, not a choice:
 * the phone packs carry it as Ogg Opus, so setup always fetches it.
 */
class GodnoticBootActivity : Activity() {
    private lateinit var data: GameData
    private lateinit var status: TextView
    private lateinit var detail: TextView
    private lateinit var progress: ProgressBar
    private lateinit var action: Button
    private lateinit var quitButton: Button
    private var plan: PackFetch.Plan? = null
    private var worker: Thread? = null
    private var generation = 0

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        if (XonoticActivity.running) { continueToBoot(); return }
        data = GameData(this)
        val density = resources.displayMetrics.density
        fun dp(v: Int) = (v * density).toInt()

        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            setPadding(dp(40), dp(32), dp(40), dp(32))
        }
        status = TextView(this).apply {
            textSize = 26f
            gravity = Gravity.CENTER
            setTextColor(Color.WHITE)
            text = "Preparing Godnotic"
        }
        detail = TextView(this).apply {
            textSize = 16f
            gravity = Gravity.CENTER
            setTextColor(Color.parseColor("#C3CDDC"))
            setPadding(0, dp(10), 0, 0)
        }
        progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            isIndeterminate = true
            visibility = View.GONE
            progressTintList = android.content.res.ColorStateList.valueOf(Color.parseColor("#2F6FED"))
        }
        action = Button(this).apply {
            text = "DOWNLOAD"
            isAllCaps = true
            visibility = View.GONE
            setTextColor(Color.WHITE)
            background = pill(dp(32), "#2F6FED")
            minHeight = dp(56)
        }
        quitButton = Button(this).apply {
            text = "QUIT"
            isAllCaps = true
            setTextColor(Color.WHITE)
            background = pill(dp(32), "#273040")
            minHeight = dp(56)
        }
        box.addView(status)
        box.addView(detail, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        box.addView(progress, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(8)).apply { topMargin = dp(20) })
        box.addView(action, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(56)).apply { topMargin = dp(24) })
        box.addView(quitButton, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(56)).apply { topMargin = dp(12) })
        setContentView(box)

        action.setOnClickListener { if (plan == null) prepare() else download() }
        quitButton.setOnClickListener { quit() }
        prepare()
    }

    /** A fully rounded button, so the screen matches the GTK launcher's pills. */
    private fun pill(radius: Int, color: String) = GradientDrawable().apply {
        cornerRadius = radius.toFloat()
        setColor(Color.parseColor(color))
    }

    private fun startWork(block: (Int) -> Unit) {
        if (worker != null) return
        val id = ++generation
        action.visibility = View.GONE
        progress.visibility = View.VISIBLE
        progress.isIndeterminate = true
        worker = Thread({
            try {
                block(id)
            } catch (e: Exception) {
                post(id) {
                    status.text = "Could not prepare game data"
                    detail.text = e.message ?: e.javaClass.simpleName
                    plan = null
                    showRetry()
                }
            } finally {
                runOnUiThread {
                    worker = null
                    if (id != generation && !isFinishing && !isDestroyed) {
                        status.text = "Download stopped"
                        detail.text = "Partial downloads are kept for resume."
                        showRetry()
                    }
                }
            }
        }, "godnotic-setup").also { it.start() }
    }

    private fun post(id: Int, action: () -> Unit) = runOnUiThread {
        if (id == generation && !isFinishing && !isDestroyed) action()
    }

    private fun report(id: Int) = GameData.Progress { text, note, percent ->
        post(id) {
            status.text = text
            detail.text = note
            progress.isIndeterminate = percent < 0
            if (percent >= 0) progress.progress = percent
        }
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
        val need = PackFetch.need(data.dataDir(), PackFetch.todo(data.dataDir(), current, true), current)
        val required = need.disk + PackFetch.SLACK_BYTES
        val free = PackFetch.freeBytes(data.dataDir())
        val size = { n: Long -> Formatter.formatShortFileSize(this, n) }
        detail.text = "Download ${size(need.fetch)}${if (need.resumed > 0) " (${size(need.resumed)} already downloaded)" else ""}\n" +
            "Music included · Storage needed: ${size(required)} · Available: ${size(free)}"
        progress.visibility = View.GONE
        action.visibility = View.VISIBLE
        action.text = if (need.resumed > 0) "RESUME" else "DOWNLOAD"
        action.isEnabled = free >= required
        if (!action.isEnabled) detail.append("\nNot enough free storage.")
        quitButton.visibility = View.VISIBLE
    }

    private fun download() {
        val current = plan ?: return
        status.text = "Downloading game data"
        startWork { id ->
            data.download(current, true, report(id))
            if (!Thread.currentThread().isInterrupted) post(id) { continueToBoot() }
        }
    }

    private fun showRetry() {
        progress.visibility = View.GONE
        action.text = "RETRY"
        action.isEnabled = true
        action.visibility = View.VISIBLE
        quitButton.visibility = View.VISIBLE
    }

    private fun quit() {
        generation++
        worker?.interrupt()
        finish()
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
