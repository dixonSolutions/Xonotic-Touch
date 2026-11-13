package io.github.dixonsolutions.xonotictouch.fe

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Rect
import android.os.Parcelable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import androidx.recyclerview.widget.GridLayoutManager
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import kotlin.math.max

/**
 * Virtualised lists and grids (RecyclerView) over a [ListWindow]: rows are
 * views recycled as they scroll, ids are the bridge's row keys (stable, so a
 * refresh keeps the item under the finger where it was), rows not loaded yet
 * are placeholder rows and the window around the viewport is asked for as it
 * approaches, pictures load only for bound rows and are dropped when a row is
 * recycled. Rows that carry "section" get a header that sticks to the top of
 * the list while its section scrolls under it.
 */
class VirtualList(
    private val ctx: Context,
    val id: String,
    private val request: (IntRange) -> Unit,
    pageSize: Int,
    private val grid: Boolean,
    private val minCellPx: Int,
    private val bindCell: (holder: Cell, row: Map<String, Any?>?, position: Int) -> Unit,
    private val makeCell: (parent: ViewGroup) -> Cell,
) {
    val window = ListWindow(id, pageSize)
    val view = RecyclerView(ctx)
    private val adapter = Adapter()
    private var spans = 1

    abstract class Cell(v: View) : RecyclerView.ViewHolder(v) {
        open fun recycled() {}
    }

    init {
        view.setHasFixedSize(false)
        view.isVerticalScrollBarEnabled = true
        view.scrollBarStyle = View.SCROLLBARS_INSIDE_OVERLAY
        view.overScrollMode = View.OVER_SCROLL_IF_CONTENT_SCROLLS
        view.itemAnimator = null // rows are updated in place, never animated across
        view.setItemViewCacheSize(8)
        view.layoutManager = if (grid) GridLayoutManager(ctx, 1) else LinearLayoutManager(ctx)
        adapter.setHasStableIds(true)
        view.adapter = adapter
        if (grid) view.addOnLayoutChangeListener { _, l, _, r, _, _, _, _, _ ->
            val n = max(1, (r - l) / max(1, minCellPx))
            if (n != spans) { spans = n; (view.layoutManager as GridLayoutManager).spanCount = n }
        }
        view.addOnScrollListener(object : RecyclerView.OnScrollListener() {
            override fun onScrolled(rv: RecyclerView, dx: Int, dy: Int) = ask()
        })
        if (!grid) view.addItemDecoration(StickySections())
    }

    /** The window around what is on screen, asked for once (ListWindow keeps track). */
    fun ask() {
        val lm = view.layoutManager as LinearLayoutManager
        val first = max(0, lm.findFirstVisibleItemPosition())
        val last = max(first, lm.findLastVisibleItemPosition())
        val margin = window.pageSize / 2
        window.missing(max(0, first - margin), last + margin)?.let { request(it) }
    }

    /** New rows from the bridge: rebinds only the changed positions. */
    fun rows(offset: Int, total: Int, rows: List<Map<String, Any?>>) {
        val before = window.total
        window.accept(mapOf("offset" to offset.toDouble(), "total" to total.toDouble(), "rows" to rows))
        if (before != total) adapter.notifyDataSetChanged() // the count changed: positions stay keyed by id
        else adapter.notifyItemRangeChanged(offset, rows.size, PAYLOAD_ROW)
    }

    /** All rows at once (a small inline list), diffed against what is shown by key. */
    fun replaceAll(rows: List<Map<String, Any?>>) {
        val old = (0 until max(0, window.total)).map { window.row(it) }
        window.invalidate()
        window.accept(mapOf("offset" to 0.0, "total" to rows.size.toDouble(), "rows" to rows))
        val diff = UiDiff.rows(old.map { it?.let(::keyOf) }, rows.map(::keyOf), old.map { it?.hashCode() ?: 0 }, rows.map { it.hashCode() })
        if (diff == null) adapter.notifyDataSetChanged()
        else diff.dispatch(adapter)
    }

    fun reset(total: Int) {
        window.invalidate()
        window.accept(mapOf("offset" to 0.0, "total" to total.toDouble(), "rows" to emptyList<Any>()))
        adapter.notifyDataSetChanged()
        window.missing(0, window.pageSize)?.let { request(it) }
    }

    fun refresh() = adapter.notifyDataSetChanged()

    fun save(): Parcelable? = view.layoutManager?.onSaveInstanceState()
    fun restore(state: Parcelable?) { if (state != null) view.layoutManager?.onRestoreInstanceState(state) }

    private inner class Adapter : RecyclerView.Adapter<Cell>() {
        override fun getItemCount() = max(0, window.total)
        override fun getItemId(position: Int): Long {
            val r = window.row(position) ?: return -1L - position // a placeholder: its own id until it loads
            return keyOf(r).hashCode().toLong() and 0xffffffffL
        }
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) = makeCell(parent)
        override fun onBindViewHolder(holder: Cell, position: Int) {
            val row = window.row(position)
            bindCell(holder, row, position)
            if (row == null) window.missing(position, position + window.pageSize)?.let { request(it) }
        }
        override fun onViewRecycled(holder: Cell) = holder.recycled()
    }

    /** The current row's section label drawn pinned at the top, pushed up by the next section's first row. */
    private inner class StickySections : RecyclerView.ItemDecoration() {
        private val label = TextView(ctx).apply {
            textSize = Theme.SP_CAPTION
            setTextColor(Theme.BLUE)
            typeface = Theme.bold
            setBackgroundColor(0xF20B1730.toInt())
            gravity = Gravity.CENTER_VERTICAL
            setPadding(Theme.dp(ctx, 16f), 0, Theme.dp(ctx, 16f), 0)
        }
        private val h = Theme.dp(ctx, 30f)

        override fun getItemOffsets(out: Rect, v: View, parent: RecyclerView, state: RecyclerView.State) {
            val pos = parent.getChildAdapterPosition(v)
            if (pos == RecyclerView.NO_POSITION) return
            val s = sectionOf(window.row(pos))
            if (s.isNotEmpty() && (pos == 0 || sectionOf(window.row(pos - 1)) != s)) out.top = h
        }

        override fun onDrawOver(c: Canvas, parent: RecyclerView, state: RecyclerView.State) {
            if (parent.childCount == 0) return
            val top = parent.getChildAt(0)
            val pos = parent.getChildAdapterPosition(top)
            if (pos == RecyclerView.NO_POSITION) return
            // headers of sections starting on screen
            for (i in 0 until parent.childCount) {
                val v = parent.getChildAt(i)
                val p = parent.getChildAdapterPosition(v)
                if (p == RecyclerView.NO_POSITION) continue
                val s = sectionOf(window.row(p))
                if (s.isNotEmpty() && (p == 0 || sectionOf(window.row(p - 1)) != s)) draw(c, parent, s, v.top - h)
            }
            // and the pinned one
            val cur = sectionOf(window.row(pos))
            if (cur.isEmpty()) return
            var y = 0
            for (i in 1 until parent.childCount) {
                val v = parent.getChildAt(i)
                val p = parent.getChildAdapterPosition(v)
                val s = sectionOf(window.row(p))
                if (s != cur) { y = minOf(0, v.top - h - h); break }
            }
            draw(c, parent, cur, y)
        }

        private fun draw(c: Canvas, parent: RecyclerView, text: String, y: Int) {
            label.text = text
            label.measure(View.MeasureSpec.makeMeasureSpec(parent.width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(h, View.MeasureSpec.EXACTLY))
            label.layout(0, 0, parent.width, h)
            c.save()
            c.translate(0f, y.toFloat())
            label.draw(c)
            c.restore()
        }
    }

    companion object {
        const val PAYLOAD_ROW = "row"
        fun keyOf(r: Map<String, Any?>) = r.str("key").ifEmpty { r.hashCode().toString() }
        /** A row's section label (cells.section), for sticky section headers. */
        fun sectionOf(r: Map<String, Any?>?) = r?.get("cells").obj().str("section")
    }
}

