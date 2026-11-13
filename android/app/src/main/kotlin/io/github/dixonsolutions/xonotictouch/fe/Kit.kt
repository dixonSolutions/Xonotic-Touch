package io.github.dixonsolutions.xonotictouch.fe

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Shader
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.text.TextUtils
import android.util.LruCache
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView
import java.io.File
import java.util.concurrent.Executors

/**
 * The small view toolkit the renderer builds from: plain platform views
 * styled in code (Theme), 48 dp touch targets, ripple and focus states,
 * TalkBack labels, start/end (RTL-safe) gravity, sizes in sp.
 */
object Kit {
    const val MIN_TARGET_DP = 48f

    fun text(ctx: Context, s: CharSequence, sp: Float = Theme.SP_BODY, color: Int = Theme.TEXT, bold: Boolean = false, lines: Int = 0): TextView =
        TextView(ctx).apply {
            text = s
            textSize = sp
            setTextColor(color)
            typeface = if (bold) Theme.bold else Theme.regular
            textAlignment = View.TEXT_ALIGNMENT_VIEW_START
            if (lines > 0) {
                maxLines = lines
                ellipsize = TextUtils.TruncateAt.END
            }
        }

    fun haptic(v: View) {
        v.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
    }

    /** An icon from the set (Icons) before the text, tinted like it; nothing for an unknown name. */
    fun icon(t: TextView, name: String, color: Int, sizeDp: Float = 22f) {
        val d = if (name.isEmpty()) null else Icons.drawable(name, Theme.dp(t.context, sizeDp), color)
        t.setCompoundDrawablesRelative(d, null, null, null)
        t.compoundDrawablePadding = Theme.dp(t.context, 10f)
    }

    /** A quiet header action: no fill, muted text and icon, still a 48 dp target. */
    fun quiet(ctx: Context, label: String, icon: String, onClick: (View) -> Unit): TextView = TextView(ctx).apply {
        text = label
        textSize = Theme.SP_BODY
        typeface = Theme.bold
        setTextColor(Theme.TEXT_MUTED)
        gravity = Gravity.CENTER
        minHeight = Theme.dp(ctx, MIN_TARGET_DP)
        val h = Theme.dp(ctx, 14f)
        setPadding(h, 0, h, 0)
        background = Theme.surface(ctx, Color.TRANSPARENT, 24f, Color.TRANSPARENT)
        isFocusable = true
        isClickable = true
        contentDescription = label
        icon(this, icon, Theme.TEXT_MUTED, 20f)
        setOnClickListener { haptic(it); onClick(it) }
    }

    /** A pill button: primary in ember, the rest navy with a blue outline. */
    fun button(ctx: Context, label: String, primary: Boolean = false, iconName: String = "", onClick: (View) -> Unit): TextView =
        TextView(ctx).apply {
            text = label
            if (iconName.isNotEmpty()) icon(this, iconName, if (primary) 0xFF1A0A00.toInt() else Theme.TEXT)
            textSize = Theme.SP_BODY
            typeface = Theme.bold
            setTextColor(if (primary) 0xFF1A0A00.toInt() else Theme.TEXT)
            gravity = Gravity.CENTER
            minHeight = Theme.dp(ctx, MIN_TARGET_DP)
            minimumWidth = Theme.dp(ctx, 96f)
            val h = Theme.dp(ctx, 22f)
            setPadding(h, Theme.dp(ctx, 10f), h, Theme.dp(ctx, 10f))
            background = Theme.pill(ctx, primary)
            isFocusable = true
            isClickable = true
            contentDescription = label
            setOnClickListener { haptic(it); onClick(it) }
        }

    /** A rounded navy panel with an outline; clickable ones get a ripple and focus ring. */
    fun card(ctx: Context, clickable: Boolean, radiusDp: Float = 16f): FrameLayout =
        FrameLayout(ctx).apply {
            background = if (clickable) Theme.surface(ctx, Theme.PANEL, radiusDp)
            else Theme.rounded(Theme.PANEL, Theme.dp(ctx, radiusDp).toFloat(), Theme.dp(ctx, 1f), Theme.OUTLINE)
            isClickable = clickable
            isFocusable = clickable
            elevation = Theme.dp(ctx, 3f).toFloat()
            clipToOutline = true
            outlineProvider = android.view.ViewOutlineProvider.BACKGROUND
        }

