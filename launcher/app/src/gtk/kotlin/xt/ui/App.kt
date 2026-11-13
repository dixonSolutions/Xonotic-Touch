// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import xt.core.*

const val APP_ID = "io.github.dixonSolutions.XonoticTouch"

/** What every page shares: settings, paths, the window, and scan results. */
class Ctx(val env: Map<String, String>, val configPath: String, val cfg: LauncherConfig) {
    var paths: GamePaths = resolvePaths()
        private set
    var window: Widget? = null
    var toasts: Widget? = null
    var data: DataStatus? = null
        private set
    var maps: List<String>? = null
        private set
    private val dataListeners = mutableListOf<() -> Unit>()
    private var saveQueued = false
    /** The config text last written, so quitting does not rewrite an unchanged file. */
    var savedText = ""

    fun resolvePaths() = PathResolver.resolve(env, Sys.exeDir(), Sys::exists, cfg.string(Keys.USER_BASE))

    fun reresolve() {
        paths = resolvePaths()
        rescan()
    }

    fun toast(text: String) {
        val t = adw_toast_new(text)
        adw_toast_set_timeout(t, 3u)
        toasts?.let { adw_toast_overlay_add_toast(it.cast(), t) }
    }

    /** Settings are written shortly after the last change, off the main thread. */
    fun save() {
        if (saveQueued) return
        saveQueued = true
        g_timeout_add(400u, saveTramp, StableRef.create(this).asCPointer())
    }

    internal fun flush() {
        saveQueued = false
        val text = cfg.serialize()
        val path = configPath
        savedText = text
        background({ Sys.writeText(path, text) }) { ok -> if (!ok) toast("Could not save settings to $path") }
    }

    fun onData(f: () -> Unit) {
        dataListeners += f
        if (data != null) f()
    }

    /** Looks at the packs and lists their maps in the background. */
    fun rescan() {
        val p = paths
        background({ scanData(p) }) { (status, mapList) ->
            data = status
            maps = mapList
            dataListeners.forEach { it() }
        }
    }

    fun play() {
        val start = paths.startScript
        if (start == null) {
            toast("Game not found. Set its folder in Settings.")
            return
        }
        val err = Sys.exec(Launch.game(cfg, start))
        toast("Could not start the game: $err")
    }
}

private val saveTramp = staticCFunction { d: COpaquePointer? ->
    val ref = d!!.asStableRef<Ctx>()
    ref.get().flush()
    ref.dispose()
    0
}

/** Runs on an I/O thread: pack count and size, the ready marker, and every map name. */
fun scanData(p: GamePaths): Pair<DataStatus, List<String>> {
    var packs = 0
    var bytes = 0L
    var main: String? = null
    val maps = mutableListOf<String>()
    for (dir in p.dataDirs) {
        for (name in Sys.list(dir)) {
            val path = "$dir/$name"
            when {
                name.endsWith(".pk3", ignoreCase = true) -> {
                    packs++
                    bytes += Sys.size(path).coerceAtLeast(0)
                    if (DataStatus.isMainPack(name)) main = name
                    Sys.withZip(path) { Zip.mapNames(Zip.entryNames(it)) }?.let { maps.addAll(it) }
                }
                name.endsWith(".pk3dir", ignoreCase = true) && Sys.isDir("$path/maps") ->
                    maps.addAll(Sys.list("$path/maps").mapNotNull { Zip.mapName("maps/$it") })
            }
        }
    }
    val ready = main != null || Sys.exists("${p.userData}/.assets-ready")
    return DataStatus(ready, packs, bytes, main) to maps.distinctBy { it.lowercase() }.sortedBy { it.lowercase() }
}

fun loadProfiles(p: GamePaths): List<Profile> {
    for (dir in p.profileDirs) {
        val files = Sys.list(dir).filter { it.endsWith(".cfg") }
        if (files.isEmpty()) continue
        return files.mapNotNull { f ->
            val id = f.removeSuffix(".cfg")
            Sys.readText("$dir/$f", 4096)?.let { Profiles.parse(id, it) }
        }
    }
    return Profiles.builtin
}

private const val CSS = """
:root {
  --accent-bg-color: #3fa9f5;
  --accent-fg-color: #04121f;
  --accent-color: #6cc0ff;
  --window-bg-color: #0b0f14;
  --view-bg-color: #0e141b;
  --headerbar-bg-color: #0b0f14;
  --headerbar-shade-color: rgba(0,0,0,0.6);
  --card-bg-color: #121922;
  --popover-bg-color: #16202b;
  --dialog-bg-color: #121922;
  --sidebar-bg-color: #0e141b;
  --destructive-bg-color: #d9534a;
}
window.xt { background: #0b0f14; }
.hero { padding: 28px 12px 8px 12px; border-radius: 24px;
  background: linear-gradient(180deg, rgba(63,169,245,0.13), rgba(63,169,245,0) 70%); }
.brand { font-size: 30px; font-weight: 900; letter-spacing: 6px; color: #e6edf3; }
.brand-accent { color: #3fa9f5; }
.tagline { color: #8b9bb0; font-size: 15px; }
.play-button {
  min-height: 84px; min-width: 280px; border-radius: 999px;
  font-size: 26px; font-weight: 900; letter-spacing: 8px;
  background: linear-gradient(180deg, #5bb8ff, #2b8fe0); color: #04121f;
  box-shadow: 0 6px 16px rgba(63,169,245,0.30), inset 0 1px rgba(255,255,255,0.35);
}
.play-button:hover { background: linear-gradient(180deg, #74c4ff, #3a9bea); }
.play-button:active { background: linear-gradient(180deg, #2b8fe0, #1d76c2); }
.status-pill { border-radius: 999px; padding: 6px 14px; background: rgba(139,155,176,0.12); color: #b8c6d6; font-size: 14px; }
.status-pill.ok { background: rgba(80,200,120,0.14); color: #8be0a8; }
.status-pill.warn { background: rgba(255,170,60,0.14); color: #ffc07a; }
.console { font-family: monospace; font-size: 12px; background: #070a0e; color: #cfe3f5; padding: 8px; }
.console-frame { border-radius: 12px; border: 1px solid #233041; }
.big-action { min-height: 56px; border-radius: 16px; font-size: 18px; font-weight: 800; padding: 0 28px; }
.section-gap { margin-top: 18px; }
/* Phones and narrow windows: bigger targets for thumbs. */
window.narrow row.activatable, window.narrow row.combo, window.narrow row.entry, window.narrow row.spin,
window.narrow row.switch-row, window.narrow row { min-height: 64px; }
window.narrow row .title { font-size: 17px; }
window.narrow .play-button { min-height: 96px; min-width: 0; font-size: 28px; }
window.narrow .brand { font-size: 24px; letter-spacing: 4px; }
window.narrow viewswitcherbar button { min-height: 64px; }
window.narrow button.big-action { min-height: 64px; }
"""

