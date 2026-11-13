// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import platform.posix.CLOCK_MONOTONIC
import platform.posix.clock_gettime
import platform.posix.timespec
import xt.core.*

private var ctx: Ctx? = null
private var mainWindow: LauncherWindow? = null
private var startPage = "play"
var windowSize: Pair<Int, Int>? = null
private var timing = false
private var exitAfterFrame = false
private val t0 = nowMs()

fun nowMs(): Double = memScoped {
    val ts = alloc<timespec>()
    clock_gettime(CLOCK_MONOTONIC, ts.ptr)
    ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6
}

private var snapshotPath: String? = null
private var snapshotDelay = 1500
var startServerNow = false

private val firstFrame = staticCFunction { _: CPointer<GtkWidget>?, _: CPointer<GdkFrameClock>?, _: gpointer? ->
    if (timing) println("first frame after ${kotlin.math.round((nowMs() - t0) * 10) / 10} ms in process")
    if (exitAfterFrame) g_application_quit(g_application_get_default())
    snapshotPath?.let { path ->
        g_timeout_add(snapshotDelay.toUInt(), snapshotRetry, StableRef.create(path).asCPointer())
    }
    0
}

/** Waits [snapshotDelay], then polls every 100 ms until the window can be captured. */
private val snapshotRetry = staticCFunction { d: COpaquePointer? ->
    g_timeout_add(100u, snapshotTramp, d)
    0
}

/** Test aid: renders the window to a PNG (works headless, e.g. on Broadway) and quits. */
private var snapshotTries = 0
private val snapshotTramp = staticCFunction { d: COpaquePointer? ->
    val path = d!!.asStableRef<String>().get()
    val win = mainWindow!!.widget
    val w = gtk_widget_get_width(win)
    val h = gtk_widget_get_height(win)
    val paintable = gtk_widget_paintable_new(win)
    val snap = gtk_snapshot_new()
    gdk_paintable_snapshot(paintable?.reinterpret(), snap?.reinterpret(), w.toDouble(), h.toDouble())
    val node = gtk_snapshot_free_to_node(snap)
    g_object_unref(paintable)
    val renderer = gtk_native_get_renderer(win.reinterpret())
    // The paintable is empty until GTK has drawn the window once more; try again shortly.
    if ((node == null || renderer == null) && ++snapshotTries < 50) return@staticCFunction 1
    if (node != null && renderer != null) {
        val tex = gsk_renderer_render_texture(renderer, node, null)
        gdk_texture_save_to_png(tex, path)
        g_object_unref(tex)
        gsk_render_node_unref(node)
        println("snapshot ${w}x$h -> $path")
    } else {
        eprintln("snapshot failed")
    }
    d.asStableRef<String>().dispose()
    g_application_quit(g_application_get_default())
    0
}

private val onActivate = staticCFunction { app: CPointer<GtkApplication>?, _: gpointer? ->
    val existing = mainWindow
    if (existing != null) {
        existing.present()
    } else {
        installCss()
        val w = LauncherWindow(ctx!!, app, startPage)
        mainWindow = w
        gtk_widget_add_tick_callback(w.widget, firstFrame, null, null)
        if (timing) {
            w.widget.on("destroy") { println("window destroyed at ${nowMs() - t0} ms") }
            w.widget.on("unmap") { println("window unmapped at ${nowMs() - t0} ms") }
        }
        w.present()
        ctx!!.rescan()
    }
}

private val onShutdown = staticCFunction { _: CPointer<GApplication>?, _: gpointer? ->
    if (timing) println("shutdown at ${nowMs() - t0} ms")
    stopServer()
    ctx?.let { if (it.cfg.serialize() != it.savedText) Sys.writeText(it.configPath, it.cfg.serialize()) }
    Unit
}

private const val HELP = """Usage: xt-launcher [OPTION]
  --play            start the game now with the saved choices
  --launcher        show the launcher even when "Open launcher" is off
  --page=NAME       open on play, mods, host or settings
  --start-server    open the Host page and start the local server
  --snapshot=FILE   render the window to a PNG and quit (tests; with --size=WxH)
  --help            this text
"""

fun main(args: Array<String>) {
    val env = Sys.env()
    val configDir = (env["XDG_CONFIG_HOME"]?.takeIf { it.isNotEmpty() } ?: "${env["HOME"]}/.config") + "/xonotic-touch"
    val configPath = "$configDir/launcher.conf"
    val cfg = Sys.readText(configPath)?.let { LauncherConfig.parse(it) } ?: LauncherConfig()
    val c = Ctx(env, configPath, cfg)
    c.savedText = cfg.serialize()
    ctx = c

    var forceLauncher = false
    var play = false
    for (a in args) when {
        a == "--help" || a == "-h" -> { print(HELP); return }
        a == "--launcher" -> forceLauncher = true
        a == "--play" -> play = true
        a.startsWith("--page=") -> { startPage = a.substringAfter('='); forceLauncher = true }
        a.startsWith("--snapshot=") -> { snapshotPath = a.substringAfter('='); forceLauncher = true }
        a.startsWith("--snapshot-delay=") -> snapshotDelay = a.substringAfter('=').toIntOrNull() ?: 1500
        a == "--start-server" -> { startServerNow = true; startPage = "host"; forceLauncher = true }
        a.startsWith("--size=") -> windowSize = Launch.parseResolution(a.substringAfter('='))
    }
    timing = env["XT_LAUNCHER_TIMING"] == "1"
    exitAfterFrame = env["XT_LAUNCHER_EXIT_AFTER_FRAME"] == "1"

    // Fast path: no GTK at all when the player asked to skip the launcher.
    if (play || (!forceLauncher && cfg.bool(Keys.SKIP_LAUNCHER, false))) {
        val start = c.paths.startScript
        if (start != null) {
            val err = Sys.exec(Launch.game(cfg, start))
            eprintln("could not start $start: $err")
        } else {
            eprintln("game not found; opening the launcher")
        }
    }

    val app = adw_application_new(APP_ID, if (snapshotPath != null || exitAfterFrame) G_APPLICATION_NON_UNIQUE else G_APPLICATION_DEFAULT_FLAGS)
    g_signal_connect_data(app, "activate", onActivate.reinterpret(), null, null, 0u)
    g_signal_connect_data(app, "shutdown", onShutdown.reinterpret(), null, null, 0u)
    val status = g_application_run(app?.reinterpret(), 0, null)
    g_object_unref(app)
    platform.posix.exit(status)
}
