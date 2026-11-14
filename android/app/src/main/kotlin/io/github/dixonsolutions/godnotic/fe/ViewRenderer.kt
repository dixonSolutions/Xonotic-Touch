package io.github.dixonsolutions.godnotic.fe

import android.content.Context
import android.graphics.Color
import android.os.Parcelable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import kotlin.math.max

/**
 * Draws UI bridge nodes (docs/UI_BRIDGE.md, section 4) with platform views
 * styled by Theme and Kit. One [Entry] per node id: its view, the group its
 * children go in, and its props, so a diff rebinds only that node.
 *
 * Supported: screen (sticky header with title, back and actions; `toolbar`
 * pinned under it; one scrolling body; `actionbar` or the screen's
 * `primary` pinned at the bottom), section (sticky headers), toolbar,
 * actionbar, row, grid (horizontal carousels too), tabs/pane, split, form,
 * card, hero, list (RecyclerView: table or cards; windows on demand),
 * preview, text, image, divider, status, progress, switch, slider, stepper,
 * choice (segmented chips or a sheet), field (password with a lock),
 * button. colour and keybind render as a generic row with their value; any
 * other type as a generic row (title, subtitle, actions, then its
 * children), per section 2.
 */
class ViewRenderer(
    private val ctx: Context,
    private val events: UiEvents,
    private val host: Host,
) : UiRenderer {

    /** Where screens, dialogs and toasts go, and the platform's actions (FrontendHost). */
    interface Host {
        fun showScreen(view: View, id: String, transition: String)
        fun showDialog(view: View?)
        fun toast(text: String, kind: String)
        fun canBack(): Boolean
        /** link / share / copy actions handled natively; true when handled. */
        fun platformAction(node: String, action: Map<String, Any?>): Boolean
        fun confirm(confirm: Map<String, Any?>, ok: () -> Unit)
        /** `app.reserved` rects (dp) while the engine's own controls show through (4.12); empty otherwise. */
        fun reserved(): List<Map<String, Any?>> = emptyList()
    }

    class Entry(val id: String, val type: String, val view: View, val children: ViewGroup?, var props: Map<String, Any?>) {
        var parent: String? = null
        var bind: ((Entry) -> Unit)? = null
        var top: ViewGroup? = null
        var bottom: ViewGroup? = null
        var fill: ViewGroup? = null
        var scroller: ScrollView? = null
        var body: LinearLayout? = null
        var list: VirtualList? = null
        var source: String = ""
        var empty: LinearLayout? = null
        /** tabs: the strip (pinned when the tabs are a screen's child) */
        var strip: View? = null
    }

    private val nodes = HashMap<String, Entry>()
    private val lists = HashMap<String, MutableList<VirtualList>>() // by source
    private val saved = HashMap<String, MutableMap<String, Any?>>()
    private val imagePaths = HashMap<String, String>()
    private val imageWaiters = HashMap<String, MutableList<(String) -> Unit>>()
    private fun dp(v: Float) = Theme.dp(ctx, v)

    fun entry(id: String): Entry? = nodes[id]
    val size: Int get() = nodes.size

    // ------------------------------------------------------------------ operations

    override fun create(id: String, parent: String?, index: Int, type: String, props: Map<String, Any?>, hints: Map<String, Any?>) {
        nodes[id]?.let { remove(id) }
        // a child of Home's bar is drawn as one of its joined segments (a button)
        val inTabBar = parent?.let { nodes[it] }?.type == "tabbar"
        val e = try { if (inTabBar) tab(id, props) else build(id, type, props) } catch (ex: RuntimeException) { generic(id, type, props) }
        e.parent = parent
        nodes[id] = e
        bindSafe(e)
        e.view.visibility = if (props.bool("hidden")) View.GONE else View.VISIBLE
        val p = parent?.let { nodes[it] } ?: return
        if (p.type == "tabs") { tabsAdd(p, e); return }
        // Home's bar of tabs: the screen's last strip, under the action bar
        if (p.type == "screen" && type == "tabbar") { (p.view as? ViewGroup)?.addView(e.view, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)); return }
        if (p.type == "split") { splitAdd(p, e); return }
        val place = UiLayout.placement(p.type, type, hints, props)
        val group = when (place) {
            Placement.TOP -> p.top
            Placement.BOTTOM -> p.bottom
            Placement.FILL -> p.children?.also { switchToFill(p) }
            Placement.BODY -> p.children
        } ?: p.children ?: return
        val lp = layoutFor(group, e, place == Placement.FILL)
        if (index < 0 || index > group.childCount) group.addView(e.view, lp) else group.addView(e.view, index, lp)
        if (group === p.top || group === p.bottom) group.visibility = View.VISIBLE
        if (type == "tabs" && p.type == "screen" && e.strip != null) { p.top?.addView(e.strip); p.top?.visibility = View.VISIBLE }
        else if (type == "tabs" && e.strip != null) group.addView(e.strip, group.indexOfChild(e.view))
        if (type == "list" && place == Placement.BODY) nestList(e)
    }

    override fun update(id: String, props: Map<String, Any?>) {
        val e = nodes[id] ?: return
        e.props = props
        bindSafe(e)
        e.view.visibility = if (props.bool("hidden")) View.GONE else View.VISIBLE
    }

    private fun bindSafe(e: Entry) {
        try { e.bind?.invoke(e) } catch (ex: RuntimeException) { android.util.Log.w("Godnotic", "Native UI: ${e.type} ${e.id} did not bind: ${ex.message}") }
    }

    override fun remove(id: String) {
        val e = nodes.remove(id) ?: return
        (e.view.parent as? ViewGroup)?.removeView(e.view)
        (e.strip?.parent as? ViewGroup)?.removeView(e.strip)
        nodes.values.filter { it.parent == id }.map { it.id }.forEach { remove(it) }
        e.list?.let { l -> lists[e.source]?.remove(l) }
    }

    override fun navigate(screen: String, transition: String) {
        val e = nodes[screen] ?: return
        (e.view.parent as? ViewGroup)?.removeView(e.view)
        bindSafe(e) // the back arrow follows app.canBack
        host.showScreen(e.view, screen, transition)
        restoreScroll(e)
    }

    override fun dialog(id: String?) {
        if (id == null) { host.showDialog(null); return }
        val e = nodes[id] ?: return
        (e.view.parent as? ViewGroup)?.removeView(e.view)
        host.showDialog(dialogFrame(e.view, e.props.bool("dismissable", true)))
    }

    override fun toast(text: String, kind: String) = host.toast(text, kind)

    override fun listRows(source: String, offset: Int, total: Int, rows: List<Map<String, Any?>>) {
        for (l in lists[source].orEmpty()) {
            if (rows.isEmpty() && offset == 0) l.reset(total) else l.rows(offset, total, rows)
            nodes[l.id]?.let { bindEmpty(it) }
        }
    }

    override fun imageReady(ref: String, path: String) {
        imagePaths[ref] = path
        imageWaiters.remove(ref)?.forEach { it(path) }
    }

    fun saveScroll(screen: String) {
        val e = nodes[screen] ?: return
        val m = saved.getOrPut(screen) { HashMap() }
        e.scroller?.let { m["body"] = it.scrollY }
        for (l in lists.values.flatten()) if (isUnder(l.id, screen)) m[l.id] = l.save()
    }

    private fun restoreScroll(e: Entry) {
        val m = saved[e.id] ?: return
        (m["body"] as? Int)?.let { y -> e.scroller?.post { e.scroller?.scrollTo(0, y) } }
        for (l in lists.values.flatten()) if (isUnder(l.id, e.id)) l.restore(m[l.id] as? Parcelable)
    }

    private fun isUnder(id: String, root: String): Boolean {
        var cur: String? = id
        while (cur != null) { if (cur == root) return true; cur = nodes[cur]?.parent }
        return false
    }

    // ------------------------------------------------------------------ build

    private fun build(id: String, type: String, props: Map<String, Any?>): Entry = when (type) {
        "screen" -> screen(id, props)
        "section" -> section(id, props)
        "toolbar" -> group(id, type, props, Kit.row(ctx, 10f).apply { setPadding(0, dp(4f), 0, dp(4f)) })
        "actionbar" -> group(id, type, props, Kit.row(ctx, 12f).apply { gravity = Gravity.END or Gravity.CENTER_VERTICAL })
        "row" -> if (props.bool("wrap")) group(id, type, props, FlowGrid(ctx, 0, dp(10f), wrap = true)) else labelledRow(id, props, Kit.row(ctx, 12f).apply {
            gravity = when (props.str("align")) { "center" -> Gravity.CENTER; "end" -> Gravity.END or Gravity.CENTER_VERTICAL; else -> Gravity.START or Gravity.CENTER_VERTICAL }
        })
        "split" -> split(id, props)
        "form", "pane", "column" -> group(id, type, props, Kit.column(ctx, 12f))
        "grid" -> grid(id, props)
        "tabs" -> tabs(id, props)
        "tabbar" -> tabbar(id, props)
        "card" -> card(id, props, hero = false)
        "hero" -> card(id, props, hero = true)
        "list" -> list(id, props)
        "preview" -> preview(id, props)
        "text" -> text(id, props)
        "image" -> image(id, props)
        "divider" -> Entry(id, type, View(ctx).apply { setBackgroundColor(Theme.OUTLINE) }, null, props)
        "status" -> status(id, props)
        "progress" -> progress(id, props)
        "switch" -> switchRow(id, props)
        "slider" -> sliderRow(id, props)
        "stepper" -> stepper(id, props)
        "choice" -> choice(id, props)
        "field" -> field(id, props)
        "button" -> button(id, props)
        else -> generic(id, type, props)
    }

    private fun layoutFor(g: ViewGroup, child: Entry, fill: Boolean): ViewGroup.LayoutParams {
        if (fill && g is LinearLayout) return LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        if (child.type == "divider") return LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, max(1, dp(1f)))
        if (g is LinearLayout && g.orientation == LinearLayout.HORIZONTAL && child.type == "field")
            return LinearLayout.LayoutParams(dp(380f), ViewGroup.LayoutParams.WRAP_CONTENT)
        if (g is LinearLayout && g.orientation == LinearLayout.HORIZONTAL) {
            val grow = child.type == "card" || child.type == "hero" || child.type == "row" || child.type == "tab" || child.props.str("width") == "fill"
            return LinearLayout.LayoutParams(if (grow) 0 else ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, if (grow) 1f else 0f)
        }
        if (g is LinearLayout) return LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        return ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
    }

    private fun title(p: Map<String, Any?>) = p.str("title").ifEmpty { p.str("label") }

    /** Invokes an action object: native link/share/copy, a confirm first if asked, then the event. */
    private fun invoke(node: String, a: Map<String, Any?>) {
        if (a.isEmpty() || (a.containsKey("enabled") && !a.bool("enabled"))) return
        val go = {
            if (a.str("kind") == "back") events.back()
            else if (!host.platformAction(node, a)) events.action(node, a)
        }
        val c = a["confirm"].obj()
        if (c.isNotEmpty()) host.confirm(c, go) else go()
    }

    private fun actionButton(node: String, a: Map<String, Any?>, primaryDefault: Boolean = false): TextView {
        val style = a.str("style")
        val b = Kit.button(ctx, a.str("label"), style == "primary" || (style.isEmpty() && primaryDefault), a.str("icon")) { invoke(node, a) }
        if (style == "destructive") b.setTextColor(Theme.BAD)
        b.isEnabled = !a.containsKey("enabled") || a.bool("enabled")
        b.contentDescription = a.str("label")
        return b
    }

    private fun screen(id: String, props: Map<String, Any?>): Entry {
        val root = Kit.column(ctx)
        root.setBackgroundColor(Theme.BG)
        val pad = dp(24f)
        // the header: back, title, the screen's actions; sticky (outside the scroller)
        val bar = Kit.row(ctx, 12f)
        bar.setPadding(pad, dp(10f), pad, dp(6f))
        bar.minimumHeight = dp(64f)
        val back = Kit.button(ctx, "‹") { events.back() }
        back.textSize = Theme.SP_HEADLINE
        back.minimumWidth = dp(56f)
        back.contentDescription = "Back"
        val titleV = Kit.text(ctx, "", Theme.SP_HEADLINE, Theme.TEXT, bold = true, lines = 1)
        titleV.typeface = Theme.display
        val sub = Kit.text(ctx, "", Theme.SP_CAPTION, Theme.TEXT_MUTED, lines = 1)
        val titles = Kit.column(ctx)
        titles.addView(titleV)
        titles.addView(sub)
        val trailing = Kit.row(ctx, 8f)
        bar.addView(back)
        bar.addView(titles, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        bar.addView(trailing)
        root.addView(bar)
        val top = Kit.column(ctx, 8f)
        top.setPadding(pad, 0, pad, dp(8f))
        top.visibility = View.GONE
        root.addView(top)
        val body = Kit.column(ctx, 18f)
        body.setPadding(pad, dp(8f), pad, dp(28f))
        val stickyHost = FrameLayout(ctx)
        val sv = PageScroll(ctx)
        sv.isFillViewport = true
        sv.isVerticalScrollBarEnabled = true
        sv.scrollBarStyle = View.SCROLLBARS_INSIDE_OVERLAY
        sv.overScrollMode = View.OVER_SCROLL_IF_CONTENT_SCROLLS
        sv.addView(body)
        stickyHost.addView(sv, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        val pinned = Kit.text(ctx, "", Theme.SP_TITLE, Theme.BLUE, bold = true)
        pinned.setBackgroundColor(0xF2000000.toInt())
        pinned.setPadding(pad + dp(4f), dp(8f), pad, dp(8f))
        pinned.visibility = View.GONE
        stickyHost.addView(pinned, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP))
        // a list scrolled to the top sits under the pinned section title
        pinned.measure(View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
        sv.topInset = pinned.measuredHeight
        root.addView(stickyHost, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val fill = FrameLayout(ctx)
        fill.visibility = View.GONE
        root.addView(fill, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        val bottom = Kit.row(ctx, 12f)
        bottom.gravity = Gravity.END or Gravity.CENTER_VERTICAL
        bottom.setPadding(pad, dp(10f), pad, dp(12f))
        bottom.setBackgroundColor(0xF2050B18.toInt())
        bottom.visibility = View.GONE
        root.addView(bottom)
        val e = Entry(id, "screen", root, body, props)
        e.top = top; e.bottom = bottom; e.fill = fill; e.scroller = sv; e.body = body
        // sticky section headers and the collapsing header
        sv.setOnScrollChangeListener { _, _, y, _, _ ->
            stickySection(e, pinned, y)
            if (e.props["header"].obj().bool("collapsible")) {
                val compact = y > dp(48f)
                titleV.textSize = if (compact) Theme.SP_TITLE else Theme.SP_HEADLINE
                sub.visibility = if (compact || sub.text.isEmpty()) View.GONE else View.VISIBLE
            }
        }
        e.bind = { x ->
            val p = x.props
            titleV.text = p.str("title")
            sub.text = p.str("subtitle")
            sub.visibility = if (sub.text.isEmpty()) View.GONE else View.VISIBLE
            val kindHome = p.str("kind") == "home"
            // a dialog or sheet screen sizes to its content inside the dialog box
            val modal = p.str("presentation") in setOf("dialog", "sheet")
            (stickyHost.layoutParams as? LinearLayout.LayoutParams)?.let { lp ->
                lp.height = if (modal) ViewGroup.LayoutParams.WRAP_CONTENT else 0; lp.weight = if (modal) 0f else 1f; stickyHost.layoutParams = lp
            }
            (sv.layoutParams as? FrameLayout.LayoutParams)?.let { lp -> lp.height = if (modal) ViewGroup.LayoutParams.WRAP_CONTENT else ViewGroup.LayoutParams.MATCH_PARENT; sv.layoutParams = lp }
            if (modal) { bar.setPadding(dp(8f), 0, dp(8f), dp(4f)); body.setPadding(dp(8f), dp(4f), dp(8f), dp(8f)); bottom.setPadding(dp(8f), dp(8f), dp(8f), dp(4f)) }
            // the pause menu sits on a scrim over the frozen game frame: the player sees the match
            root.setBackgroundColor(if (p.str("kind") == "game") 0xB3000000.toInt() else if (modal) Color.TRANSPARENT else Theme.BG)
            bar.visibility = if (p["header"].obj().bool("hidden")) View.GONE else View.VISIBLE
            titleV.visibility = if (kindHome) View.INVISIBLE else View.VISIBLE // Home shows the banner instead
            back.visibility = if (host.canBack() && !kindHome && !modal) View.VISIBLE else View.GONE
            if (p["header"].obj().bool("compact")) titleV.textSize = Theme.SP_TITLE
            // over the game (a scrim) the engine's console button shows through: the
            // header's actions keep clear of its rect plus 8 dp (docs/UI_BRIDGE.md 4.12)
            var endPad = pad
            if (p.str("kind") == "game") for (r in host.reserved()) {
                val wdp = ctx.resources.displayMetrics.widthPixels / ctx.resources.displayMetrics.density
                if (r.num("y") < 72.0) endPad = maxOf(endPad, dp((wdp - r.num("x")).toFloat() + 8f))
            }
            bar.setPadding(bar.paddingLeft, bar.paddingTop, endPad, bar.paddingBottom)
            trailing.removeAllViews()
            // header actions are quiet (Quit must not compete with Play)
            for (a in p["actions"].list()) { val m = a.obj(); trailing.addView(Kit.quiet(ctx, m.str("label"), m.str("icon")) { invoke(id, m) }) }
            // the screen's primary action sits in the bottom bar when there is no actionbar node
            bottom.findViewWithTag<View>("primary")?.let { bottom.removeView(it) }
            val prim = p["primary"].obj()
            if (prim.isNotEmpty()) bottom.addView(actionButton(id, prim, true).apply { tag = "primary" })
            bottom.visibility = if (bottom.childCount == 0) View.GONE else View.VISIBLE
            top.visibility = if (top.childCount == 0) View.GONE else View.VISIBLE
        }
        return e
    }

    /** The last sticky section whose header scrolled above the top stays pinned; the next one pushes it up. */
    private fun stickySection(screen: Entry, pinned: TextView, y: Int) {
        val body = screen.body ?: return
        var current: Entry? = null
        var nextTop = Int.MAX_VALUE
        for (i in 0 until body.childCount) {
            val v = body.getChildAt(i)
            val e = nodes.values.firstOrNull { it.view === v && it.type == "section" && it.props.bool("sticky") } ?: continue
            if (v.top <= y) current = e else { nextTop = minOf(nextTop, v.top - y); break }
        }
        if (current == null || current.view.bottom - y < pinned.height) { pinned.visibility = View.GONE; return }
        pinned.text = title(current.props)
        pinned.visibility = View.VISIBLE
        pinned.translationY = minOf(0, nextTop - pinned.height).toFloat()
    }

    /** A list in a screen: the body stops scrolling as a whole and the list takes the space left. */
    private fun switchToFill(screen: Entry) {
        val sv = screen.scroller ?: return
        val body = screen.body ?: return
        val fill = screen.fill ?: return
        if (body.parent === sv) {
            sv.removeView(body)
            (sv.parent as? View)?.visibility = View.GONE
            fill.addView(body, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            fill.visibility = View.VISIBLE
            body.setPadding(body.paddingLeft, body.paddingTop, body.paddingRight, dp(8f))
        }
    }

    /**
     * A list inside a scrolling page (the map picker in Create, a short list
     * in a section): not the page's scroller, so cards become one horizontal
     * row (a picker strip) and a table shows up to eight rows and scrolls
     * inside its frame.
     */
    private fun nestList(e: Entry) {
        val vl = e.list ?: return
        val lp = e.view.layoutParams
        if (e.props.str("display") == "cards" && fitRows(e.props) > 0) {
            // `rows: N` in a scrolling page: N rows that scroll sideways, as
            // tall as the page's visible area under the pinned section title,
            // so scrolled to them all N rows show whole
            val frame = (vl.view.layoutParams as? LinearLayout.LayoutParams)
            frame?.let { it.weight = 0f; it.height = ctx.resources.displayMetrics.heightPixels / 2; vl.view.layoutParams = it }
            lp?.height = ViewGroup.LayoutParams.WRAP_CONTENT
            e.view.layoutParams = lp
            var top: Entry? = e
            while (top != null && top.type != "screen") top = top.parent?.let { nodes[it] }
            top?.scroller?.addOnLayoutChangeListener { sv, _, t, _, b, _, _, _, _ ->
                val h = b - t - sv.paddingTop - sv.paddingBottom - ((sv as? PageScroll)?.topInset ?: 0)
                val l = vl.view.layoutParams
                if (h > dp(120f) && l != null && l.height != h) { l.height = h; vl.view.post { vl.view.layoutParams = l } }
            }
            return
        }
        if (e.props.str("display") == "cards") {
            val w = dp(e.props.num("minItem", 220.0).toFloat()) + dp(14f)
            vl.carousel(w)
            (vl.view.layoutParams as? LinearLayout.LayoutParams)?.let { it.height = ViewGroup.LayoutParams.WRAP_CONTENT; it.weight = 0f; vl.view.layoutParams = it }
            lp?.height = ViewGroup.LayoutParams.WRAP_CONTENT
        } else {
            val rows = minOf(8, maxOf(3, e.props.int("count", 5)))
            vl.view.isNestedScrollingEnabled = true
            lp?.height = dp(36f) + rows * dp(if (e.props.str("rowHeight") == "comfortable") 56f else 40f) + dp(2f)
        }
        e.view.layoutParams = lp
    }

    private fun fitRows(p: Map<String, Any?>) = listOf(p.int("rows"), p["style"].obj().int("rows"), p["hints"].obj().int("rows")).firstOrNull { it > 0 } ?: 0

    private fun fillChain(list: Entry) {
        var cur: Entry? = list
        while (cur != null) {
            val parent = cur.parent?.let { nodes[it] } ?: break
            (cur.view.layoutParams as? LinearLayout.LayoutParams)?.let { it.height = 0; it.weight = 1f; cur.view.layoutParams = it }
            (cur.view.layoutParams as? FrameLayout.LayoutParams)?.let { it.height = ViewGroup.LayoutParams.MATCH_PARENT; cur.view.layoutParams = it }
            if (parent.type == "screen") { switchToFill(parent); break }
            cur = parent
        }
    }

    private fun section(id: String, props: Map<String, Any?>): Entry {
        val box = Kit.column(ctx, 8f)
        val head = Kit.row(ctx, 12f)
        val titleV = Kit.text(ctx, "", Theme.SP_TITLE, Theme.BLUE, bold = true)
        titleV.typeface = Theme.display
        val link = Kit.text(ctx, "", Theme.SP_BODY, Theme.EMBER, bold = true)
        link.minHeight = dp(Kit.MIN_TARGET_DP)
        link.gravity = Gravity.CENTER_VERTICAL
        link.isClickable = true
        head.addView(titleV, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        head.addView(link)
        val sub = Kit.text(ctx, "", Theme.SP_CAPTION, Theme.TEXT_MUTED)
        val content = Kit.column(ctx, 10f)
        val foot = Kit.text(ctx, "", Theme.SP_CAPTION, Theme.TEXT_DIM)
        box.addView(head); box.addView(sub); box.addView(content); box.addView(foot)
        val e = Entry(id, "section", box, content, props)
        var collapsed = props.bool("collapsed")
        titleV.setOnClickListener { if (e.props.bool("collapsible")) { collapsed = !collapsed; content.visibility = if (collapsed) View.GONE else View.VISIBLE } }
        e.bind = { x ->
            val p = x.props
            titleV.text = title(p)
            head.visibility = if (titleV.text.isEmpty()) View.GONE else View.VISIBLE
            sub.text = p.str("subtitle"); sub.visibility = if (sub.text.isEmpty()) View.GONE else View.VISIBLE
            foot.text = p.str("footer"); foot.visibility = if (foot.text.isEmpty()) View.GONE else View.VISIBLE
            val l = p["link"].obj()
            link.text = if (l.isEmpty()) "" else l.str("label") + "  ›"
            link.visibility = if (l.isEmpty()) View.GONE else View.VISIBLE
            link.setOnClickListener { invoke(id, l) }
            content.visibility = if (collapsed) View.GONE else View.VISIBLE
        }
        return e
    }

    private fun group(id: String, type: String, props: Map<String, Any?>, g: ViewGroup) = Entry(id, type, g, g, props)

    /**
     * A row with a `label` (the LAN popup's "Wi-Fi", "VPN (tailscale0)", "Link"):
     * the label with its icon in a fixed column, then the children.
     */
    private fun labelledRow(id: String, props: Map<String, Any?>, inner: LinearLayout): Entry {
        val outer = Kit.row(ctx, 16f)
        outer.gravity = Gravity.CENTER_VERTICAL
        val label = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT_MUTED, bold = true, lines = 2)
        label.compoundDrawablePadding = dp(8f)
        outer.addView(label, LinearLayout.LayoutParams(dp(200f), ViewGroup.LayoutParams.WRAP_CONTENT))
        outer.addView(inner, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val e = Entry(id, "row", outer, inner, props)
        e.bind = { x ->
            val t = x.props.str("label")
            label.text = t
            label.setCompoundDrawablesRelative(Icons.drawable(x.props.str("icon"), dp(20f), Theme.TEXT_MUTED), null, null, null)
            label.visibility = if (t.isEmpty()) View.GONE else View.VISIBLE
            label.contentDescription = t
        }
        return e
    }

    /** Cards in as many columns as fit at minItem dp; scroll "horizontal" makes a carousel. */
    private fun grid(id: String, props: Map<String, Any?>): Entry {
        val gap = dp(16f)
        if (props.str("scroll") == "horizontal") {
            val hs = HorizontalScrollView(ctx)
            hs.isHorizontalScrollBarEnabled = false
            hs.overScrollMode = View.OVER_SCROLL_IF_CONTENT_SCROLLS
            val row = CarouselRow(ctx, dp(props.num("minItem", 260.0).toFloat()), gap)
            hs.addView(row)
            return Entry(id, "grid", hs, row, props)
        }
        val g = FlowGrid(ctx, dp(props.num("minItem", 160.0).toFloat()), gap)
        val e = Entry(id, "grid", g, g, props)
        // Home's cards: always side by side in one row, whatever the width (the
        // owner's rule); they shrink, and the description joins two when three
        // do not fit (`compact`)
        e.bind = { x -> val one = screenKind(x) == "home"; if (g.singleRow != one) { g.singleRow = one; g.requestLayout() } }
        return e
    }

    private fun tabs(id: String, props: Map<String, Any?>): Entry {
        val panes = FrameLayout(ctx)
        val holder = FrameLayout(ctx)
        val e = Entry(id, "tabs", panes, panes, props)
        e.strip = holder
        e.bind = { x -> tabsBind(x, holder) }
        return e
    }

    private fun tabsAdd(tabs: Entry, pane: Entry) {
        tabs.children?.addView(pane.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        bindSafe(tabs)
    }

    private fun tabsBind(x: Entry, holder: FrameLayout) {
        val sel = x.props.str("value")
        val list = nodes.values.filter { it.parent == x.id && it.type == "pane" }.sortedBy { (x.children)?.indexOfChild(it.view) ?: 0 }
        holder.removeAllViews()
        val opts = list.map { it.props.str("key") to Icons.label(it.props.str("icon"), it.props.str("label")) }
        // sub-pages: a strip that scrolls sideways when it does not fit
        val hs = HorizontalScrollView(ctx)
        hs.isHorizontalScrollBarEnabled = false
        hs.addView(Kit.segmented(ctx, opts, sel) { v ->
            x.props = x.props + ("value" to v)
            for (p in list) p.view.visibility = if (p.props.str("key") == v) View.VISIBLE else View.GONE
            events.value(x.id, v)
        })
        holder.addView(hs, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        for (p in list) p.view.visibility = if (p.props.str("key") == sel || (sel.isEmpty() && p === list.first())) View.VISIBLE else View.GONE
    }

    /**
     * List-detail (Settings: vertical tabs). The section list on the start
     * side scrolls on its own and stays put; the selected section's content
     * scrolls on the end side. On a compact width the list keeps only its icons.
     */
    private fun split(id: String, props: Map<String, Any?>): Entry {
        val row = Kit.row(ctx, 20f)
        row.gravity = Gravity.TOP
        val wdp = ctx.resources.displayMetrics.widthPixels / ctx.resources.displayMetrics.density
        val compact = wdp < 720
        val rail = Kit.column(ctx, 6f)
        val railScroll = ScrollView(ctx)
        railScroll.isVerticalScrollBarEnabled = true
        railScroll.scrollBarStyle = View.SCROLLBARS_INSIDE_OVERLAY
        railScroll.addView(rail)
        val panes = FrameLayout(ctx)
        val paneScroll = ScrollView(ctx)
        paneScroll.isFillViewport = true
        paneScroll.scrollBarStyle = View.SCROLLBARS_INSIDE_OVERLAY
        paneScroll.overScrollMode = View.OVER_SCROLL_IF_CONTENT_SCROLLS
        paneScroll.addView(panes)
        row.addView(railScroll, LinearLayout.LayoutParams(dp(if (compact) 76f else 300f), ViewGroup.LayoutParams.MATCH_PARENT))
        row.addView(paneScroll, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f))
        val e = Entry(id, "split", row, panes, props)
        e.strip = null
        e.bind = { x ->
            rail.removeAllViews()
            val sel = x.props.str("value")
            val list = nodes.values.filter { it.parent == x.id && it.type == "pane" }.sortedBy { panes.indexOfChild(it.view) }
            for (p in list) {
                val on = p.props.str("key") == sel
                val item = Kit.row(ctx, 14f)
                item.minimumHeight = dp(56f)
                item.setPadding(dp(14f), dp(8f), dp(14f), dp(8f))
                item.background = if (on) Theme.rounded(0xFF15306A.toInt(), dp(14f).toFloat(), dp(2f), Theme.BLUE) else Theme.surface(ctx, Color.TRANSPARENT, 14f, Color.TRANSPARENT)
                item.isClickable = true; item.isFocusable = true; item.isSelected = on
                val icon = View(ctx)
                icon.background = Icons.drawable(p.props.str("icon"), dp(24f), if (on) Theme.EMBER else Theme.BLUE)
                item.addView(icon, LinearLayout.LayoutParams(dp(24f), dp(24f)))
                if (!compact) {
                    val texts = Kit.column(ctx)
                    texts.addView(Kit.text(ctx, p.props.str("label"), Theme.SP_BODY, if (on) Color.WHITE else Theme.TEXT, bold = true, lines = 1))
                    p.props.str("subtitle").takeIf { it.isNotEmpty() }?.let { texts.addView(Kit.text(ctx, it, Theme.SP_CAPTION, Theme.TEXT_MUTED, lines = 1)) }
                    item.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
                }
                item.contentDescription = p.props.str("label") + if (on) ", selected" else ""
                item.setOnClickListener { v ->
                    Kit.haptic(v)
                    x.props = x.props + ("value" to p.props.str("key"))
                    bindSafe(x)
                    paneScroll.scrollTo(0, 0)
                    events.value(x.id, p.props.str("key"))
                }
                rail.addView(item)
            }
            for (p in list) p.view.visibility = if (p.props.str("key") == sel || (sel.isEmpty() && p === list.firstOrNull())) View.VISIBLE else View.GONE
        }
        return e
    }

    private fun splitAdd(split: Entry, pane: Entry) {
        split.children?.addView(pane.view, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        bindSafe(split)
    }

    private fun aspect(p: Map<String, Any?>, def: Float): Float {
        val parts = p.str("aspect").split(':')
        if (parts.size == 2) { val w = parts[0].toFloatOrNull(); val h = parts[1].toFloatOrNull(); if (w != null && h != null && h > 0) return w / h }
        return def
    }

    /** A map or server picture (shown as a photo) rather than artwork that stands for a section. */
    private fun isPhoto(ref: String) = ref.startsWith("pk3:maps/") || ref.startsWith("/") || ref.startsWith("mod:")

    private fun card(id: String, props: Map<String, Any?>, hero: Boolean): Entry {
        if (hero) return heroCard(id, props)
        val c = Kit.card(ctx, clickable = true, radiusDp = 16f)
        // photo cards: the picture on top; section cards: an icon slot beside the text
        val col = Kit.column(ctx)
        val img = RoundImage(ctx, 0f)
        val bar = ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000; progressTintList = android.content.res.ColorStateList.valueOf(Theme.EMBER); visibility = View.GONE
        }
        val row = Kit.row(ctx, 14f)
        row.setPadding(dp(16f), dp(14f), dp(16f), dp(14f))
        row.minimumHeight = dp(88f)
        val slot = FrameLayout(ctx)
        slot.background = Theme.rounded(0xFF0F2350.toInt(), dp(14f).toFloat(), max(1, dp(1f)), 0xFF2B4A86.toInt())
        val slotIcon = View(ctx)
        slot.addView(slotIcon, FrameLayout.LayoutParams(dp(32f), dp(32f), Gravity.CENTER))
        val texts = Kit.column(ctx, 4f)
        val titleV = Kit.text(ctx, "", Theme.SP_TITLE - 1f, Theme.TEXT, bold = true, lines = 1)
        titleV.typeface = Theme.display
        titleV.maxLines = 2
        titleV.ellipsize = null
        Kit.fitWords(titleV, Theme.SP_TITLE - 1f, 13f) // "Team Deathmatch" on a narrow card: smaller, never "Dea|thmatch"
        val subtitle = Kit.text(ctx, "", Theme.SP_CAPTION + 1f, Theme.TEXT_MUTED, lines = 2)
        val badges = FlowGrid(ctx, 0, dp(6f), wrap = true)
        val meta = FlowGrid(ctx, 0, dp(12f), wrap = true)
        val actions = FlowGrid(ctx, 0, dp(10f), wrap = true)
        texts.addView(badges); texts.addView(titleV); texts.addView(subtitle); texts.addView(meta); texts.addView(actions)
        row.addView(slot, LinearLayout.LayoutParams(dp(52f), dp(52f)))
        row.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val chevron = View(ctx)
        chevron.background = Icons.drawable("chevron", dp(24f), Theme.TEXT_DIM)
        row.addView(chevron, LinearLayout.LayoutParams(dp(24f), dp(24f)))
        col.addView(img, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        col.addView(bar, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(4f)))
        col.addView(row)
        c.addView(col)
        val e = Entry(id, "card", c, null, props)
        c.setOnClickListener { invoke(id, e.props["primary"].obj()) }
        e.bind = { x ->
            val p = x.props
            val ref = p.str("image")
            val photo = ref.isNotEmpty() && isPhoto(ref)
            titleV.text = title(p)
            // words are never broken: one line for one word (autosize shrinks it), two for more
            titleV.maxLines = if (title(p).trim().contains(' ')) 2 else 1
            subtitle.text = p.str("subtitle")
            subtitle.visibility = if (subtitle.text.isEmpty()) View.GONE else View.VISIBLE
            badges.removeAllViews()
            for (b in p["badges"].list()) badges.addView(badge(b.obj()))
            badges.visibility = if (badges.childCount == 0) View.GONE else View.VISIBLE
            meta.removeAllViews()
            for (m in p["meta"].list()) meta.addView(Kit.text(ctx, m.obj().str("text"), Theme.SP_CAPTION, Theme.TEXT, bold = true).also { Kit.icon(it, m.obj().str("icon"), Theme.BLUE, 16f) })
            meta.visibility = if (meta.childCount == 0) View.GONE else View.VISIBLE
            actions.removeAllViews()
            for (a in p["actions"].list()) actions.addView(actionButton(id, a.obj()))
            actions.visibility = if (actions.childCount == 0) View.GONE else View.VISIBLE
            // the icon slot: the bridge's icon as a vector; artwork only when there is no icon
            val icon = Icons.drawable(p.str("icon"), dp(32f), Theme.BLUE)
            slotIcon.background = icon
            slot.visibility = if (!photo && (icon != null)) View.VISIBLE else View.GONE
            chevron.visibility = if (photo || actions.childCount > 0 || nodes[x.parent ?: ""]?.type == "grid") View.GONE else View.VISIBLE
            img.aspect = aspect(p, 16f / 9f)
            img.maxHeight = (ctx.resources.displayMetrics.heightPixels * 0.22f).toInt()
            if (photo) { img.visibility = View.VISIBLE; picture(img, ref) } else img.visibility = View.GONE
            val prog = p.num("progress", -1.0)
            bar.visibility = if (prog >= 0) View.VISIBLE else View.GONE
            if (prog >= 0) bar.progress = (prog * 1000).toInt()
            c.contentDescription = listOf(title(p), p.str("subtitle")).filter { it.isNotEmpty() }.joinToString(". ")
            c.isSelected = p.bool("selected")
            c.isEnabled = !p.containsKey("enabled") || p.bool("enabled")
        }
        return e
    }

    /**
     * The hero (Play): a fixed-height band, the map's picture at a sensible
     * size on the right (centre-cropped, never stretched across the card) under
     * a scrim that fades into the navy on the left, where the title, the facts
     * and the buttons sit on one or two rows.
     */
    private fun heroCard(id: String, props: Map<String, Any?>): Entry {
        val c = Kit.card(ctx, clickable = true, radiusDp = 24f)
        val h = (ctx.resources.displayMetrics.heightPixels * 0.30f).toInt()
        val picW = h * 16 / 9
        val stack = FrameLayout(ctx)
        val img = RoundImage(ctx, 0f)
        stack.addView(img, FrameLayout.LayoutParams(picW, h, Gravity.END))
        img.fadeInto = Theme.PANEL
        val texts = Kit.column(ctx, 12f)
        texts.gravity = Gravity.CENTER_VERTICAL
        texts.setPadding(dp(28f), dp(16f), dp(16f), dp(16f))
        val titleV = Kit.text(ctx, "", Theme.SP_DISPLAY, Theme.TEXT, bold = true, lines = 1)
        titleV.typeface = Theme.display
        val subtitle = Kit.text(ctx, "", Theme.SP_TITLE, Theme.TEXT_MUTED, lines = 1)
        val meta = FlowGrid(ctx, 0, dp(18f), wrap = true)
        val actions = FlowGrid(ctx, 0, dp(12f), wrap = true)
        texts.addView(titleV); texts.addView(subtitle); texts.addView(meta); texts.addView(actions)
        stack.addView(texts, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, h).apply { marginEnd = picW / 2 })
        c.addView(stack, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, h))
        val e = Entry(id, "hero", c, null, props)
        c.setOnClickListener { invoke(id, e.props["primary"].obj()) }
        e.bind = { x ->
            val p = x.props
            titleV.text = title(p)
            meta.removeAllViews()
            for (m in p["meta"].list()) meta.addView(Kit.text(ctx, m.obj().str("text"), Theme.SP_BODY, Theme.TEXT, bold = true).also { Kit.icon(it, m.obj().str("icon"), Theme.BLUE, 20f) })
            meta.visibility = if (meta.childCount == 0) View.GONE else View.VISIBLE
            // the facts or the subtitle, not both (they say the same)
            subtitle.text = if (meta.childCount > 0) "" else p.str("subtitle")
            subtitle.visibility = if (subtitle.text.isEmpty()) View.GONE else View.VISIBLE
            actions.removeAllViews()
            val prim = p["primary"].obj()
            if (prim.isNotEmpty()) actions.addView(actionButton(id, prim, true).apply { minimumWidth = dp(200f) })
            for (a in p["actions"].list()) actions.addView(actionButton(id, a.obj()))
            val ref = p.str("image")
            if (ref.isEmpty()) img.none() else picture(img, ref)
            c.contentDescription = listOf(title(p), p.str("subtitle")).filter { it.isNotEmpty() }.joinToString(". ")
        }
        return e
    }

    private fun badge(b: Map<String, Any?>): TextView = Kit.text(ctx, b.str("text"), Theme.SP_CAPTION, toneColor(b.str("tone")), bold = true).apply {
        Kit.icon(this, b.str("icon"), toneColor(b.str("tone")), 16f)
        compoundDrawablePadding = dp(4f)
        val h = dp(8f)
        setPadding(h, dp(3f), h, dp(3f))
        background = Theme.rounded(0xFF13254D.toInt(), dp(10f).toFloat(), max(1, dp(1f)), 0xFF2B4A86.toInt())
    }

    private fun screenKind(e: Entry): String {
        var cur: Entry? = e
        while (cur != null) { if (cur.type == "screen") return cur.props.str("kind"); cur = cur.parent?.let { nodes[it] } }
        return ""
    }

    private fun toneColor(t: String) = when (t) { "positive" -> Theme.GOOD; "danger" -> Theme.BAD; "warning" -> Theme.WARN; "accent" -> Theme.EMBER; else -> Theme.TEXT }

    /** Asks for a picture once (through the host's bridge), fills every view waiting on it. */
    fun picture(img: RoundImage, ref: String) {
        img.tag = ref
        val ready: (String) -> Unit = { path ->
            if (img.tag == ref) {
                if (path.isEmpty()) img.none()
                else {
                    val w = max(img.width, dp(320f))
                    Images.cached(path, w)?.let { img.set(it) } ?: run {
                        Images.load(path, w) { b -> if (img.tag == ref) { if (b != null) img.set(b) else img.none() } }
                    }
                }
            }
        }
        imagePaths[ref]?.let { ready(it); return }
        img.set(null)
        val waiting = imageWaiters.getOrPut(ref) { ArrayList() }
        waiting.add(ready)
        if (waiting.size == 1) events.image(ref, dp(480f), dp(270f))
    }

    private fun preview(id: String, props: Map<String, Any?>): Entry {
        val row = Kit.row(ctx, 28f)
        row.gravity = Gravity.TOP
        val img = RoundImage(ctx, 20f)
        val info = Kit.column(ctx, 10f)
        val titleV = Kit.text(ctx, "", Theme.SP_DISPLAY, Theme.TEXT, bold = true)
        titleV.typeface = Theme.display
        val sub = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT_MUTED)
        val badges = FlowGrid(ctx, 0, dp(6f), wrap = true)
        val attrs = Kit.column(ctx, 4f)
        val desc = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT)
        val actions = FlowGrid(ctx, 0, dp(12f), wrap = true)
        info.addView(titleV); info.addView(sub); info.addView(badges); info.addView(attrs); info.addView(desc); info.addView(actions)
        row.addView(img, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.1f))
        row.addView(info, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val e = Entry(id, "preview", row, null, props)
        e.bind = { x ->
            val p = x.props
            titleV.text = title(p)
            sub.text = p.str("subtitle"); sub.visibility = if (sub.text.isEmpty()) View.GONE else View.VISIBLE
            desc.text = p.str("body"); desc.visibility = if (desc.text.isEmpty()) View.GONE else View.VISIBLE
            attrs.removeAllViews()
            for (f in p["attrs"].list()) {
                val m = f.obj()
                val line = Kit.row(ctx, 12f)
                line.addView(Kit.text(ctx, Icons.label(m.str("icon"), m.str("label")), Theme.SP_BODY, Theme.TEXT_MUTED), LinearLayout.LayoutParams(dp(200f), ViewGroup.LayoutParams.WRAP_CONTENT))
                line.addView(Kit.text(ctx, m.str("value"), Theme.SP_BODY, Theme.TEXT, bold = true))
                attrs.addView(line)
            }
            badges.removeAllViews()
            for (b in p["badges"].list()) badges.addView(badge(b.obj()))
            actions.removeAllViews()
            val prim = p["primary"].obj()
            if (prim.isNotEmpty()) actions.addView(actionButton(id, prim, true))
            for (a in p["actions"].list()) actions.addView(actionButton(id, a.obj()))
            img.aspect = aspect(p, 4f / 3f)
            val ref = p.str("image")
            // no picture (a mod without an icon): the text takes the row
            img.visibility = if (ref.isEmpty()) View.GONE else View.VISIBLE
            if (ref.isEmpty()) img.none() else picture(img, ref)
        }
        return e
    }

    private fun text(id: String, props: Map<String, Any?>): Entry {
        val t = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT)
        val e = Entry(id, "text", t, null, props)
        e.bind = { x ->
            val style = x.props.str("style", "body")
            t.text = x.props.str("body").ifEmpty { title(x.props) }
            t.textSize = when (style) { "title" -> Theme.SP_TITLE; "caption", "note" -> Theme.SP_CAPTION; else -> Theme.SP_BODY }
            t.typeface = when (style) { "title" -> Theme.bold; "mono" -> android.graphics.Typeface.MONOSPACE; else -> Theme.regular }
            t.setTextColor(when (style) { "caption", "note" -> Theme.TEXT_MUTED; "warning" -> Theme.WARN; else -> Theme.TEXT })
            t.setTextIsSelectable(x.props.bool("selectable"))
        }
        return e
    }

    private fun image(id: String, props: Map<String, Any?>): Entry {
        val img = RoundImage(ctx, 16f)
        val e = Entry(id, "image", img, null, props)
        e.bind = { x ->
            img.aspect = aspect(x.props, 16f / 9f)
            img.contain = x.props.str("fit") == "contain"
            // a banner or picture never takes more than a fifth (contain) or two fifths of the screen's height
            img.maxHeight = (ctx.resources.displayMetrics.heightPixels * if (img.contain) 0.2f else 0.4f).toInt()
            // `role: banner` with `emphasis: high` (Home, docs/UI_BRIDGE.md): at the top, scaled to the
            // screen; on a short screen (phone landscape) it gives way first: the row of cards and the
            // bar of Settings, Credits and Quit must fit under it
            val h = ctx.resources.displayMetrics.heightPixels
            val hdp = h / ctx.resources.displayMetrics.density
            if (x.props.str("role") == "banner" && x.props.str("emphasis") == "high") img.maxHeight = (h * (if (hdp < 480f) 0.16f else if (hdp < 640f) 0.22f else 0.3f)).toInt()
            else if (img.contain && screenKind(x) == "home") img.maxHeight = (h * 0.11f).toInt()
            img.contentDescription = x.props.str("alt")
            val r = x.props.str("ref")
            if (r.isEmpty()) img.none() else picture(img, r)
        }
        return e
    }

    /** Home's bar (`tabbar`: Settings, Credits, Quit): one strip along the bottom, joined edge to edge. */
    private fun tabbar(id: String, props: Map<String, Any?>): Entry {
        val row = Kit.row(ctx)
        row.setBackgroundColor(Theme.PANEL)
        row.showDividers = LinearLayout.SHOW_DIVIDER_MIDDLE
        row.dividerDrawable = android.graphics.drawable.GradientDrawable().apply { setColor(Theme.OUTLINE); setSize(max(1, dp(1f)), 1) }
        return Entry(id, "tabbar", row, row, props)
    }

    /**
     * One segment of Home's bar (Settings, Credits, Quit): a flat button joined to its neighbours.
     * The open screen's one (the bar's `current`) is a darker shade with an ember line on top and
     * does nothing when tapped; a pressed or focused one is darker too. Quit asks first (invoke).
     */
    private fun tab(id: String, props: Map<String, Any?>): Entry {
        val frame = FrameLayout(ctx)
        frame.minimumHeight = dp(56f)
        frame.isClickable = true
        frame.isFocusable = true
        val titleV = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT, bold = true, lines = 1)
        titleV.gravity = Gravity.CENTER
        titleV.ellipsize = android.text.TextUtils.TruncateAt.END
        titleV.compoundDrawablePadding = dp(8f)
        titleV.setPadding(dp(8f), 0, dp(8f), 0)
        frame.addView(titleV, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        val line = View(ctx)
        frame.addView(line, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, max(1, dp(1f)), Gravity.TOP))
        val e = Entry(id, "tab", frame, null, props)
        e.bind = { x ->
            val p = x.props
            val cur = nodes[x.parent ?: ""]?.props?.str("current").orEmpty()
            val current = cur.isNotEmpty() && x.id.endsWith("/$cur")
            frame.background = android.graphics.drawable.StateListDrawable().apply {
                val dark = android.graphics.drawable.ColorDrawable(Theme.BG)
                addState(intArrayOf(android.R.attr.state_pressed), dark)
                addState(intArrayOf(android.R.attr.state_focused), dark)
                addState(intArrayOf(), android.graphics.drawable.ColorDrawable(if (current) Theme.BG else Theme.PANEL_HI))
            }
            line.setBackgroundColor(if (current) Theme.EMBER else Theme.OUTLINE)
            (line.layoutParams as? FrameLayout.LayoutParams)?.let { it.height = if (current) dp(3f) else max(1, dp(1f)); line.layoutParams = it }
            val a = p["action"].obj()
            val tint = if (current) Theme.EMBER else Theme.TEXT
            titleV.text = p.str("label").ifEmpty { a.str("label") }
            titleV.setTextColor(tint)
            titleV.setCompoundDrawablesRelative(Icons.drawable(a.str("icon"), dp(18f), tint), null, null, null)
            frame.contentDescription = titleV.text
            frame.isSelected = current
            frame.setOnClickListener { if (!current) invoke(id, e.props["action"].obj()) }
        }
        return e
    }

    private fun status(id: String, props: Map<String, Any?>): Entry {
        val col = Kit.column(ctx, 12f)
        col.gravity = Gravity.CENTER_HORIZONTAL
        col.setPadding(0, dp(32f), 0, dp(32f))
        val icon = Kit.text(ctx, "", Theme.SP_DISPLAY, Theme.BLUE)
        val titleV = Kit.text(ctx, "", Theme.SP_HEADLINE, Theme.TEXT, bold = true)
        val body = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT_MUTED)
        val bar = ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000; progressTintList = android.content.res.ColorStateList.valueOf(Theme.EMBER)
        }
        val actions = Kit.row(ctx, 12f)
        actions.gravity = Gravity.CENTER
        for (v in listOf(icon, titleV, body)) { v.gravity = Gravity.CENTER; col.addView(v) }
        col.addView(bar, LinearLayout.LayoutParams(dp(420f), dp(8f)))
        col.addView(actions)
        val e = Entry(id, "status", col, null, props)
        e.bind = { x ->
            val p = x.props
            val d = Icons.drawable(p.str("icon"), dp(56f), Theme.BLUE)
            icon.setCompoundDrawablesRelative(null, d, null, null)
            icon.visibility = if (d == null) View.GONE else View.VISIBLE
            titleV.text = title(p)
            body.text = p.str("body"); body.visibility = if (body.text.isEmpty()) View.GONE else View.VISIBLE
            val prog = p.num("progress", -1.0)
            bar.visibility = if (prog >= 0 || p.bool("indeterminate")) View.VISIBLE else View.GONE
            bar.isIndeterminate = p.bool("indeterminate")
            if (prog >= 0) bar.progress = (prog * 1000).toInt()
            actions.removeAllViews()
            p["primary"].obj().takeIf { it.isNotEmpty() }?.let { actions.addView(actionButton(id, it, true)) }
            for (a in p["actions"].list()) actions.addView(actionButton(id, a.obj()))
        }
        return e
    }

    private fun progress(id: String, props: Map<String, Any?>): Entry {
        val col = Kit.column(ctx, 6f)
        val head = Kit.row(ctx, 12f)
        val label = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT, bold = true)
        head.addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        val cancel = Kit.button(ctx, "Cancel") {}
        head.addView(cancel)
        val bar = ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            progressTintList = android.content.res.ColorStateList.valueOf(Theme.EMBER)
            progressBackgroundTintList = android.content.res.ColorStateList.valueOf(0xFF2A3342.toInt())
        }
        val detail = Kit.text(ctx, "", Theme.SP_CAPTION, Theme.TEXT_MUTED)
        col.addView(head)
        col.addView(bar, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(10f)))
        col.addView(detail)
        val e = Entry(id, "progress", col, null, props)
        e.bind = { x ->
            val p = x.props
            label.text = p.str("label")
            bar.isIndeterminate = p.bool("indeterminate")
            if (!bar.isIndeterminate) bar.progress = (p.num("value") * 1000).toInt()
            detail.text = p.str("detail"); detail.visibility = if (detail.text.isEmpty()) View.GONE else View.VISIBLE
            val c = p["cancel"].obj()
            cancel.visibility = if (c.isEmpty()) View.GONE else View.VISIBLE
            cancel.text = c.str("label", "Cancel")
            cancel.setOnClickListener { invoke(id, c) }
        }
        return e
    }

    private fun applyNote(p: Map<String, Any?>): String = when (p.str("apply")) {
        "video" -> "Applies when you leave this screen"
        "sound" -> "Applies after a sound restart"
        "reconnect" -> "Applies to the next match"
        "restart" -> "Applies after a restart"
        else -> ""
    }

    private fun help(p: Map<String, Any?>): String {
        val err = p.str("error")
        if (err.isNotEmpty()) return err
        return listOf(p.str("help"), applyNote(p)).filter { it.isNotEmpty() }.joinToString(" · ")
    }

    private fun switchRow(id: String, props: Map<String, Any?>): Entry {
        if (props.str("style") == "checkbox") return checkChip(id, props)
        var muted = false
        val holder = FrameLayout(ctx)
        val e = Entry(id, "switch", holder, null, props)
        lateinit var s: android.widget.CompoundButton
        // `confirm` (an object like an action's): asked before the switch turns on
        // (a mod's Full engine access); Cancel puts it back
        s = Kit.switch(ctx, props.bool("value"), title(props)) { v ->
            if (muted) return@switch
            val c = e.props["confirm"].obj()
            if (v && c.isNotEmpty()) {
                muted = true; s.isChecked = false; muted = false
                host.confirm(c) { muted = true; s.isChecked = true; muted = false; events.value(id, true) }
            } else events.value(id, v)
        }
        e.bind = { x ->
            holder.removeAllViews()
            (s.parent as? ViewGroup)?.removeView(s)
            muted = true
            s.isChecked = x.props.bool("value")
            muted = false
            s.isEnabled = !x.props.containsKey("enabled") || x.props.bool("enabled")
            holder.addView(Kit.formRow(ctx, title(x.props), help(x.props), s) { if (s.isEnabled) s.toggle() })
        }
        return e
    }

    private fun format(p: Map<String, Any?>): (Double) -> String {
        val unit = p.str("unit")
        return { v ->
            when (p.str("format")) {
                "percent" -> "${Math.round(v * 100)}%"
                "float1" -> String.format(java.util.Locale.US, "%.1f", v)
                "float2" -> String.format(java.util.Locale.US, "%.2f", v)
                "ms" -> "${Math.round(v)} ms"
                "x" -> String.format(java.util.Locale.US, "%.2f×", v)
                "deg" -> "${Math.round(v)}°"
                "db" -> String.format(java.util.Locale.US, "%.1f dB", v)
                else -> if (p.num("step", 1.0) >= 1.0) Math.round(v).toString() else String.format(java.util.Locale.US, "%.2f", v)
            } + if (unit.isNotEmpty()) " $unit" else ""
        }
    }

    private fun sliderRow(id: String, props: Map<String, Any?>): Entry {
        val holder = FrameLayout(ctx)
        val e = Entry(id, "slider", holder, null, props)
        var dragging = false
        var lastSent = 0L
        e.bind = { x ->
            if (!dragging) {
                holder.removeAllViews()
                val p = x.props
                val sl = Kit.slider(ctx, p.num("value"), p.num("min"), p.num("max", 1.0), p.num("step", 0.0), title(p), format(p)) { v, done ->
                    dragging = !done
                    val now = android.os.SystemClock.uptimeMillis()
                    if (done || now - lastSent >= 100) { lastSent = now; events.value(id, v) } // 10 Hz while dragging, the last on release
                }
                holder.addView(Kit.formRow(ctx, title(p), help(p), sl))
            }
        }
        return e
    }

    private fun stepper(id: String, props: Map<String, Any?>): Entry {
        val holder = FrameLayout(ctx)
        val e = Entry(id, "stepper", holder, null, props)
        e.bind = { x ->
            holder.removeAllViews()
            val p = x.props
            val v = p.num("value"); val step = p.num("step", 1.0)
            val box = Kit.row(ctx, 8f)
            val minus = Kit.button(ctx, "−") { events.value(id, maxOf(p.num("min", -1e9), v - step)) }
            val shown = Kit.text(ctx, format(p)(v), Theme.SP_TITLE, Theme.TEXT, bold = true).apply { minWidth = dp(64f); gravity = Gravity.CENTER }
            val plus = Kit.button(ctx, "+") { events.value(id, minOf(p.num("max", 1e9), v + step)) }
            minus.contentDescription = "Less"; plus.contentDescription = "More"
            box.addView(minus); box.addView(shown); box.addView(plus)
            holder.addView(Kit.formRow(ctx, title(p), help(p), box))
        }
        return e
    }

    private fun choice(id: String, props: Map<String, Any?>): Entry {
        val holder = FrameLayout(ctx)
        val e = Entry(id, "choice", holder, null, props)
        e.bind = { x ->
            holder.removeAllViews()
            val p = x.props
            val opts = p["options"].list().map { it.obj() }
            val sel = p.str("value")
            val style = p.str("style", "auto").let { if (it == "auto") (if (opts.size <= 5) "segmented" else "dropdown") else it }
            if (style == "segmented" || style == "radio" || style == "cards") {
                val col = Kit.column(ctx, 6f)
                col.setPadding(dp(16f), dp(8f), dp(16f), dp(8f))
                if (title(p).isNotEmpty()) col.addView(Kit.text(ctx, title(p), Theme.SP_BODY, Theme.TEXT, bold = true))
                val chips = FlowGrid(ctx, 0, dp(8f), wrap = true)
                for (o in opts) chips.addView(chipView(Icons.label(o.str("icon"), o.str("label", o.str("value"))), o.str("value") == sel) {
                    events.value(id, o.str("value")); x.props = x.props + ("value" to o.str("value")); bindSafe(x)
                })
                col.addView(chips)
                val h = help(p)
                if (h.isNotEmpty()) col.addView(Kit.text(ctx, h, Theme.SP_CAPTION, Theme.TEXT_MUTED))
                holder.addView(col)
            } else {
                val cur = opts.firstOrNull { it.str("value") == sel }
                val value = Kit.text(ctx, (cur?.str("label") ?: sel) + "  ▾", Theme.SP_BODY, Theme.EMBER, bold = true)
                holder.addView(Kit.formRow(ctx, title(p), help(p), value) { pick(x, opts) })
            }
        }
        return e
    }

    /** A checkbox as a compact pill (the server list's filters: LAN, Empty, Full, Laggy). */
    private fun checkChip(id: String, props: Map<String, Any?>): Entry {
        val holder = FrameLayout(ctx)
        val e = Entry(id, "switch", holder, null, props)
        e.bind = { x ->
            holder.removeAllViews()
            val on = x.props.bool("value")
            val chip = chipView(title(x.props), on) {
                x.props = x.props + ("value" to !on)
                bindSafe(x)
                events.value(id, !on)
            }
            Kit.icon(chip, x.props.str("icon"), if (on) Color.WHITE else Theme.TEXT_MUTED, 18f)
            chip.isEnabled = !x.props.containsKey("enabled") || x.props.bool("enabled")
            holder.addView(chip)
        }
        return e
    }

    /** A compact on/off pill; tapping it never opens anything. */
    private fun chipView(label: String, on: Boolean, tap: () -> Unit): TextView = Kit.text(ctx, (if (on) "✓ " else "") + label, Theme.SP_BODY, if (on) Color.WHITE else Theme.TEXT_MUTED, bold = true, lines = 1).apply {
        gravity = Gravity.CENTER
        minHeight = dp(Kit.MIN_TARGET_DP)
        setPadding(dp(16f), 0, dp(16f), 0)
        isClickable = true; isFocusable = true; isSelected = on
        background = if (on) Theme.rounded(0xFF1A3D7A.toInt(), dp(24f).toFloat(), dp(2f), Theme.BLUE) else Theme.surface(ctx, 0xFF060E1F.toInt(), 24f)
        contentDescription = label + if (on) ", selected" else ""
        setOnClickListener { Kit.haptic(it); tap() }
    }

    private fun pick(e: Entry, opts: List<Map<String, Any?>>) {
        val list = Kit.column(ctx, 4f)
        list.addView(Kit.text(ctx, title(e.props), Theme.SP_TITLE, Theme.TEXT, bold = true).apply { setPadding(dp(16f), dp(8f), dp(16f), dp(8f)) })
        for (o in opts) {
            val on = o.str("value") == e.props.str("value")
            val r = Kit.formRow(ctx, Icons.label(o.str("icon"), o.str("label", o.str("value"))), o.str("help"), if (on) Kit.text(ctx, "✓", Theme.SP_TITLE, Theme.EMBER, bold = true) else null) {
                events.value(e.id, o.str("value"))
                e.props = e.props + ("value" to o.str("value"))
                bindSafe(e)
                host.showDialog(null)
            }
            r.isActivated = on
            list.addView(r)
        }
        val sv = ScrollView(ctx).apply { addView(list); scrollBarStyle = View.SCROLLBARS_INSIDE_OVERLAY }
        host.showDialog(dialogFrame(sv, true))
    }

    private fun field(id: String, props: Map<String, Any?>): Entry {
        val secret = props.str("kind") == "password" || props.bool("private")
        val col = Kit.column(ctx, 6f)
        col.setPadding(dp(16f), dp(6f), dp(16f), dp(6f))
        val titleV = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT, bold = true)
        val lock = Kit.text(ctx, "", Theme.SP_TITLE, Theme.TEXT_MUTED).apply { minWidth = dp(40f); gravity = Gravity.CENTER }
        val e = Entry(id, "field", col, null, props)
        val input = Kit.input(ctx, if (secret) "" else props.str("value"), props.str("placeholder"), secret) { text ->
            val p = e.props
            if (p["submit"].obj().isNotEmpty()) return@input
            if (secret) { if (text.isNotEmpty()) events.value(id, text) } else if (text != p.str("value")) events.value(id, text)
        }
        input.setOnEditorActionListener { v, action, _ ->
            if (action == android.view.inputmethod.EditorInfo.IME_ACTION_DONE || action == android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH) {
                val text = v.text.toString()
                if (e.props["submit"].obj().isNotEmpty()) events.submit(id, text) else events.value(id, text)
                if (secret) (v as android.widget.EditText).setText("") // write-only: no copy kept
            }
            false
        }
        when (props.str("kind")) {
            "number" -> input.inputType = android.text.InputType.TYPE_CLASS_NUMBER
            "url", "address" -> input.inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_URI
            "search" -> input.imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_SEARCH or android.view.inputmethod.EditorInfo.IME_FLAG_NO_EXTRACT_UI
        }
        val line = Kit.row(ctx, 8f)
        line.addView(input, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (secret) line.addView(lock)
        val helpV = Kit.text(ctx, "", Theme.SP_CAPTION, Theme.TEXT_MUTED)
        col.addView(titleV); col.addView(line); col.addView(helpV)
        e.bind = { x ->
            val p = x.props
            val inBar = nodes[x.parent ?: ""]?.type in setOf("toolbar", "actionbar", "row")
            titleV.text = title(p); titleV.visibility = if (titleV.text.isEmpty() || inBar) View.GONE else View.VISIBLE
            if (inBar) col.setPadding(0, 0, 0, 0)
            input.hint = p.str("placeholder").ifEmpty { title(p) }
            input.isEnabled = !p.containsKey("enabled") || p.bool("enabled")
            if (!secret && !input.hasFocus() && input.text.toString() != p.str("value")) input.setText(p.str("value"))
            // the lock: open while no password applies, closed when one is on and set
            val locked = p.bool("hasValue") && input.isEnabled
            lock.text = if (locked) "🔒" else "🔓"
            lock.setTextColor(if (locked) Theme.WARN else Theme.TEXT_DIM)
            lock.contentDescription = if (locked) "Password set" else "No password"
            val h = help(p)
            helpV.text = h
            helpV.setTextColor(if (p.str("error").isNotEmpty()) Theme.BAD else Theme.TEXT_MUTED)
            helpV.visibility = if (h.isEmpty()) View.GONE else View.VISIBLE
            p.int("maxLength").takeIf { it > 0 }?.let { input.filters = arrayOf(android.text.InputFilter.LengthFilter(it)) }
        }
        return e
    }

    private fun button(id: String, props: Map<String, Any?>): Entry {
        val holder = FrameLayout(ctx)
        val e = Entry(id, "button", holder, null, props)
        e.bind = { x ->
            holder.removeAllViews()
            val p = x.props
            val act = p["action"].obj().ifEmpty { mapOf("id" to id) }
            val a = act + mapOf("style" to p.str("style", act.str("style")), "label" to p.str("label").ifEmpty { act.str("label") })
            val b = actionButton(id, a)
            b.isEnabled = !p.containsKey("enabled") || p.bool("enabled")
            holder.addView(b, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }
        return e
    }

    private fun dialogFrame(content: View, dismissable: Boolean): View {
        val frame = FrameLayout(ctx)
        frame.setBackgroundColor(0xB3000000.toInt())
        frame.isClickable = true // nothing behind it takes the tap
        if (dismissable) frame.setOnClickListener { events.back() }
        val box = Kit.card(ctx, clickable = false, radiusDp = 24f)
        box.isClickable = true
        val p = dp(20f)
        box.setPadding(p, p, p, p)
        (content.parent as? ViewGroup)?.removeView(content)
        box.addView(content)
        val w = minOf(dp(760f), (ctx.resources.displayMetrics.widthPixels * 0.9).toInt())
        frame.addView(box, FrameLayout.LayoutParams(w, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER).apply { topMargin = dp(24f); bottomMargin = dp(24f) })
        return frame
    }

    private fun generic(id: String, type: String, props: Map<String, Any?>): Entry {
        val col = Kit.column(ctx, 8f)
        val head = FrameLayout(ctx)
        col.addView(head)
        val kids = Kit.column(ctx, 8f)
        col.addView(kids)
        val e = Entry(id, type, col, kids, props)
        e.bind = { x ->
            head.removeAllViews()
            val p = x.props
            val t = title(p)
            if (t.isNotEmpty()) {
                val trailing = Kit.row(ctx, 8f)
                if (p.containsKey("value")) trailing.addView(Kit.text(ctx, p["value"].let { v -> if (v is List<*>) v.joinToString(" / ") else p.str("value") }, Theme.SP_BODY, Theme.EMBER, bold = true))
                for (a in p["actions"].list()) trailing.addView(actionButton(id, a.obj()))
                head.addView(Kit.formRow(ctx, Icons.label(p.str("icon"), t), p.str("subtitle").ifEmpty { help(p) }, trailing))
            }
        }
        return e
    }

    // ------------------------------------------------------------------ lists

    private fun colWeight(c: Map<String, Any?>) = when (c.str("width")) { "xs" -> 0.35f; "s" -> 0.7f; "m" -> 1.2f; "l" -> 1.8f; "fill" -> 3f; else -> 1f }

    private fun visibleColumns(cols: List<Map<String, Any?>>): List<Map<String, Any?>> {
        val wdp = ctx.resources.displayMetrics.widthPixels / ctx.resources.displayMetrics.density
        val maxPriority = when { wdp >= 900 -> 3; wdp >= 640 -> 2; else -> 1 }
        return cols.filter { it.int("priority", 1) <= maxPriority }
    }

    /**
     * A data list over a source (the server browser, mods, recent games):
     * `display: table` is one dense row per entry with aligned columns and a
     * slim sortable header; `cards` is a grid of preview cards (the map
     * picker). Rows come in windows as they approach (VirtualList).
     */
    private fun list(id: String, props: Map<String, Any?>): Entry {
        val cards = props.str("display") == "cards"
        val box = Kit.column(ctx)
        val header = Kit.row(ctx)
        val empty = Kit.column(ctx, 6f).apply { gravity = Gravity.CENTER; setPadding(0, dp(24f), 0, dp(24f)); visibility = View.GONE }
        var columns: List<Map<String, Any?>> = emptyList()
        val e = Entry(id, "list", box, null, props)
        e.source = props.str("source")
        e.empty = empty
        val rowH = dp(when (props.str("rowHeight", if (cards) "card" else "compact")) { "compact" -> 40f; "card" -> 96f; else -> 56f })
        val request: (IntRange) -> Unit = { r ->
            val p = e.props
            events.window(id, e.source, r.first, r.last - r.first + 1, p.str("sort"), p.bool("desc"), p.str("filter"))
        }
        val vl: VirtualList
        if (cards) {
            val gap = dp(14f)
            class CardCell(val card: FrameLayout, val img: RoundImage, val titleV: TextView, val sub: TextView, val badges: FlowGrid) : VirtualList.Cell(card) {
                override fun recycled() { img.tag = null; img.set(null) }
            }
            vl = VirtualList(ctx, id, request, 60, grid = true, minCellPx = dp(props.num("minItem", 220.0).toFloat()) + gap,
                bindCell = { h, row, _ ->
                    val c = h as CardCell
                    val cells = row?.get("cells").obj()
                    c.titleV.text = cells.str("title").ifEmpty { cells.str("name") }
                    c.sub.text = listOf(cells.str("players"), cells.str("author")).filter { it.isNotEmpty() }.joinToString(" · ")
                    c.sub.visibility = if (c.sub.text.isEmpty()) View.GONE else View.VISIBLE
                    c.badges.removeAllViews()
                    row?.get("badges").list().take(4).forEach { b -> c.badges.addView(badge(b.obj())) }
                    if (row != null && row["badges"] == null) cells.str("modes").split(' ').filter { it.isNotEmpty() }.take(4).forEach { m -> c.badges.addView(badge(mapOf("text" to m.uppercase()))) }
                    c.img.aspect = aspect(mapOf("aspect" to e.props.str("aspectItem", "16:9")), 16f / 9f)
                    val ref = row?.str("image") ?: ""
                    if (ref.isEmpty()) { if (row == null) c.img.set(null) else c.img.none() } else picture(c.img, ref)
                    c.card.isSelected = row != null && e.props["selected"].list().contains(row.str("key"))
                    (c.card.layoutParams as? ViewGroup.MarginLayoutParams)?.let { lp ->
                        // `rows: N`: fixed tiles in rows that scroll sideways
                        val f = e.list?.fit
                        val carousel = e.list?.carouselWidth ?: 0
                        val w = f?.width ?: if (carousel > 0) carousel - gap else ViewGroup.LayoutParams.MATCH_PARENT
                        val h = f?.height ?: ViewGroup.LayoutParams.WRAP_CONTENT
                        if (lp.width != w || lp.height != h) { lp.width = w; lp.height = h; c.card.layoutParams = lp }
                    }
                    c.card.contentDescription = row?.let { "${c.titleV.text}. ${c.sub.text}" } ?: "Loading"
                    bindTap(c.card, e, id, row)
                },
                makeCell = { _ ->
                    val card = Kit.card(ctx, clickable = true, radiusDp = 16f)
                    val col = Kit.column(ctx)
                    val img = RoundImage(ctx, 0f)
                    val texts = Kit.column(ctx, 4f)
                    texts.setPadding(dp(12f), dp(8f), dp(12f), dp(12f))
                    val t = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT, bold = true, lines = 1)
                    val s = Kit.text(ctx, "", Theme.SP_CAPTION, Theme.TEXT_MUTED, lines = 1)
                    val b = FlowGrid(ctx, 0, dp(4f), wrap = true)
                    texts.addView(t); texts.addView(s); texts.addView(b)
                    col.addView(img); col.addView(texts)
                    card.addView(col)
                    val lp = androidx.recyclerview.widget.RecyclerView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                    lp.setMargins(gap / 2, gap / 2, gap / 2, gap / 2)
                    card.layoutParams = lp
                    CardCell(card, img, t, s, b)
                })
            vl.cellGap = gap
            vl.cellChrome = {
                // one tile measured with a typical row: its height less the picture's
                val probe = Kit.column(ctx)
                val t = Kit.text(ctx, "Map", Theme.SP_BODY, Theme.TEXT, bold = true, lines = 1)
                val sub = Kit.text(ctx, "2-4", Theme.SP_CAPTION, Theme.TEXT_MUTED, lines = 1)
                val badges = FlowGrid(ctx, 0, dp(4f), wrap = true).apply { addView(badge(mapOf("text" to "DM"))) }
                probe.setPadding(dp(12f), dp(8f), dp(12f), dp(12f))
                probe.addView(t); probe.addView(sub); probe.addView(badges)
                probe.measure(View.MeasureSpec.makeMeasureSpec(dp(300f), View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED))
                probe.measuredHeight + dp(12f) // the badges row measures short outside a card
            }
            box.addView(empty)
            box.addView(vl.view, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        } else {
            box.background = Theme.rounded(Theme.PANEL, dp(16f).toFloat(), max(1, dp(1f)), Theme.OUTLINE)
            box.clipToOutline = true
            box.outlineProvider = android.view.ViewOutlineProvider.BACKGROUND
            header.setPadding(dp(12f), 0, dp(12f), 0)
            header.minimumHeight = dp(36f)
            header.setBackgroundColor(0xFF0E1E40.toInt())
            class RowCell(val row: LinearLayout) : VirtualList.Cell(row)
            vl = VirtualList(ctx, id, request, 50, grid = false, minCellPx = 0,
                bindCell = { holder, data, _ ->
                    val r = (holder as RowCell).row
                    if (r.childCount != columns.size) rebuildRow(r, columns, rowH)
                    val cells = data?.get("cells").obj()
                    for ((i, c) in columns.withIndex()) bindCell(r.getChildAt(i) as TextView, c, cells, data == null)
                    // `stale`: a saved server not answered yet this refresh (drawn dimmed)
                    r.alpha = if (data?.bool("disabled") == true) 0.5f else if (cells.bool("stale") || data?.bool("stale") == true) 0.55f else 1f
                    val on = data != null && e.props["selected"].list().any { it.toString() == VirtualList.keyOf(data) }
                    r.isSelected = on
                    r.background = if (on) Theme.rounded(0xFF1D3A73.toInt(), dp(8f).toFloat(), max(1, dp(1.5f)), Theme.EMBER)
                        else Theme.surface(ctx, Color.TRANSPARENT, 0f, Color.TRANSPARENT)
                    r.contentDescription = if (data == null) "Loading" else columns.joinToString(", ") { c -> "${c.str("label")} ${cells.str(c.str("key"))}" } + if (on) ", selected" else ""
                    bindTap(r, e, id, data)
                },
                makeCell = { _ ->
                    val r = Kit.row(ctx)
                    r.layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, rowH)
                    r.background = Theme.surface(ctx, Color.TRANSPARENT, 0f, Color.TRANSPARENT)
                    r.isClickable = true
                    r.isFocusable = true
                    rebuildRow(r, columns, rowH)
                    RowCell(r)
                })
            vl.view.addItemDecoration(object : androidx.recyclerview.widget.RecyclerView.ItemDecoration() {
                val paint = android.graphics.Paint().apply { color = 0xFF16264A.toInt() }
                override fun onDraw(c: android.graphics.Canvas, parent: androidx.recyclerview.widget.RecyclerView, state: androidx.recyclerview.widget.RecyclerView.State) {
                    for (i in 0 until parent.childCount) {
                        val v = parent.getChildAt(i)
                        c.drawRect(0f, v.bottom.toFloat(), parent.width.toFloat(), v.bottom + max(1, dp(1f)).toFloat(), paint)
                    }
                }
            })
            box.addView(header, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            box.addView(empty)
            box.addView(vl.view, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        lists.getOrPut(e.source) { ArrayList() }.add(vl)
        e.list = vl
        vl.onVisible = { r, touching -> events.visible(id, r.first, r.last - r.first + 1, touching) }
        var lastQuery = ""
        var lastSelected: List<Any?> = emptyList()
        e.bind = { x ->
            val p = x.props
            // only `selected` changed (a tap's diff): rebind in place, the list keeps its position
            val sel = p["selected"].list()
            if (sel != lastSelected) { lastSelected = sel; vl.refresh() }
            if (cards) {
                // `rows: 2`: the map picker shows two whole rows of tiles (docs/UI_BRIDGE.md)
                val rows = fitRows(p)
                val asp = aspect(mapOf("aspect" to p.str("aspectItem", "16:9")), 16f / 9f)
                if (rows != vl.fitRows || asp != vl.cellAspect) { vl.fitRows = rows; vl.cellAspect = asp; vl.view.requestLayout() }
            }
            val newCols = visibleColumns(p["columns"].list().map { it.obj() })
            val colsChanged = newCols != columns
            columns = newCols
            if (!cards) {
                header.removeAllViews()
                val sortKey = p.str("sort")
                val desc = p.bool("desc")
                for (c in columns) {
                    val on = c.str("key") == sortKey
                    val t = Kit.text(ctx, c.str("label") + if (on) (if (desc) " ▾" else " ▴") else "", Theme.SP_CAPTION, if (on) Theme.EMBER else Theme.TEXT_MUTED, bold = true, lines = 1)
                    t.gravity = gravityOf(c) or Gravity.CENTER_VERTICAL
                    t.minHeight = dp(36f)
                    t.setPadding(dp(4f), 0, dp(4f), 0)
                    if (c.bool("sortable")) {
                        t.isClickable = true
                        t.isFocusable = true
                        t.background = Theme.surface(ctx, Color.TRANSPARENT, 6f, Color.TRANSPARENT)
                        t.contentDescription = "Sort by ${c.str("label")}"
                        t.setOnClickListener {
                            val d = if (on) !desc else (c.str("type") in setOf("players", "number"))
                            x.props = x.props + mapOf("sort" to c.str("key"), "desc" to d)
                            bindSafe(x)
                        }
                    }
                    header.addView(t, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, colWeight(c)))
                }
            }
            // a new sort or filter re-windows the visible range
            val query = "${p.str("sort")}|${p.bool("desc")}|${p.str("filter")}"
            if (lastQuery.isNotEmpty() && query != lastQuery) vl.reset(maxOf(0, vl.window.total))
            else if (vl.window.total < 0 && p.containsKey("count")) vl.reset(p.int("count"))
            lastQuery = query
            if (colsChanged) vl.refresh()
            bindEmpty(x)
        }
        return e
    }

    private val a11yActions = java.util.WeakHashMap<View, Int>()
    private var lastTapKey = ""
    private var lastTapAt = 0L

    /**
     * Rows and cards of a list. With `selection` single or multi, a tap
     * selects (event row/select, drawn at once) and a double tap, Enter or
     * the accessibility "Activate" action activates (event row/activate:
     * joins the server, starts the map); without selection a tap activates.
     */
    private fun bindTap(v: View, e: Entry, id: String, row: Map<String, Any?>?) {
        if (row == null) { v.setOnClickListener(null); v.setOnKeyListener(null); return }
        val key = VirtualList.keyOf(row)
        val selecting = e.props.str("selection") in setOf("single", "multi")
        v.setOnClickListener { x ->
            Kit.haptic(x)
            if (!selecting) { events.row(id, key, "activate"); return@setOnClickListener }
            val now = android.os.SystemClock.uptimeMillis()
            val second = key == lastTapKey && now - lastTapAt <= android.view.ViewConfiguration.getDoubleTapTimeout() + 100
            lastTapKey = if (second) "" else key
            lastTapAt = now
            if (second) {
                if (e.props.str("selection") == "multi") select(e, id, key, true) // the first tap's toggle undone
                events.row(id, key, "activate")
            } else select(e, id, key, true)
        }
        v.setOnKeyListener { _, code, ev ->
            if (ev.action == android.view.KeyEvent.ACTION_UP && (code == android.view.KeyEvent.KEYCODE_ENTER || code == android.view.KeyEvent.KEYCODE_DPAD_CENTER || code == android.view.KeyEvent.KEYCODE_NUMPAD_ENTER)) {
                events.row(id, key, "activate"); true
            } else false
        }
        // recycled cells: the previous row's action goes first
        a11yActions.remove(v)?.let { androidx.core.view.ViewCompat.removeAccessibilityAction(v, it) }
        if (selecting) {
            androidx.core.view.ViewCompat.replaceAccessibilityAction(v, androidx.core.view.accessibility.AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_CLICK, "Select", null)
            a11yActions[v] = androidx.core.view.ViewCompat.addAccessibilityAction(v, e.props["rowPrimary"].obj().str("label", "Activate")) { _, _ -> events.row(id, key, "activate"); true }
        }
    }

    private fun select(e: Entry, id: String, key: String, send: Boolean) {
        val cur = e.props["selected"].list().map { it.toString() }
        val next = if (e.props.str("selection") == "multi") (if (key in cur) cur - key else cur + key) else listOf(key)
        if (next != cur) { e.props = e.props + mapOf("selected" to next); e.list?.refresh() }
        if (send) events.row(id, key, "select")
    }

    private fun bindEmpty(e: Entry) {
        val empty = e.empty ?: return
        val p = e.props
        val total = e.list?.window?.total ?: -1
        empty.removeAllViews()
        val st = p.str("state", "ready")
        val m = if (st == "error") p["error"].obj() else p["empty"].obj()
        val show = (st == "error") || (st == "ready" && total == 0)
        if (show) {
            empty.addView(Kit.text(ctx, Icons.label(m.str("icon"), m.str("title", if (st == "error") "Something went wrong" else "Nothing here yet")), Theme.SP_TITLE, Theme.TEXT, bold = true).apply { gravity = Gravity.CENTER })
            m.str("text").takeIf { it.isNotEmpty() }?.let { empty.addView(Kit.text(ctx, it, Theme.SP_BODY, Theme.TEXT_MUTED).apply { gravity = Gravity.CENTER }) }
            m["action"].obj().takeIf { it.isNotEmpty() }?.let { empty.addView(actionButton(e.id, it, true)) }
        }
        empty.visibility = if (show) View.VISIBLE else View.GONE
    }

    private fun gravityOf(c: Map<String, Any?>) = when (c.str("align", if (c.str("type") in setOf("number", "ping", "players", "size")) "end" else "start")) {
        "end" -> Gravity.END; "center" -> Gravity.CENTER_HORIZONTAL; else -> Gravity.START
    }

    private fun rebuildRow(r: LinearLayout, columns: List<Map<String, Any?>>, h: Int) {
        r.removeAllViews()
        r.setPadding(dp(12f), 0, dp(12f), 0)
        for (c in columns) {
            val t = Kit.text(ctx, "", Theme.SP_BODY, Theme.TEXT, lines = 1)
            t.gravity = gravityOf(c) or Gravity.CENTER_VERTICAL
            t.setPadding(dp(4f), 0, dp(4f), 0)
            r.addView(t, LinearLayout.LayoutParams(0, h, colWeight(c)))
        }
    }

    private fun bindCell(t: TextView, c: Map<String, Any?>, cells: Map<String, Any?>, placeholder: Boolean) {
        if (placeholder) { t.text = ""; t.setBackgroundColor(0x0FFFFFFF); return }
        t.background = null
        val key = c.str("key")
        t.typeface = if (c.str("width") == "fill") Theme.bold else Theme.regular
        when (c.str("type")) {
            "lock" -> { t.text = if (cells.bool(key)) "🔒" else ""; t.setTextColor(Theme.WARN) }
            "bool" -> { t.text = if (cells.bool(key)) "✓" else ""; t.setTextColor(Theme.GOOD) }
            "icon" -> { t.text = Icons.glyph(cells.str(key)); t.setTextColor(Theme.TEXT) }
            "ping" -> {
                val p = cells.int(key)
                t.text = if (p <= 0) "–" else "$p"
                t.setTextColor(when { p <= 0 -> Theme.TEXT_DIM; p < 80 -> Theme.GOOD; p < 160 -> Theme.WARN; else -> Theme.BAD })
            }
            "players" -> {
                val n = cells.int(key); val mx = cells.int("max")
                t.text = if (mx > 0) "$n/$mx" else "$n"
                t.setTextColor(if (n == 0) Theme.TEXT_DIM else Theme.TEXT)
            }
            "mode" -> { t.text = cells.str("modeName").ifEmpty { cells.str(key).uppercase() }; t.setTextColor(Theme.TEXT_MUTED) }
            else -> { t.text = cells.str(key); t.setTextColor(if (c.str("width") == "fill") Theme.TEXT else Theme.TEXT_MUTED) }
        }
    }
}

/**
 * A screen's page. A list scrolling inside it (a `rows: N` map grid, as tall
 * as the page's visible area) gets the drag only once the page has brought
 * it to the top: until then the page scrolls, so the grid is never scrolled
 * half off screen. Coming back up the grid scrolls to its start first
 * (the platform's nested scrolling), then the page.
 */
class PageScroll(ctx: Context) : ScrollView(ctx) {
    init { isNestedScrollingEnabled = true }

    /** The pinned section title's height: a list stops under it. */
    var topInset = 0

    private fun topOf(target: View): Int {
        var y = 0
        var v: View? = target
        while (v != null && v !== this) { y += v.top; v = v.parent as? View }
        return y - scrollY - topInset
    }

    override fun onStartNestedScroll(child: View, target: View, axes: Int): Boolean =
        (axes and View.SCROLL_AXIS_VERTICAL) != 0

    override fun onNestedPreScroll(target: View, dx: Int, dy: Int, consumed: IntArray) {
        if (dy > 0) {
            val gap = topOf(target)
            if (gap > 0 && canScrollVertically(1)) {
                val d = minOf(dy, gap)
                scrollBy(0, d)
                consumed[1] = d
            }
        }
    }

    override fun onNestedPreFling(target: View, velocityX: Float, velocityY: Float): Boolean {
        if (velocityY > 0 && topOf(target) > 0 && canScrollVertically(1)) { fling(velocityY.toInt()); return true }
        return false
    }
}

/** Children side by side at a fixed card width, for horizontal carousels. */
class CarouselRow(ctx: Context, private val item: Int, private val gap: Int) : ViewGroup(ctx) {
    override fun onMeasure(w: Int, h: Int) {
        var x = 0; var hh = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            c.measure(MeasureSpec.makeMeasureSpec(item, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
            x += item + gap; hh = max(hh, c.measuredHeight)
        }
        setMeasuredDimension(max(0, x - gap), hh)
    }
    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        var x = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            c.layout(x, 0, x + item, c.measuredHeight)
            x += item + gap
        }
    }
}