    fun column(ctx: Context, spacingDp: Float = 0f): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.VERTICAL
        if (spacingDp > 0) {
            showDividers = LinearLayout.SHOW_DIVIDER_MIDDLE
            dividerDrawable = android.graphics.drawable.ShapeDrawable().apply {
                intrinsicHeight = Theme.dp(ctx, spacingDp); paint.color = Color.TRANSPARENT
            }
        }
    }

    fun row(ctx: Context, spacingDp: Float = 0f): LinearLayout = LinearLayout(ctx).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
        if (spacingDp > 0) {
            showDividers = LinearLayout.SHOW_DIVIDER_MIDDLE
            dividerDrawable = android.graphics.drawable.ShapeDrawable().apply {
                intrinsicWidth = Theme.dp(ctx, spacingDp); paint.color = Color.TRANSPARENT
            }
        }
    }

    /** A form row: label (and optional help) on the start side, the control on the end side, 48 dp+ tall. */
    fun formRow(ctx: Context, label: String, help: String, control: View?, onClick: ((View) -> Unit)? = null): LinearLayout {
        val row = row(ctx, 16f)
        row.minimumHeight = Theme.dp(ctx, 56f)
        val p = Theme.dp(ctx, 16f)
        row.setPadding(p, Theme.dp(ctx, 8f), p, Theme.dp(ctx, 8f))
        val texts = column(ctx)
        texts.addView(text(ctx, label, Theme.SP_BODY, Theme.TEXT, bold = true))
        if (help.isNotEmpty()) texts.addView(text(ctx, help, Theme.SP_CAPTION, Theme.TEXT_MUTED))
        row.addView(texts, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (control != null) row.addView(control)
        if (onClick != null) {
            row.background = Theme.surface(ctx, Color.TRANSPARENT, 12f, Color.TRANSPARENT)
            row.isClickable = true
            row.isFocusable = true
            row.setOnClickListener { haptic(it); onClick(it) }
        }
        row.contentDescription = if (help.isEmpty()) label else "$label. $help"
        return row
    }

    fun switch(ctx: Context, on: Boolean, label: String, onChange: (Boolean) -> Unit): Switch = Switch(ctx).apply {
        isChecked = on
        thumbTintList = android.content.res.ColorStateList(
            arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(Theme.EMBER, 0xFFB8C4DA.toInt()))
        trackTintList = android.content.res.ColorStateList(
            arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()), intArrayOf(0xAAFF8A2A.toInt(), 0xFF33415C.toInt()))
        minHeight = Theme.dp(ctx, MIN_TARGET_DP)
        contentDescription = label
        setOnCheckedChangeListener { v, checked -> haptic(v); onChange(checked) }
    }

    /** A slider over [min, max] in [step]s, with the value shown beside it. */
    fun slider(ctx: Context, value: Double, min: Double, max: Double, step: Double, label: String, format: (Double) -> String, onChange: (Double, Boolean) -> Unit): LinearLayout {
        val box = row(ctx, 12f)
        val steps = maxOf(1, Math.round((max - min) / (if (step > 0) step else (max - min) / 100)).toInt())
        val shown = text(ctx, format(value), Theme.SP_BODY, Theme.TEXT, bold = true)
        shown.minWidth = Theme.dp(ctx, 56f)
        shown.gravity = Gravity.END
        val bar = SeekBar(ctx).apply {
            this.max = steps
            progress = Math.round((value - min) / (max - min) * steps).toInt().coerceIn(0, steps)
            progressTintList = android.content.res.ColorStateList.valueOf(Theme.BLUE)
            thumbTintList = android.content.res.ColorStateList.valueOf(Theme.EMBER)
            minimumHeight = Theme.dp(ctx, MIN_TARGET_DP)
            contentDescription = label
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                fun v(p: Int) = min + (max - min) * p / steps
                override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                    if (!fromUser) return
                    shown.text = format(v(p)); onChange(v(p), false)
                }
                override fun onStartTrackingTouch(s: SeekBar) {}
                override fun onStopTrackingTouch(s: SeekBar) { onChange(v(s.progress), true) }
            })
        }
        box.addView(bar, LinearLayout.LayoutParams(Theme.dp(ctx, 220f), ViewGroup.LayoutParams.WRAP_CONTENT))
        box.addView(shown)
        return box
    }

    fun input(ctx: Context, value: String, hint: String, password: Boolean = false, onDone: (String) -> Unit): EditText = EditText(ctx).apply {
        setText(value)
        this.hint = hint
        setHintTextColor(Theme.TEXT_DIM)
        setTextColor(Theme.TEXT)
        textSize = Theme.SP_BODY
        isSingleLine = true
        minHeight = Theme.dp(ctx, MIN_TARGET_DP)
        val p = Theme.dp(ctx, 14f)
        setPadding(p, 0, p, 0)
        background = Theme.surface(ctx, 0xFF060E1F.toInt(), 12f)
        inputType = if (password) InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD else InputType.TYPE_CLASS_TEXT
        imeOptions = EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_FULLSCREEN
        contentDescription = hint
        setOnEditorActionListener { v, action, _ ->
            if (action == EditorInfo.IME_ACTION_DONE) { onDone(v.text.toString()); false } else false
        }
        setOnFocusChangeListener { v, has -> if (!has) onDone((v as EditText).text.toString()) }
    }

    /** A segmented control: one choice out of a few, as connected pills. */
    fun segmented(ctx: Context, options: List<Pair<String, String>>, selected: String, onSelect: (String) -> Unit): LinearLayout {
        val bar = row(ctx, 4f)
        bar.background = Theme.rounded(0xFF060E1F.toInt(), Theme.dp(ctx, 26f).toFloat(), Theme.dp(ctx, 1f), Theme.OUTLINE)
        val p = Theme.dp(ctx, 4f)
        bar.setPadding(p, p, p, p)
        val views = ArrayList<TextView>()
        for ((value, label) in options) {
            val t = text(ctx, label, Theme.SP_BODY, Theme.TEXT, bold = true)
            t.gravity = Gravity.CENTER
            t.minHeight = Theme.dp(ctx, 44f)
            t.minWidth = Theme.dp(ctx, 96f)
            val h = Theme.dp(ctx, 18f)
            t.setPadding(h, 0, h, 0)
            t.isClickable = true
            t.isFocusable = true
            t.contentDescription = label
            t.tag = value
            views.add(t)
            bar.addView(t)
        }
        fun paint(sel: String) {
            for (v in views) {
                val on = v.tag == sel
                v.isSelected = on
                v.background = if (on) Theme.rounded(Theme.BLUE, Theme.dp(ctx, 22f).toFloat()) else Theme.surface(ctx, Color.TRANSPARENT, 22f, Color.TRANSPARENT)
                v.setTextColor(if (on) Color.WHITE else Theme.TEXT_MUTED)
            }
        }
        paint(selected)
        for (v in views) v.setOnClickListener { haptic(it); paint(it.tag as String); onSelect(it.tag as String) }
        return bar
    }

    /** A thin sweeping highlight over a placeholder while its picture loads; still when motion is reduced. */
    class Shimmer(ctx: Context) : View(ctx) {
        private val paint = Paint()
        private var phase = 0f
        private val anim: ValueAnimator? = if (Theme.reducedMotion(ctx)) null else ValueAnimator.ofFloat(0f, 1f).apply {
            duration = 1100
            repeatCount = ValueAnimator.INFINITE
            addUpdateListener { phase = it.animatedValue as Float; invalidate() }
        }
        override fun onAttachedToWindow() { super.onAttachedToWindow(); anim?.start() }
        override fun onDetachedFromWindow() { anim?.cancel(); super.onDetachedFromWindow() }
        override fun onDraw(c: Canvas) {
            c.drawColor(0xFF0E1C3A.toInt())
            if (anim == null) return
            val w = width.toFloat()
            val x = -w + phase * 2 * w
            paint.shader = LinearGradient(x, 0f, x + w, 0f, intArrayOf(0x00FFFFFF, 0x22FFFFFF, 0x00FFFFFF), null, Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, w, height.toFloat(), paint)
        }
    }
}

