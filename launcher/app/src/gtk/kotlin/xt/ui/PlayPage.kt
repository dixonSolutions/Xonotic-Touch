// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import xt.core.*

fun playPage(ctx: Ctx): Widget {
    val cfg = ctx.cfg
    val profiles = loadProfiles(ctx.paths)
    val layouts = Profiles.layouts(profiles).ifEmpty { Profiles.layouts(Profiles.builtin) }
    val perf = Profiles.performance(profiles)

    // Hero: the banner, where the data stands, and one very large button.
    val brand = Branding.picture(Branding.banner, 132, "Godnotic")?.css("banner-pic")
        ?: hbox(12, label("GODNOTIC", "brand", "brand-accent")).align(h = GTK_ALIGN_CENTER)
    val status = label("Checking game data…", "status-pill", wrap = true).align(h = GTK_ALIGN_CENTER)
    val play = button("PLAY", null, "play-button") { ctx.play() }
    gtk_widget_set_tooltip_text(play, "Start Godnotic with these settings")
    val playClamp = clamp(play, 460).margins(18, 4, 8, 4)
    // Shown while the data is missing: the way to the download.
    val getData = button("Download Game Data", "folder-download-symbolic", "big-action", "suggested-action") { ctx.showPage("data") }
        .align(h = GTK_ALIGN_CENTER).visible(false)
    val hero = vbox(10,
        brand,
        label("Arena FPS for phones, tablets and touchscreens", "tagline", wrap = true),
        status,
        getData,
        playClamp,
    ).css("hero")

    fun showStatus() {
        val d = ctx.data ?: return
        val dl = ctx.download
        gtk_widget_remove_css_class(status, "ok")
        gtk_widget_remove_css_class(status, "warn")
        val p = dl.progress
        when {
            dl.phase == Download.Phase.RUNNING && p != null && p.total > 0 -> {
                status.setLabel("Downloading game data · ${p.done * 100 / p.total} %")
                status.css("warn")
            }
            d.ready -> {
                status.setLabel("Game data ready · ${d.summary}")
                status.css("ok")
            }
            dl.phase == Download.Phase.PAUSED || (dl.need?.resumed ?: 0) > 0 -> {
                status.setLabel("Game data download paused · resume it on the Data page")
                status.css("warn")
            }
            else -> {
                status.setLabel("Game data not downloaded yet")
                status.css("warn")
            }
        }
        getData.visible(!d.ready)
        playClamp.visible(d.ready)
    }
    ctx.onData { showStatus() }
    ctx.download.onChange { showStatus() }

    // Controls
    val layoutIdx = layouts.indexOfFirst { it.id == cfg.string(Keys.TOUCH_PROFILE, "standard") }.coerceAtLeast(0)
    lateinit var layoutRow: Widget
    layoutRow = comboRow("Touch layout", layouts[layoutIdx].summary, layouts.map { it.title }, layoutIdx) { i ->
        val p = layouts.getOrNull(i) ?: return@comboRow
        cfg[Keys.TOUCH_PROFILE] = if (p.id == "standard") "" else p.id
        layoutRow.setSubtitle(p.summary)
        ctx.save()
    }
    val perfTitles = listOf("Automatic") + perf.map { it.title }
    val perfIdx = perf.indexOfFirst { it.id == cfg.string(Keys.PERF_PROFILE) }.let { if (it < 0) 0 else it + 1 }
    fun perfSummary(i: Int) = if (i == 0) "The game's own choice; menu changes are kept" else perf[i - 1].summary
    lateinit var perfRow: Widget
    perfRow = comboRow("Performance", perfSummary(perfIdx), perfTitles, perfIdx) { i ->
        cfg[Keys.PERF_PROFILE] = if (i <= 0) "" else perf[i - 1].id
        perfRow.setSubtitle(perfSummary(i))
        ctx.save()
    }
    val controls = group("Controls", null, layoutRow, perfRow)

    // Display
    val modes = Choices.windowModes
    val modeRow = comboRow("Window", null, modes.map { it.title }, Choices.indexOf(modes, cfg.string(Keys.WINDOW_MODE, "auto"))) { i ->
        cfg[Keys.WINDOW_MODE] = modes[i].id.takeIf { it != "auto" }.orEmpty()
        ctx.save()
    }
    val res = Choices.resolutions
    val resRow = comboRow("Resolution", null, res.map { it.title }, Choices.indexOf(res, cfg[Keys.RESOLUTION])) { i ->
        cfg[Keys.RESOLUTION] = res[i].id
        ctx.save()
    }
    val extra = entryRow("Extra arguments", cfg.string(Keys.EXTRA_ARGS)) { t ->
        cfg[Keys.EXTRA_ARGS] = t.trim()
        ctx.save()
    }
    gtk_widget_set_tooltip_text(extra, "Console arguments, e.g. +connect example.org or +cl_maxfps 90")
    val display = group("Display", null, modeRow, resRow, extra)

    val content = vbox(24, hero, controls, display).margins(12, 16, 32, 16)
    return scrolled(clamp(content, 720))
}