/**
 * Lays children out in equal columns, as many as fit at [minChild] px
 * (or, with [wrap], at their own widths like words in a line).
 */
class FlowGrid(ctx: Context, private val minChild: Int, private val gap: Int, private val wrap: Boolean = false) : ViewGroup(ctx) {
    /** Every child in one row (Home's cards), however narrow. */
    var singleRow = false
    override fun onMeasure(wSpec: Int, hSpec: Int) {
        val width = MeasureSpec.getSize(wSpec)
        if (wrap) {
            var x = 0; var y = 0; var line = 0
            for (i in 0 until childCount) {
                val c = getChildAt(i)
                if (c.visibility == GONE) continue
                c.measure(MeasureSpec.makeMeasureSpec(width, MeasureSpec.AT_MOST), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
                if (x > 0 && x + c.measuredWidth > width) { x = 0; y += line + gap; line = 0 }
                x += c.measuredWidth + gap
                line = max(line, c.measuredHeight)
            }
            setMeasuredDimension(width, y + line)
            return
        }
        val visible = (0 until childCount).count { getChildAt(it).visibility != GONE }
        val cols = if (singleRow) max(1, visible) else max(1, (width + gap) / (max(1, minChild) + gap))
        val cw = (width - gap * (cols - 1)) / cols
        var y = 0; var rowH = 0; var col = 0
        for (i in 0 until childCount) {
            val c = getChildAt(i)
            if (c.visibility == GONE) continue
            c.measure(MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED))
            rowH = max(rowH, c.measuredHeight)
            if (++col == cols) { y += rowH + gap; rowH = 0; col = 0 }
        }
        setMeasuredDimension(width, if (col == 0) max(0, y - gap) else y + rowH)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val width = r - l
        val rtl = layoutDirection == LAYOUT_DIRECTION_RTL
        if (wrap) {
            var x = 0; var y = 0; var line = 0
            for (i in 0 until childCount) {
                val c = getChildAt(i)
                if (c.visibility == GONE) continue
                if (x > 0 && x + c.measuredWidth > width) { x = 0; y += line + gap; line = 0 }
                val left = if (rtl) width - x - c.measuredWidth else x
                c.layout(left, y, left + c.measuredWidth, y + c.measuredHeight)
                x += c.measuredWidth + gap
                line = max(line, c.measuredHeight)
            }
            return
        }
        val visible = (0 until childCount).map { getChildAt(it) }.filter { it.visibility != GONE }
        val cols = if (singleRow) max(1, visible.size) else max(1, (width + gap) / (max(1, minChild) + gap))
        val cw = (width - gap * (cols - 1)) / cols
        var y = 0
        var i = 0
        while (i < visible.size) {
            val row = visible.subList(i, minOf(i + cols, visible.size))
            val rowH = row.maxOf { it.measuredHeight }
            for ((k, c) in row.withIndex()) {
                val x = k * (cw + gap)
                val left = if (rtl) width - x - cw else x
                c.layout(left, y, left + cw, y + rowH)
            }
            y += rowH + gap
            i += cols
        }
    }
}