/**
 * Row diff by key (Myers would be overkill for lists the bridge already
 * windows): unchanged keys in the same order are kept, changed contents are
 * rebound, anything else is an insert/remove. Plain Kotlin, unit tested.
 */
class UiDiff private constructor(val ops: List<Op>) {
    sealed class Op {
        data class Change(val pos: Int, val count: Int) : Op()
        data class Insert(val pos: Int, val count: Int) : Op()
        data class Remove(val pos: Int, val count: Int) : Op()
    }

    fun dispatch(a: RecyclerView.Adapter<*>) {
        for (op in ops) when (op) {
            is Op.Change -> a.notifyItemRangeChanged(op.pos, op.count, VirtualList.PAYLOAD_ROW)
            is Op.Insert -> a.notifyItemRangeInserted(op.pos, op.count)
            is Op.Remove -> a.notifyItemRangeRemoved(op.pos, op.count)
        }
    }

    companion object {
        /**
         * Old and new key sequences; null when keys are not unique (then the
         * caller rebinds everything). Common prefix and suffix are matched,
         * the middle becomes one remove and one insert; matched rows whose
         * contents hash differs are changes.
         */
        fun rows(old: List<String?>, new: List<String>, oldHash: List<Int>, newHash: List<Int>): UiDiff? {
            if (old.any { it == null } || old.toSet().size != old.size || new.toSet().size != new.size) return null
            var pre = 0
            while (pre < old.size && pre < new.size && old[pre] == new[pre]) pre++
            var suf = 0
            while (suf < old.size - pre && suf < new.size - pre && old[old.size - 1 - suf] == new[new.size - 1 - suf]) suf++
            val ops = ArrayList<Op>()
            fun changes(range: IntRange, shift: Int) {
                var start = -1
                for (i in range) {
                    val changed = oldHash[i] != newHash[i + shift]
                    if (changed && start < 0) start = i + shift
                    if (!changed && start >= 0) { ops.add(Op.Change(start, i + shift - start)); start = -1 }
                }
                if (start >= 0) ops.add(Op.Change(start, range.last + 1 + shift - start))
            }
            changes(0 until pre, 0)
            val removed = old.size - pre - suf
            val inserted = new.size - pre - suf
            if (removed > 0) ops.add(Op.Remove(pre, removed))
            if (inserted > 0) ops.add(Op.Insert(pre, inserted))
            changes((old.size - suf) until old.size, new.size - old.size)
            return UiDiff(ops)
        }
    }
}

/** Where a node goes inside a screen: pinned above, pinned below, filling the rest, or in the scrolling body. */
enum class Placement { TOP, BOTTOM, FILL, BODY }

object UiLayout {
    /**
     * docs/UI_BRIDGE.md 4.10: a screen's `toolbar` child is pinned under the
     * header, its `actionbar` child above the navigation area and the IME; a
     * `list` (or anything hinted fill) takes the space left and is its own
     * scroller (a RecyclerView); everything else scrolls in the body.
     */
    fun placement(parentType: String, type: String, hints: Map<String, Any?>, props: Map<String, Any?>): Placement {
        if (parentType != "screen") return Placement.BODY
        return when {
            type == "toolbar" -> Placement.TOP
            type == "actionbar" -> Placement.BOTTOM
            hints.bool("fill") || type == "list" -> Placement.FILL
            else -> Placement.BODY
        }
    }
}