/**
 * Pictures for cards and previews, decoded off the UI thread at the size
 * they are shown, kept in memory (LRU, an eighth of the heap) and as small
 * JPEG thumbnails on disk, so a grid of maps scrolls without decoding the
 * 512 px originals again.
 */
object Images {
    private val cache = object : LruCache<String, Bitmap>((Runtime.getRuntime().maxMemory() / 8 / 1024).toInt()) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount / 1024
    }
    private val pool = Executors.newFixedThreadPool(2) { r -> Thread(r, "godnotic-images").apply { isDaemon = true; priority = Thread.MIN_PRIORITY } }
    private val main = Handler(Looper.getMainLooper())
    var thumbDir: File? = null

    fun cached(path: String, widthPx: Int): Bitmap? = cache.get("$path@${bucket(widthPx)}")

    private fun bucket(w: Int) = when { w <= 256 -> 256; w <= 512 -> 512; else -> 1024 }

    fun load(path: String, widthPx: Int, done: (Bitmap?) -> Unit) {
        val key = "$path@${bucket(widthPx)}"
        cache.get(key)?.let { done(it); return }
        pool.execute {
            val bmp = try { decode(path, bucket(widthPx)) } catch (e: Exception) { null }
            if (bmp != null) cache.put(key, bmp)
            main.post { done(bmp) }
        }
    }

    private fun decode(path: String, width: Int): Bitmap? {
        val src = File(path)
        if (!src.isFile) return null
        val thumb = thumbDir?.let { File(it, "${src.name}.${src.length()}.${src.lastModified()}.$width.jpg") }
        if (thumb != null && thumb.isFile) BitmapFactory.decodeFile(thumb.path)?.let { return it }
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeFile(path, bounds)
        var sample = 1
        while (bounds.outWidth / (sample * 2) >= width) sample *= 2
        val bmp = BitmapFactory.decodeFile(path, BitmapFactory.Options().apply { inSampleSize = sample }) ?: return null
        val scaled = if (bmp.width > width) Bitmap.createScaledBitmap(bmp, width, bmp.height * width / bmp.width, true) else bmp
        if (thumb != null) try {
            thumb.parentFile?.mkdirs()
            thumb.outputStream().use { scaled.compress(Bitmap.CompressFormat.JPEG, 85, it) }
        } catch (e: Exception) { /* the cache is a convenience */ }
        return scaled
    }
}

