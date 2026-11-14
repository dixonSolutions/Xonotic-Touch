package io.github.dixonsolutions.godnotic.fe

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.FrameLayout
import io.github.dixonsolutions.godnotic.BootSplash
import io.github.dixonsolutions.godnotic.NativeBridge
import java.io.File

/**
 * The native interface's place in XonoticActivity: one overlay over the SDL
 * surface. Not a second window or activity: the engine's surface, its input
 * and its lifecycle stay exactly as they were, and the overlay is GONE
 * during play. While it shows it is clickable edge to edge, so no touch
 * reaches the engine behind it; keys go to it first (dispatchKey), Back
 * included; its text fields get Android's IME.
 *
 * Phases:
 *  1. Skeleton: the boot screen's banner, at the same place, the moment the
 *     activity is created: no disk, no network, no engine needed.
 *  2. The engine's front-end bridge says hello (app topic): the host
 *     attaches (`fe attach 1`, a 1 Hz heartbeat while resumed) and the
 *     engine stops drawing and feeding the QuakeC menu (touch_frontend.c).
 *  3. Screens: from the engine's UI bridge when it has one (app topic
 *     "ui_bridge"; its messages arrive on the "ui" queue and go to
 *     [UibClient]). Until the engine side of the UI bridge lands, STOPGAP:
 *     the bridge's own Home and pause descriptions ship as assets
 *     (fe/home.json, fe/pause.json, from tests/uibridge/fixtures) and their
 *     actions map to engine commands here ([StopgapActions]).
 *  4. Anything wrong (no bridge in the engine, an old resident libnativeui,
 *     an exception here): the overlay goes, `fe detach`, and the QuakeC
 *     menu is in charge, as before this existed.
 */
class FrontendHost(private val activity: Activity, private val baseDir: File?) : ViewRenderer.Host, UiEvents {
    private val tag = "Godnotic"
    private val main = Handler(Looper.getMainLooper())
    val root: FrameLayout = object : FrameLayout(activity) {
        override fun dispatchTouchEvent(ev: android.view.MotionEvent): Boolean {
            // the engine's console button under the pause scrim takes its own touches (4.12)
            if (ev.actionMasked == android.view.MotionEvent.ACTION_DOWN && passThrough(ev.x, ev.y)) return false
            return super.dispatchTouchEvent(ev)
        }
    }
    private val screenLayer = FrameLayout(activity)
    private val dialogLayer = FrameLayout(activity)
    private val toastLayer = FrameLayout(activity)
    private var skeleton: View? = null
    private var backPill: View? = null
    private val started = SystemClock.elapsedRealtime()
    private var engineSeen = false
    private var app: Map<String, Any?> = emptyMap()
    private var resumed = true
    private var attached = false
    private var failed = false
    private var renderer: ViewRenderer? = null
    private var client: UiClient? = null
    private var stopgap: StopgapActions? = null
    private val screens = ArrayList<String>()
    private var drainPosted = false
    private val reported = HashMap<String, String>()
    private var shownAt = 0L
    private var lastSig = ""

    init {
        // transparent: each screen brings its background (the pause menu a scrim over the game)
        root.isClickable = true // nothing behind it while it shows
        root.addView(screenLayer, match())
        root.addView(dialogLayer, match())
        root.addView(toastLayer, match())
        dialogLayer.visibility = View.GONE
        Images.thumbDir = File(activity.cacheDir, "fe-thumbs")
    }