class LauncherWindow(val ctx: Ctx, app: CPointer<GtkApplication>?, startPage: String) {
    private val win = adw_application_window_new(app)!!
    private val stack = adw_view_stack_new()!!
    private val built = mutableSetOf<String>()
    private val holders = mutableMapOf<String, Widget>()
    private val builders = linkedMapOf<String, () -> Widget>(
        "play" to { playPage(ctx) },
        "mods" to { modsPage(ctx) },
        "host" to { hostPage(ctx) },
        "settings" to { settingsPage(ctx) },
    )

    init {
        ctx.window = win
        gtk_window_set_title(win.cast(), "Xonotic Touch")
        val (dw, dh) = windowSize ?: (980 to 720)
        gtk_window_set_default_size(win.cast(), dw, dh)
        gtk_widget_set_size_request(win, 360, 480)
        win.css("xt")

        val header = adw_header_bar_new()!!
        val switcher = adw_view_switcher_new()!!
        adw_view_switcher_set_stack(switcher.cast(), stack.cast())
        adw_view_switcher_set_policy(switcher.cast(), AdwViewSwitcherPolicy.ADW_VIEW_SWITCHER_POLICY_WIDE)
        adw_header_bar_set_title_widget(header.cast(), switcher)

        val bar = adw_view_switcher_bar_new()!!
        adw_view_switcher_bar_set_stack(bar.cast(), stack.cast())

        page("play", "Play", "media-playback-start-symbolic")
        page("mods", "Mods", "application-x-addon-symbolic")
        page("host", "Host", "network-server-symbolic")
        page("settings", "Settings", "emblem-system-symbolic")

        val banner = adw_banner_new("")!!
        val tv = adw_toolbar_view_new()!!
        adw_toolbar_view_add_top_bar(tv.cast(), header)
        adw_toolbar_view_add_top_bar(tv.cast(), banner)
        adw_toolbar_view_add_bottom_bar(tv.cast(), bar)
        adw_toolbar_view_set_content(tv.cast(), stack)

        val overlay = adw_toast_overlay_new()!!
        adw_toast_overlay_set_child(overlay.cast(), tv)
        ctx.toasts = overlay
        adw_application_window_set_content(win.cast(), overlay)

        // Narrow: switcher moves to a thumb-height bar at the bottom, rows grow.
        val bp = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 640sp"))!!
        bp.on("apply") {
            adw_header_bar_set_title_widget(header.cast(), null)
            adw_view_switcher_bar_set_reveal(bar.cast(), 1)
            gtk_widget_add_css_class(win, "narrow")
        }
        bp.on("unapply") {
            adw_header_bar_set_title_widget(header.cast(), switcher)
            adw_view_switcher_bar_set_reveal(bar.cast(), 0)
            gtk_widget_remove_css_class(win, "narrow")
        }
        adw_application_window_add_breakpoint(win.cast(), bp)

        // Pages are built the first time they are shown; Play is built now.
        stack.onNotify("visible-child-name") { ensureBuilt(adw_view_stack_get_visible_child_name(stack.cast())?.toKString()) }
        ensureBuilt(startPage)
        adw_view_stack_set_visible_child_name(stack.cast(), startPage)

        if (ctx.paths.startScript == null) {
            adw_banner_set_title(banner.cast(), "Xonotic Touch was not found next to the launcher")
            adw_banner_set_button_label(banner.cast(), "Settings")
            banner.on("button-clicked") { adw_view_stack_set_visible_child_name(stack.cast(), "settings") }
            adw_banner_set_revealed(banner.cast(), 1)
        }
    }

    private fun page(name: String, title: String, icon: String) {
        val holder = vbox()
        holders[name] = holder
        val p = adw_view_stack_add_titled_with_icon(stack.cast(), holder, name, title, icon)
        if (name == "mods") adw_view_stack_page_set_use_underline(p, 0)
    }

    private fun ensureBuilt(name: String?) {
        if (name == null || name in built) return
        val build = builders[name] ?: return
        built += name
        holders[name]?.append(build().expand(h = true, v = true))
    }

    fun present() = gtk_window_present(win.cast())
    val widget get() = win
}

fun installCss() {
    val provider = gtk_css_provider_new()
    gtk_css_provider_load_from_string(provider, CSS)
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), provider?.cast(), 600u)
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(), AdwColorScheme.ADW_COLOR_SCHEME_FORCE_DARK)
}