/** A picture with rounded corners that fills its box (centre crop), shimmering until it has one. */
class RoundImage(ctx: Context, private val radiusDp: Float = 12f) : FrameLayout(ctx) {
    private val shimmer = Kit.Shimmer(ctx)
    private var bitmap: Bitmap? = null
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val clip = Path()
    private val m = Matrix()
    var aspect = 16f / 9f
    /** fit "contain": the whole picture inside the box (the banner); else cover (crop). */
    var contain = false
    /** A cap on the height (px) whatever the aspect: Home must fit a landscape screen. */
    var maxHeight = 0
    /** Fade the picture's start side into this colour (the hero's scrim); 0: none. */
    var fadeInto = 0
    private val fadePaint = Paint()

    init {
        setWillNotDraw(false)
        addView(shimmer, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
    }

    fun set(b: Bitmap?) {
        bitmap = b
        shimmer.visibility = if (b == null) VISIBLE else GONE
        invalidate()
    }

    /** No picture at all: the placeholder stops shimmering and stays a plain panel. */
    fun none() { shimmer.visibility = GONE; bitmap = null; invalidate() }

    override fun onMeasure(w: Int, h: Int) {
        val width = MeasureSpec.getSize(w)
        if (MeasureSpec.getMode(h) != MeasureSpec.EXACTLY && width > 0) {
            var hh = (width / aspect).toInt()
            if (maxHeight > 0 && hh > maxHeight) hh = maxHeight
            super.onMeasure(w, MeasureSpec.makeMeasureSpec(hh, MeasureSpec.EXACTLY))
        }
        else super.onMeasure(w, h)
    }

    override fun dispatchDraw(c: Canvas) {
        val r = Theme.dp(context, radiusDp).toFloat()
        clip.reset()
        clip.addRoundRect(RectF(0f, 0f, width.toFloat(), height.toFloat()), r, r, Path.Direction.CW)
        c.save()
        c.clipPath(clip)
        if (!contain) c.drawColor(0xFF0E1C3A.toInt())
        bitmap?.let { b ->
            val s = if (contain) minOf(width.toFloat() / b.width, height.toFloat() / b.height) else maxOf(width.toFloat() / b.width, height.toFloat() / b.height)
            m.setScale(s, s)
            m.postTranslate((width - b.width * s) / 2, (height - b.height * s) / 2)
            c.drawBitmap(b, m, paint)
        }
        if (fadeInto != 0 && bitmap != null) {
            val w = width * 0.55f
            fadePaint.shader = LinearGradient(0f, 0f, w, 0f, intArrayOf(fadeInto, fadeInto and 0x00FFFFFF), null, Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, w, height.toFloat(), fadePaint)
        }
        super.dispatchDraw(c)
        c.restore()
    }
}