    private fun match() = FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)

    /** Puts the skeleton up at once; call from onCreate, after super.onCreate (SDL's layout exists). */
    fun install() {
        Theme.loadDisplayFont(baseDir)
        val sk = skeleton()
        skeleton = sk
        screenLayer.addView(sk, match())
        activity.addContentView(root, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        root.setOnApplyWindowInsetsListener { _, insets -> applyInsets(insets); insets }
        root.viewTreeObserver.addOnPreDrawListener(object : android.view.ViewTreeObserver.OnPreDrawListener {
            override fun onPreDraw(): Boolean {
                root.viewTreeObserver.removeOnPreDrawListener(this)
                Log.i(tag, "Native UI: skeleton drawn ${SystemClock.elapsedRealtime() - started} ms after onCreate")
                return true
            }
        })
        NativeBridge.onDirty = Runnable { postDrain() }
        watchPower()
        main.postDelayed(tick, 50)
    }

    // ---------------------------------------------------------------- skeleton

    /**
     * Exactly the boot screen's picture (BootSplash.view, the same banner at
     * the same place and size on the same black), so the hand-off from the
     * boot activity is invisible: one screen from the icon tap until Home
     * replaces it with a single fade.
     */
    private fun skeleton(): View {
        val v = BootSplash.view(activity, 0.62f, 900)
        v.contentDescription = "Godnotic is starting"
        return v
    }

    private fun liftSkeleton(why: String) {
        val sk = skeleton ?: return
        skeleton = null
        Log.i(tag, "Native UI: skeleton lifted after ${SystemClock.elapsedRealtime() - started} ms ($why)")
        pendingJoin?.let { addr -> pendingJoin = null; if (!failed) askJoin(addr) }
        val gone = { screenLayer.removeView(sk); if (screens.isEmpty()) root.visibility = View.GONE }
        if (Theme.reducedMotion(activity)) gone() else sk.animate().alpha(0f).setDuration(150).withEndAction(gone).start()
    }

    /** 20 Hz until the skeleton has gone, then 1 Hz for the heartbeat while attached and resumed. */
    private val tick = object : Runnable {
        override fun run() {
            if (activity.isFinishing || activity.isDestroyed) return
            val waited = SystemClock.elapsedRealtime() - started
            if (skeleton != null && screens.isEmpty()) {
                val presented = NativeBridge.framePresented()
                when {
                    !NativeBridge.available || NativeBridge.frontendVersion == 0 -> fallBack("no front-end bridge in this build")
                    !engineSeen && presented && waited > 4000 -> fallBack("the engine has no front-end bridge")
                    waited > 30_000 -> fallBack("nothing in 30 s")
                }
            }
            if (attached && resumed) NativeBridge.frontendBeat()
            if (attached && resumed) checkStuck()
            main.postDelayed(this, if (skeleton != null) 50 else 1000)
        }
    }

    private var stuckSince = 0L

    /**
     * The engine is in the menu but the described screen is still a loading
     * screen (seen after leaving a match): ask for a fresh snapshot after 4 s,
     * and if it stays, hand the menu to QuakeC rather than leave the player on
     * a screen that will not go.
     */
    private fun checkStuck() {
        val top = screens.lastOrNull() ?: return
        val kind = client?.topKind ?: ""
        val stuck = app.str("state") == "menu" && kind == "loading" && root.visibility == View.VISIBLE && dialogLayer.visibility != View.VISIBLE
        val now = SystemClock.elapsedRealtime()
        if (!stuck) { stuckSince = 0; return }
        if (stuckSince == 0L) { stuckSince = now; return }
        if (now - stuckSince in 4000..4999) { Log.w(tag, "Native UI: loading screen over the menu; asking for a snapshot"); client?.sync() }
        if (now - stuckSince > 9000) fallBack("a loading screen stayed over the menu")
    }

    /** The QuakeC menu is in charge for this session. */
    private fun fallBack(why: String) {
        if (failed) return
        failed = true
        Log.i(tag, "Native UI: not used ($why); the QuakeC menu is in charge")
        if (attached) NativeBridge.enqueue(FeArgs.command("detach", why))
        attached = false
        liftSkeleton(why)
        root.visibility = View.GONE
    }

    private fun fail(where: String, e: Throwable) {
        // A native layer that breaks hands the menu back: never a frozen or black screen.
        Log.e(tag, "Native UI failed in $where", e)
        screens.clear()
        screenLayer.removeAllViews()
        fallBack("error in $where")
    }

    // ---------------------------------------------------------------- bridge

    private fun postDrain() {
        synchronized(this) { if (drainPosted) return; drainPosted = true }
        main.post {
            synchronized(this) { drainPosted = false }
            try { drain() } catch (e: Exception) { fail("drain", e) }
        }
    }

    private fun drain() {
        for ((topic, json) in NativeBridge.frontendTake(false)) when (topic) {
            "app" -> onApp(Json.parseOrNull(json).obj())
            "event" -> onEvent(Json.parseOrNull(json).obj())
            "image" -> Json.parseOrNull(json).obj().let { renderer?.imageReady(it.str("ref"), it.str("path")) }
            "ui" -> if (!failed) client?.receiveText(json)
            "mods" -> onMods(Json.parseOrNull(json).obj())
            "connect" -> onConnect(Json.parseOrNull(json).obj())
        }
    }

    private fun onApp(a: Map<String, Any?>) {
        val first = !engineSeen
        engineSeen = true
        app = a
        if (failed) return
        if (first) {
            Log.i(tag, "Native UI: engine bridge protocol ${a.int("proto")}, enabled=${a.bool("enabled")}, ui_bridge=${a.bool("ui_bridge")}, after ${SystemClock.elapsedRealtime() - started} ms")
            reportAll()
            if (!a.bool("enabled") || a.int("proto") != 1) { fallBack("touch_native_frontend is off or the protocol differs"); return }
            attach()
        }
        if (attached && resumed && !a.bool("attached")) reattach()
        val state = a.str("state")
        val sig = "$state menu=${a.bool("menu")} owns=${a.bool("owns_menu")} adv=${a.bool("advanced")} console=${a.bool("console")}"
        if (sig != lastSig) { lastSig = sig; Log.i(tag, "Native UI: engine $sig") }
        when {
            a.bool("advanced") || a.bool("console") -> { setVisible(false); showBackPill(a.bool("advanced")) }
            state == "menu" -> { showBackPill(false); stopgap?.show("home"); setVisible(true) }
            state == "paused" -> { showBackPill(false); stopgap?.show("pause", a); setVisible(true) }
            else -> { showBackPill(false); setVisible(false) } // in a match, or the engine's loading screen
        }
        reportPower("_android_idle_home", if (state == "menu" && root.visibility == View.VISIBLE) "1" else "0")
        showReconnect(a.str("reconnect"))
        if (state != "loading" && state != "connecting" && loadingStrip != null) onConnect(mapOf("stage" to "idle"))
        val note = a.str("reconnect_note")
        if (note != lastReconnectNote) { lastReconnectNote = note; if (note.isNotEmpty() && note.startsWith("Disconnected")) toast(note, "warning") }
    }

    private var loadingStrip: android.widget.LinearLayout? = null
    private var loadingText: android.widget.TextView? = null
    private var loadingBar: android.widget.ProgressBar? = null

    /**
     * A join or a map load: the engine publishes its progress from inside the
     * blocking load (TouchFE_Loading, a few times a second). The engine's own
     * loading screen stops redrawing during a long item; this strip is drawn
     * by Android on its own thread, so the player always sees it moving
     * (an indeterminate bar) with the step and the percentage.
     */
    private fun onConnect(c: Map<String, Any?>) {
        val stage = c.str("stage")
        val busy = stage in setOf("connecting", "downloading", "loading", "signon") && !failed
        if (!busy || root.visibility == View.VISIBLE) {
            loadingStrip?.let { (it.parent as? ViewGroup)?.removeView(it) }
            loadingStrip = null; loadingText = null; loadingBar = null
            return
        }
        if (loadingStrip == null) {
            val strip = android.widget.LinearLayout(activity)
            strip.orientation = android.widget.LinearLayout.VERTICAL
            val p = Theme.dp(activity, 12f)
            strip.setPadding(Theme.dp(activity, 20f), p, Theme.dp(activity, 20f), p)
            strip.background = Theme.rounded(0xE60B1730.toInt(), Theme.dp(activity, 14f).toFloat(), Theme.dp(activity, 1f), Theme.BLUE)
            val t = Kit.text(activity, "", Theme.SP_BODY, Theme.TEXT, bold = true, lines = 1)
            t.accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
            val bar = android.widget.ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal)
            bar.isIndeterminate = true
            bar.indeterminateTintList = android.content.res.ColorStateList.valueOf(Theme.EMBER)
            strip.addView(t)
            strip.addView(bar, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, Theme.dp(activity, 6f)).apply { topMargin = Theme.dp(activity, 8f) })
            val lp = FrameLayout.LayoutParams(Theme.dp(activity, 520f), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL)
            lp.bottomMargin = Theme.dp(activity, 56f)
            activity.addContentView(strip, lp)
            loadingStrip = strip; loadingText = t; loadingBar = bar
        }
        val pct = when (stage) {
            "downloading" -> c.num("download_progress")
            "loading" -> c.num("load_progress")
            else -> 0.0
        }
        val what = when (stage) {
            "connecting" -> "Connecting to ${c.str("address")}"
            "downloading" -> "Downloading ${c.str("file").substringAfterLast('/')}"
            "signon" -> "Joining the game"
            else -> c.str("load").ifEmpty { "Loading the map" }
        }
        loadingText?.text = if (pct > 0.0) "$what  ${(pct * 100).toInt()}%" else what
    }

    private var reconnectBar: View? = null
    private var reconnectText: android.widget.TextView? = null
    private var lastReconnectNote = ""

    /**
     * The automatic reconnect (docs/NETWORK.md, touch_net.c) over whatever is
     * on screen, the game or the loading screen: "Reconnecting to ... (attempt
     * n of 5)" and Cancel (net_reconnect_cancel). Added to the activity, not
     * the overlay, because the overlay is hidden while the engine draws.
     */
    private fun showReconnect(text: String) {
        if (text.isEmpty()) { reconnectBar?.let { (it.parent as? ViewGroup)?.removeView(it) }; reconnectBar = null; reconnectText = null; return }
        reconnectText?.let { if (it.text != text) it.text = text; return }
        val bar = android.widget.LinearLayout(activity)
        bar.orientation = android.widget.LinearLayout.HORIZONTAL
        bar.gravity = Gravity.CENTER_VERTICAL
        val p = Theme.dp(activity, 12f)
        bar.setPadding(Theme.dp(activity, 16f), p / 2, p / 2, p / 2)
        bar.background = Theme.rounded(0xF20B1730.toInt(), Theme.dp(activity, 14f).toFloat(), Theme.dp(activity, 2f), Theme.WARN)
        val t = Kit.text(activity, text, Theme.SP_BODY, Theme.TEXT, bold = true)
        t.accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
        bar.addView(t)
        val cancel = Kit.button(activity, "Cancel") { NativeBridge.enqueue(FeArgs.command("reconnect_cancel")) }
        bar.addView(cancel, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { leftMargin = p })
        val lp = FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.CENTER_HORIZONTAL)
        lp.topMargin = Theme.dp(activity, 12f)
        activity.addContentView(bar, lp)
        reconnectBar = bar
        reconnectText = t
    }

    private fun attach() {
        val r = ViewRenderer(activity, this, this)
        renderer = r
        val send: (String) -> Unit = { msg -> NativeBridge.enqueue(FeArgs.command("ui", msg)) }
        // the Rust core (loader/crates/android) when this build carries it; the Kotlin reader otherwise
        val c: UiClient = (if (app.bool("ui_bridge") && !File(baseDir, "fe-kotlin-core").isFile) RustCore.create(r, send) else null)
            ?: UibClient(r, send)
        Log.i(tag, "Native UI: UI bridge read by ${if (c is RustCore) "the Rust core" else "the Kotlin reader"}")
        c.ignoreRows = testRows
        client = c
        attached = true
        reattachAt = SystemClock.elapsedRealtime() + 2000 // the first app snapshots still say detached
        NativeBridge.frontendBeat()
        NativeBridge.enqueue(FeArgs.command("attach", "1"))
        if (app.bool("ui_bridge")) {
            // touch_warm prepares the map Home's Play starts: the hero's picture names it
            c.onChanged = {
                val ref = c.hero
                if (ref.startsWith("pk3:maps/")) reportPower("touch_warm_map", ref.removePrefix("pk3:maps/"))
            }
            c.hello(caps())
        }
        else {
            stopgap = StopgapActions(activity, c as UibClient)
            // touch_warm prepares what Play starts while Home is idle
            NativeBridge.enqueue(FeArgs.command("set", "touch_warm_map", StopgapActions.STOPGAP_MAP))
        }
    }

    private var reattachAt = 0L

    /**
     * The engine dropped the UI (no heartbeat for 30 s, the bridge closed):
     * attach again and ask for the screens anew, rather than leave the
     * QuakeC menu running under a native screen nobody updates.
     */
    private fun reattach() {
        val now = SystemClock.elapsedRealtime()
        if (now - reattachAt < 3000) return
        reattachAt = now
        Log.w(tag, "Native UI: the engine detached us; attaching again")
        NativeBridge.frontendBeat()
        NativeBridge.enqueue(FeArgs.command("attach", "1"))
        if (app.bool("ui_bridge")) client?.hello(caps())
    }

    private fun caps(): Map<String, Any?> {
        val m = activity.resources.displayMetrics
        val w = (m.widthPixels / m.density).toInt(); val h = (m.heightPixels / m.density).toInt()
        return mapOf("nodes" to listOf("app", "screen", "toast", "section", "toolbar", "actionbar", "row", "grid", "tabs", "pane", "split", "form",
            "card", "hero", "list", "preview", "text", "image", "divider", "status", "progress", "switch", "slider", "stepper", "choice", "field", "button"),
            "hints" to listOf("density", "aspect", "tone", "live", "featured"),
            "width" to w, "height" to h, "dpi" to m.densityDpi,
            "widthClass" to if (w < 600) "compact" else if (w < 840) "medium" else "expanded",
            "heightClass" to if (h < 480) "compact" else if (h < 900) "medium" else "expanded",
            "input" to listOf("touch"), "locale" to java.util.Locale.getDefault().toLanguageTag(),
            "reducedMotion" to Theme.reducedMotion(activity))
    }

    private fun onEvent(ev: Map<String, Any?>) {
        Log.i(tag, "Native UI event: ${Json.write(ev)}")
        if (!attached) return
        when (ev.str("type")) {
            "overflow" -> { NativeBridge.enqueue(FeArgs.command("get", "app")); client?.sync() }
            "disconnected" -> toast("Disconnected: ${ev.str("reason")}", "error")
            "rejected" -> toast("Refused: ${ev.str("reason")}", "error")
            "failed" -> toast(ev.str("reason"), "error")
            "password" -> {
                val wrong = ev.str("kind") == "wrong"
                val addr = ev.str("address")
                // the described prompt when there is one; otherwise (or when the
                // description did not come back to it) a native one, so a refused
                // join always ends at a password prompt with the reason
                main.postDelayed({
                    val modal = client?.modal
                    if (modal.isNullOrEmpty() && app.str("state") == "menu") passwordPrompt(addr, wrong)
                }, 700)
            }
        }
    }

    /** What the engine's state asks for; applied again when the first screen arrives. */
    private var wantVisible = false

    private fun setVisible(show: Boolean) {
        wantVisible = show
        val v = if (show && screens.isNotEmpty()) View.VISIBLE else View.GONE
        if (root.visibility != v) {
            root.visibility = v
            if (v == View.VISIBLE) { shownAt = SystemClock.elapsedRealtime(); root.requestFocus() }
        }
        // opaque: the engine draws nothing behind it and idles at cl_maxidlefps
        NativeBridge.enqueue(FeArgs.command("screen", if (v == View.VISIBLE) screens.lastOrNull() ?: "home" else "none", if (v == View.VISIBLE) "2" else "0"))
    }

    /** While the QuakeC menu is up on purpose (Advanced, a stopgap screen): a way back to the native one. */
    private fun showBackPill(show: Boolean) {
        if (!show) { backPill?.let { (it.parent as? ViewGroup)?.removeView(it) }; backPill = null; return }
        if (backPill != null) return
        val b = Kit.button(activity, "‹  Godnotic menu", primary = true) { NativeBridge.enqueue(FeArgs.command("menu", "native")) }
        val lp = FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START)
        lp.leftMargin = Theme.dp(activity, 16f); lp.topMargin = Theme.dp(activity, 12f)
        activity.addContentView(b, lp)
        backPill = b
    }

    // ---------------------------------------------------------------- install from a file

    private var pendingFile: Pair<String, Map<String, Any?>>? = null
    private var installSeen = ""

    /** `kind: file` (Mods > Install from file): the system picker, the file copied where the engine can read it. */
    private fun pickFile(node: String, action: Map<String, Any?>) {
        pendingFile = node to action
        // tests: `<basedir>/fe-test-install` lists zips (one per line, relative to the basedir);
        // the first one stands for the picker's choice and is taken off the list
        val hook = baseDir?.let { File(it, "fe-test-install") }
        if (hook != null && hook.isFile) {
            val lines = hook.readLines().map { it.trim() }.filter { it.isNotEmpty() }
            if (lines.isNotEmpty()) {
                hook.writeText(lines.drop(1).joinToString("\n"))
                onActivityResult(PICK_FILE, Activity.RESULT_OK, Intent().setData(Uri.fromFile(File(baseDir, lines[0]))))
                return
            }
        }
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
            .putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
        try { activity.startActivityForResult(i, PICK_FILE) } catch (e: Exception) { pendingFile = null; toast("No app can pick a file here", "error") }
    }

    /** From XonoticActivity.onActivityResult: true when it was the picker's. */
    fun onActivityResult(request: Int, result: Int, data: Intent?): Boolean {
        if (request != PICK_FILE) return false
        val (node, action) = pendingFile ?: return true
        pendingFile = null
        val uri = data?.data ?: return true
        Thread({
            val path = try { importFile(uri) } catch (e: Exception) { Log.w(tag, "Native UI: could not copy the chosen file", e); null }
            main.post {
                if (path == null) { toast("Could not read that file", "error"); return@post }
                // the install is reviewed before anything is written: the mods topic says when
                NativeBridge.enqueue(FeArgs.command("sub", "mods", "1"))
                client?.event("action", mapOf("node" to node, "action" to action.str("id"), "payload" to mapOf("path" to path)))
            }
        }, "godnotic-import").start()
        return true
    }

    private fun importFile(uri: Uri): String {
        var name = if (uri.scheme == "file") uri.lastPathSegment ?: "mod.zip" else "mod.zip"
        if (uri.scheme != "file") activity.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) c.getString(0)?.let { name = it }
        }
        // the file's stem names the mod when the archive has no top folder: keep it, plainly
        val ext = if (name.lowercase().endsWith(".pk3")) ".pk3" else ".zip"
        val stem = name.substringBeforeLast('.').lowercase().replace(Regex("[^a-z0-9_-]"), "_").take(48).ifEmpty { "mod" }
        // where ModImport (the QuakeC menu's picker) puts it too: no mod can reach it
        val dir = java.io.File(baseDir ?: activity.getExternalFilesDir(null) ?: activity.filesDir, "userdata/luastate/incoming")
        dir.mkdirs()
        val out = java.io.File(dir, stem + ext)
        var total = 0L
        activity.contentResolver.openInputStream(uri)!!.use { input ->
            out.outputStream().use { o ->
                val buf = ByteArray(64 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    total += n
                    if (total > 8L * 1024 * 1024) throw java.io.IOException("over 8 MB, the engine's limit for a package")
                    o.write(buf, 0, n)
                }
            }
        }
        return out.absolutePath
    }

    /** The install review (`_lua_install`): what the package is and asks for, then Install or Cancel. */
    private fun onMods(m: Map<String, Any?>) {
        val raw = m.str("install")
        if (raw == installSeen) return
        installSeen = raw
        val kv = LinkedHashMap<String, String>()
        val perms = ArrayList<String>()
        for (line in raw.split('\n')) {
            val f = line.split('\t')
            if (f.size < 2) continue
            if (f[0] == "perm" && f.size >= 4) perms.add("${if (f[2] == "kept") "✓" else "•"} ${f[1]}: ${f[3]}${if (f[2] == "new") " (new)" else ""}")
            else kv[f[0]] = f[1]
        }
        when (kv["state"]) {
            "review" -> {
                val body = buildString {
                    append(listOf(kv["version"], kv["author"]).filter { !it.isNullOrEmpty() }.joinToString(" · "))
                    kv["description"]?.takeIf { it.isNotEmpty() }?.let { append("\n\n").append(it) }
                    append("\n\n").append(kv["files"] ?: "")
                    if (perms.isNotEmpty()) append("\n\nAsks for:\n").append(perms.joinToString("\n"))
                    append("\n\nIt is installed turned off, with nothing granted.")
                }
                confirmOrCancel(mapOf("title" to (if (kv["update"] == "1") "Update ${kv["title"]}?" else "Install ${kv["title"]}?"), "body" to body.trim(), "ok" to "Install"),
                    ok = { NativeBridge.enqueue(FeArgs.command("mod", "confirm")) },
                    cancel = { NativeBridge.enqueue(FeArgs.command("mod", "cancel")) })
            }
            "done" -> { toast(kv["message"]?.ifEmpty { null } ?: "Installed", "success"); NativeBridge.enqueue(FeArgs.command("sub", "mods", "0")) }
            "error" -> { toast(kv["message"]?.ifEmpty { null } ?: "Could not install it", "error"); NativeBridge.enqueue(FeArgs.command("sub", "mods", "0")) }
        }
    }

    // ---------------------------------------------------------------- UiEvents (from the views)

    override fun action(node: String, action: Map<String, Any?>) {
        if (stopgap?.action(node, action) == true) return
        client?.event("action", mapOf("node" to node, "action" to action.str("id")))
    }
    override fun value(node: String, value: Any?) { client?.event("value", mapOf("node" to node, "value" to value)) }
    override fun submit(node: String, value: Any?) { client?.event("submit", mapOf("node" to node, "value" to value)) }
    override fun row(list: String, key: String, kind: String) { client?.event("row", mapOf("node" to list, "row" to key, "kind" to kind)) }
    override fun window(list: String, source: String, offset: Int, limit: Int, sort: String, desc: Boolean, filter: String) {
        if (testRows) {
            // test builds: 2,000 servers / 200 maps generated here, to measure flings (dumpsys gfxinfo)
            val total = if (source.startsWith("maps")) 200 else 2000
            val rows = (offset until minOf(offset + limit, total)).map { TestRows.make(source, it) }
            main.post { renderer?.listRows(source, offset, total, rows) }
            return
        }
        client?.window(source, offset, limit, sort, desc, filter)
    }

    /** `<basedir>/fe-test-rows`: lists get generated rows instead of the engine's (tests only). */
    private val testRows = baseDir != null && File(baseDir, "fe-test-rows").isFile
    override fun image(ref: String, w: Int, h: Int) {
        when {
            ref == "asset:banner" -> renderer?.imageReady(ref, bannerFile())
            ref.startsWith("asset:") -> renderer?.imageReady(ref, "")
            app.bool("ui_bridge") -> client?.image(ref, w, h)
            else -> NativeBridge.enqueue(FeArgs.command("image", ref)) // engine: copy out of the packs
        }
    }
    override fun back() {
        if (dialogLayer.visibility == View.VISIBLE) { showDialog(null); return }
        if (stopgap?.back() == true) return
        client?.event("back", emptyMap())
    }

    private fun bannerFile(): String {
        val f = File(activity.cacheDir, "fe-banner.png")
        if (!f.isFile) try { activity.assets.open("xt/godnotic-banner.png").use { i -> f.outputStream().use { i.copyTo(it) } } } catch (e: Exception) { return "" }
        return f.path
    }

    // ---------------------------------------------------------------- platform state for the engine

    private fun reportPower(cvar: String, value: String) {
        if (!engineSeen || reported[cvar] == value) return
        if (NativeBridge.enqueue(FeArgs.command("set", cvar, value))) reported[cvar] = value
    }

    private fun reportAll() {
        val pm = activity.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        reportPower("touch_device_powersave", if (pm.isPowerSaveMode) "1" else "0")
        reportPower("touch_device_charging", if (charging()) "1" else "0")
        if (Build.VERSION.SDK_INT >= 29) reportPower("touch_device_thermal", pm.currentThermalStatus.toString())
    }

    private var powerReceiver: BroadcastReceiver? = null
    private var thermalListener: Any? = null

    private fun watchPower() {
        val pm = activity.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        powerReceiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                reportPower("touch_device_powersave", if (pm.isPowerSaveMode) "1" else "0")
                reportPower("touch_device_charging", if (charging()) "1" else "0")
            }
        }
        try {
            val f = IntentFilter(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)
            f.addAction(Intent.ACTION_POWER_CONNECTED)
            f.addAction(Intent.ACTION_POWER_DISCONNECTED)
            activity.registerReceiver(powerReceiver, f)
        } catch (e: Exception) { powerReceiver = null }
        if (Build.VERSION.SDK_INT >= 29) {
            val l = PowerManager.OnThermalStatusChangedListener { s -> reportPower("touch_device_thermal", s.toString()) }
            pm.addThermalStatusListener(activity.mainExecutor, l)
            thermalListener = l
        }
    }

    /** Plugged in (AC, USB, wireless): touch_warm then runs fully under battery saver. */
    private fun charging(): Boolean {
        val bm = activity.getSystemService(Context.BATTERY_SERVICE) as? android.os.BatteryManager
        if (Build.VERSION.SDK_INT >= 23 && bm != null && bm.isCharging) return true
        val st = try { activity.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) } catch (e: Exception) { null }
        return (st?.getIntExtra(android.os.BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
    }

    fun onResume() { resumed = true; if (attached) NativeBridge.frontendBeat() }
    fun onPause() { resumed = false }

    fun onDestroy() {
        NativeBridge.onDirty = null
        main.removeCallbacks(tick)
        client?.close()
        powerReceiver?.let { try { activity.unregisterReceiver(it) } catch (e: Exception) {} }
        if (Build.VERSION.SDK_INT >= 29) (thermalListener as? PowerManager.OnThermalStatusChangedListener)?.let {
            (activity.getSystemService(Context.POWER_SERVICE) as? PowerManager)?.removeThermalStatusListener(it)
        }
    }

    private fun applyInsets(insets: WindowInsets) {
        var l = 0; var t = 0; var r = 0; var b = 0
        if (Build.VERSION.SDK_INT >= 28) insets.displayCutout?.let { c -> l = c.safeInsetLeft; t = c.safeInsetTop; r = c.safeInsetRight; b = c.safeInsetBottom }
        if (Build.VERSION.SDK_INT >= 30) b = maxOf(b, insets.getInsets(WindowInsets.Type.ime()).bottom)
        // the screens keep clear of cutouts; the pinned bottom bar rides above the keyboard
        screenLayer.setPadding(l, t, r, b)
        dialogLayer.setPadding(l, t, r, b)
    }

    // ---------------------------------------------------------------- input

    /** From XonoticActivity.dispatchKeyEvent: true when the overlay took the key. */
    fun dispatchKey(event: KeyEvent): Boolean {
        if (root.visibility != View.VISIBLE) return false
        val k = event.keyCode
        if (k == KeyEvent.KEYCODE_VOLUME_UP || k == KeyEvent.KEYCODE_VOLUME_DOWN || k == KeyEvent.KEYCODE_VOLUME_MUTE) return false
        if (k == KeyEvent.KEYCODE_BACK || k == KeyEvent.KEYCODE_ESCAPE) {
            if (event.action == KeyEvent.ACTION_UP) try { back() } catch (e: Exception) { fail("back", e) }
            return true
        }
        root.dispatchKeyEvent(event)
        return true // never to the engine while a native screen is up
    }

    // ---------------------------------------------------------------- ViewRenderer.Host

    override fun showScreen(view: View, id: String, transition: String) {
        val old = if (screenLayer.childCount > 0) screenLayer.getChildAt(screenLayer.childCount - 1) else null
        renderer?.let { r -> screens.lastOrNull()?.let { r.saveScroll(it) } }
        if (transition != "push" && screens.isNotEmpty()) screens.removeAt(screens.size - 1)
        screens.remove(id)
        screens.add(id)
        hideKeyboard()
        (view.parent as? ViewGroup)?.removeView(view)
        screenLayer.addView(view, match())
        if (old != null && old !== view && old !== skeleton) {
            if (Theme.reducedMotion(activity)) screenLayer.removeView(old)
            else {
                // a short fade of the new screen in; it takes taps at once
                view.alpha = 0f
                view.animate().alpha(1f).setDuration(110).withEndAction { screenLayer.removeView(old) }.start()
            }
        }
        if (wantVisible && root.visibility != View.VISIBLE) setVisible(true)
        if (skeleton != null) {
            // the one deliberate transition: Home fades in over the banner
            if (!Theme.reducedMotion(activity)) { view.alpha = 0f; view.animate().alpha(1f).setDuration(180).start() }
            val sk = skeleton
            skeleton = null
            main.postDelayed({ screenLayer.removeView(sk) }, 200)
            Log.i(tag, "Native UI: skeleton replaced by $id after ${SystemClock.elapsedRealtime() - started} ms")
        }
        Log.i(tag, "Native UI: screen $id ($transition) ${SystemClock.elapsedRealtime() - started} ms after onCreate")
    }

    override fun showDialog(view: View?) {
        dialogLayer.removeAllViews()
        if (view == null) { dialogLayer.visibility = View.GONE; hideKeyboard(); return }
        dialogLayer.addView(view, match())
        dialogLayer.visibility = View.VISIBLE
        root.visibility = View.VISIBLE
        view.requestFocus()
    }

    override fun toast(text: String, kind: String) {
        val t = Kit.text(activity, text, Theme.SP_BODY, Theme.TEXT, bold = true)
        val p = Theme.dp(activity, 16f)
        t.setPadding(p, Theme.dp(activity, 12f), p, Theme.dp(activity, 12f))
        t.background = Theme.rounded(0xF20B1730.toInt(), Theme.dp(activity, 14f).toFloat(), Theme.dp(activity, 2f),
            when (kind) { "error" -> Theme.BAD; "success" -> Theme.GOOD; "warning" -> Theme.WARN; else -> Theme.BLUE })
        t.accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
        toastLayer.addView(t, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply {
            bottomMargin = Theme.dp(activity, 32f)
        })
        main.postDelayed({ toastLayer.removeView(t) }, 3500)
    }

    override fun canBack(): Boolean = client?.canBack ?: false

    override fun reserved(): List<Map<String, Any?>> = client?.reserved ?: emptyList()

    /**
     * A touch in a reserved rect (the engine's console button) while the game
     * shows through the pause scrim: the overlay lets it go to the engine.
     */
    fun passThrough(xPx: Float, yPx: Float): Boolean {
        if (client?.topKind != "game" || dialogLayer.visibility == View.VISIBLE) return false
        val d = activity.resources.displayMetrics.density
        return reserved().any { r ->
            val x = xPx / d; val y = yPx / d
            x >= r.num("x") && x < r.num("x") + r.num("w") && y >= r.num("y") && y < r.num("y") + r.num("h")
        }
    }

    override fun platformAction(node: String, action: Map<String, Any?>): Boolean {
        when (action.str("kind")) {
            "file" -> { pickFile(node, action); return true }
            "link" -> {
                val url = action.str("url")
                if (!url.startsWith("https://") && !url.startsWith("http://")) return true
                confirm(mapOf("title" to "Open ${Uri.parse(url).host}?", "body" to url, "ok" to "Open")) {
                    try { activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addCategory(Intent.CATEGORY_BROWSABLE)) }
                    catch (e: Exception) { toast("No app can open $url", "error") }
                }
                return true
            }
            "share" -> {
                val send = Intent(Intent.ACTION_SEND).setType("text/plain").putExtra(Intent.EXTRA_TEXT, action.str("text"))
                try { activity.startActivity(Intent.createChooser(send, action.str("label", "Share"))) } catch (e: Exception) {}
                return true
            }
            "copy" -> {
                (activity.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager)?.setPrimaryClip(ClipData.newPlainText("Godnotic", action.str("text")))
                toast("Copied", "success")
                return true
            }
        }
        return false
    }

    override fun confirm(confirm: Map<String, Any?>, ok: () -> Unit) = confirmOrCancel(confirm, ok) {}

    private fun confirmOrCancel(confirm: Map<String, Any?>, ok: () -> Unit, cancel: () -> Unit) {
        val col = Kit.column(activity, 14f)
        col.addView(Kit.text(activity, confirm.str("title"), Theme.SP_HEADLINE, Theme.TEXT, bold = true))
        confirm.str("body").takeIf { it.isNotEmpty() }?.let { col.addView(Kit.text(activity, it, Theme.SP_BODY, Theme.TEXT_MUTED)) }
        val actions = Kit.row(activity, 12f)
        actions.gravity = Gravity.END
        actions.addView(Kit.button(activity, confirm.str("cancel", "Cancel")) { showDialog(null); cancel() })
        val okB = Kit.button(activity, confirm.str("ok", "OK"), primary = true) { showDialog(null); ok() }
        if (confirm.bool("destructive")) okB.setTextColor(0xFF3A0000.toInt())
        actions.addView(okB)
        col.addView(actions)
        val frame = FrameLayout(activity)
        frame.setBackgroundColor(0xB3000000.toInt())
        frame.isClickable = true
        frame.setOnClickListener { showDialog(null); cancel() }
        val box = Kit.card(activity, clickable = false, radiusDp = 24f)
        box.isClickable = true
        val p = Theme.dp(activity, 24f)
        box.setPadding(p, p, p, p)
        box.addView(col)
        frame.addView(box, FrameLayout.LayoutParams(Theme.dp(activity, 560f), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        showDialog(frame)
    }

    /** The join password prompt: the password goes to the engine's private cvar through `fe password`, never kept here. */
    private fun passwordPrompt(addr: String, wrong: Boolean) {
        val col = Kit.column(activity, 14f)
        col.addView(Kit.text(activity, if (wrong) "Wrong password" else "Password needed", Theme.SP_HEADLINE, if (wrong) Theme.BAD else Theme.TEXT, bold = true).apply { typeface = Theme.display })
        col.addView(Kit.text(activity, if (wrong) "$addr refused that password. Try again." else "$addr needs a password to join.", Theme.SP_BODY, Theme.TEXT_MUTED))
        var field: android.widget.EditText? = null
        val join = {
            val pw = field?.text?.toString() ?: ""
            field?.setText("")
            if (pw.isNotEmpty()) {
                showDialog(null)
                NativeBridge.enqueue(FeArgs.command("password", addr, pw))
            }
        }
        val f = Kit.input(activity, "", "Password", password = true) { }
        f.setOnEditorActionListener { _, a, _ -> if (a == android.view.inputmethod.EditorInfo.IME_ACTION_DONE) { join(); true } else false }
        field = f
        col.addView(f, android.widget.LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        col.addView(Kit.text(activity, "Sent to this game's host only, as a proof, never as text.", Theme.SP_CAPTION, Theme.TEXT_DIM))
        val actions = Kit.row(activity, 12f)
        actions.gravity = Gravity.END
        actions.addView(Kit.button(activity, "Cancel") { showDialog(null) })
        actions.addView(Kit.button(activity, "Join", primary = true, iconName = "play") { join() })
        col.addView(actions)
        val frame = FrameLayout(activity)
        frame.setBackgroundColor(0xB3000000.toInt())
        frame.isClickable = true
        val box = Kit.card(activity, clickable = false, radiusDp = 24f)
        box.isClickable = true
        val p = Theme.dp(activity, 24f)
        box.setPadding(p, p, p, p)
        box.addView(col)
        frame.addView(box, FrameLayout.LayoutParams(Theme.dp(activity, 600f), ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        showDialog(frame)
        f.requestFocus()
    }

    private fun hideKeyboard() {
        val imm = activity.getSystemService(Context.INPUT_METHOD_SERVICE) as? android.view.inputmethod.InputMethodManager ?: return
        imm.hideSoftInputFromWindow(root.windowToken, 0)
    }

    // ---------------------------------------------------------------- join links

    /** A godnotic://join/<host>:<port> link: a native confirmation, then `fe connect`. */
    private var pendingJoin: String? = null

    fun handleJoinLink(uri: Uri?): Boolean {
        val addr = joinAddress(uri) ?: return false
        // at a cold start the question waits for Home: the first screen would cover it
        if (skeleton != null) { pendingJoin = addr; return true }
        askJoin(addr)
        return true
    }

    private fun askJoin(addr: String) {
        // in a match the overlay is hidden: open the pause menu first, then ask over it
        if (app.str("state") == "ingame") {
            NativeBridge.enqueue(FeArgs.command("menu", "open"))
            main.postDelayed({ askJoinNow(addr) }, 600)
            return
        }
        askJoinNow(addr)
    }

    private fun askJoinNow(addr: String) {
        main.post {
            confirm(mapOf("title" to "Join this game?", "body" to addr, "ok" to "Join")) {
                NativeBridge.enqueue(FeArgs.command("connect", addr))
            }
        }
    }

    companion object {
        const val PICK_FILE = 0x6d6f64
        fun joinAddress(uri: Uri?): String? {
            if (uri == null || uri.scheme != "godnotic" || uri.host != "join") return null
            return parseJoin(uri.path?.trimStart('/') ?: "")
        }

        /** host:port, [v6]:port or a bare host (port 26000); letters, digits, . : [ ] - only. */
        fun parseJoin(s: String): String? {
            val t = s.trim().removePrefix("godnotic://join/").trimEnd('/')
            if (t.isEmpty() || t.length > 255) return null
            if (!t.all { it.isLetterOrDigit() || it in ".:[]-" }) return null
            val port = when {
                t.startsWith("[") -> t.substringAfter("]", "").removePrefix(":")
                t.count { it == ':' } == 1 -> t.substringAfter(':')
                else -> ""
            }
            if (port.isNotEmpty() && (port.toIntOrNull() ?: 0) !in 1..65535) return null
            return if (port.isEmpty() && !t.startsWith("[") && t.count { it == ':' } == 0) "$t:26000" else t
        }
    }
}

/**
 * STOPGAP until the engine side of the UI bridge (touch_uib*.c, work/uibridge)
 * describes the screens: the bridge's own Home and pause descriptions,
 * shipped as assets (fe/home.json, fe/pause.json), played into the
 * renderer, with their actions mapped to engine commands. What the native
 * screens do not cover yet opens the QuakeC screen (`fe menu qcmenu`), with
 * a "Godnotic menu" pill to come back.
 */
class StopgapActions(private val activity: Activity, private val client: UibClient) {
    private var current = ""

    companion object {
        /** What the stopgap Home's Play starts (and touch_warm prepares while Home is idle). */
        const val STOPGAP_MAP = "stormkeep"
    }

    private fun load(name: String): Map<String, Any?> = try {
        Json.parse(activity.assets.open("fe/$name.json").bufferedReader().readText()).obj()
    } catch (e: Exception) { Log.w("Godnotic", "Native UI: no fe/$name.json: ${e.message}"); emptyMap() }

    fun show(name: String, app: Map<String, Any?> = emptyMap()) {
        if (name == current && name != "pause") return
        val snap = load(name)["snapshot"].obj()
        if (snap.isEmpty()) return
        val nodes = snap["nodes"].list().map { n ->
            val m = n.obj()
            if (name == "pause" && m.str("id") == "pause/match") {
                // the real match, not the description's example
                val map = app.str("map")
                m + ("props" to (m["props"].obj() + mapOf(
                    "title" to map.replaceFirstChar { it.uppercase() },
                    "subtitle" to if (app.bool("local")) (if (app.bool("hosting")) "Hosting a LAN game" else "Local game") else app.str("server"),
                    "image" to "pk3:maps/$map", "meta" to emptyList<Any>())))
            } else if (name == "pause" && m.str("id") in setOf("pause/actions/spectate", "pause/actions/team")) {
                m + ("props" to (m["props"].obj() + ("hidden" to true)))
            } else m
        }
        client.receive(snap + ("nodes" to nodes))
        current = name
    }

    /** True when the action was handled here. */
    fun action(node: String, a: Map<String, Any?>): Boolean {
        val id = a.str("id")
        Log.i("Godnotic", "Native UI action: $node / $id")
        when (id) {
            "play", "quick" -> NativeBridge.enqueue(FeArgs.command("play", "dm", STOPGAP_MAP, "4", "3"))
            "quit" -> NativeBridge.enqueue(FeArgs.command("menu", "quit"))
            "resume" -> NativeBridge.enqueue(FeArgs.command("menu", "resume"))
            "disconnect" -> NativeBridge.enqueue(FeArgs.command("menu", "leave"))
            // not native yet: the QuakeC screens, with a way back
            "sp" -> NativeBridge.enqueue(FeArgs.command("menu", "qcmenu"))
            "mp" -> NativeBridge.enqueue(FeArgs.command("menu", "qcmenu", "servers"))
            "settings" -> NativeBridge.enqueue(FeArgs.command("menu", "qcmenu", "settings"))
            "credits" -> NativeBridge.enqueue(FeArgs.command("menu", "qcmenu", "Credits"))
            else -> return false
        }
        if (id in setOf("sp", "mp", "settings", "credits")) current = ""
        return true
    }

    fun back(): Boolean {
        if (current == "pause") { NativeBridge.enqueue(FeArgs.command("menu", "resume")); return true }
        return current == "home" // Home: Back does nothing (Quit is on screen)
    }
}

/** Deterministic rows for the fling measurements (fe-test-rows). */
object TestRows {
    private val maps = listOf("stormkeep", "afterslime", "atelier", "boil", "bromine", "catharsis", "courtfun", "dance", "drain", "erbium", "finalrage", "fuse", "glowplant", "implosion", "oilrig", "silentsiege", "solarium", "techassault", "vorix", "warfare")
    private val modes = listOf("dm", "tdm", "ctf", "ca", "ft", "duel", "kh", "race")
    fun make(source: String, i: Int): Map<String, Any?> {
        val m = maps[i % maps.size]
        return if (source.startsWith("maps")) mapOf("key" to "$m-$i", "image" to "pk3:maps/$m",
            "cells" to mapOf("name" to m, "title" to m.replaceFirstChar { it.uppercase() } + " " + (i / maps.size + 1), "author" to "test", "modes" to "dm tdm ctf"))
        else mapOf("key" to "10.${i / 250}.${i % 250}.7:26000",
            "cells" to mapOf("name" to "Server ${i + 1}", "map" to m, "mode" to modes[i % modes.size], "players" to i % 17, "max" to 16 + (i % 3) * 8,
                "bots" to 0, "ping" to 20 + (i * 37) % 260, "lock" to (i % 9 == 0), "fav" to false, "address" to "10.${i / 250}.${i % 250}.7:26000",
                "section" to if (i < 12) "Favourites and LAN" else "Internet"))
    }
}
