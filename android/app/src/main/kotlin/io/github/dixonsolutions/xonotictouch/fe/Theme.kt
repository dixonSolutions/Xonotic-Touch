package io.github.dixonsolutions.xonotictouch.fe

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.RippleDrawable
import android.graphics.drawable.StateListDrawable
import android.provider.Settings
import android.util.TypedValue
import android.view.View

/**
 * Godnotic's look, from the banner: black behind everything, deep navy
 * panels, electric blue outlines and glow, ember orange for the one thing
 * that matters most on a screen. Built in code, never from res/ (HostRes:
 * under AndySU the resource table is the installed host's).
 */
object Theme {
    const val BG = 0xFF000000.toInt()
    const val BG_SCRIM = 0xE6000000.toInt()
    const val PANEL = 0xFF0B1730.toInt()       // deep navy
    const val PANEL_HI = 0xFF12234A.toInt()
    const val PANEL_PRESSED = 0xFF1A3266.toInt()
    const val OUTLINE = 0xFF2B5DB8.toInt()     // electric blue, quiet
    const val BLUE = 0xFF3A8DFF.toInt()        // electric blue, lit
    const val BLUE_GLOW = 0x553A8DFF
    const val EMBER = 0xFFFF8A2A.toInt()       // the eagle's orange
    const val EMBER_DARK = 0xFFC2551A.toInt()
    const val TEXT = 0xFFEAF1FF.toInt()
    const val TEXT_MUTED = 0xFF9DB0D0.toInt()
    const val TEXT_DIM = 0xFF6F82A6.toInt()
    const val GOOD = 0xFF5BD68A.toInt()
    const val BAD = 0xFFFF6B6B.toInt()
    const val WARN = 0xFFFFC870.toInt()
    const val RIPPLE = 0x553A8DFF

    // Type scale, in sp so the system font size applies. 16 sp+ for body.
    const val SP_CAPTION = 14f
    const val SP_BODY = 17f
    const val SP_TITLE = 20f
    const val SP_HEADLINE = 26f
    const val SP_DISPLAY = 34f

    val bold: Typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
    val regular: Typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
    val condensed: Typeface = Typeface.create("sans-serif-condensed", Typeface.BOLD)

    /**
     * The game's display face for headings: Xolonium, which the game data
     * unpacks to <basedir>/data/font-xolonium.pk3dir (FrontendHost loads it);
     * the system bold until then.
     */
    @Volatile var display: Typeface = bold

    fun loadDisplayFont(baseDir: java.io.File?) {
        val f = java.io.File(baseDir ?: return, "data/font-xolonium.pk3dir/fonts/xolonium-bold.otf")
        if (f.isFile) try { display = Typeface.createFromFile(f) } catch (e: RuntimeException) {}
    }

    fun dp(ctx: Context, v: Float): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, ctx.resources.displayMetrics).toInt()
    fun dp(v: View, x: Float) = dp(v.context, x)

    /** Animations off in the system settings: no shimmer, no slides, cuts instead. */
    fun reducedMotion(ctx: Context): Boolean = try {
        Settings.Global.getFloat(ctx.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
    } catch (e: Exception) { false }

    fun rounded(color: Int, radius: Float, stroke: Int = 0, strokeColor: Int = 0): GradientDrawable =
        GradientDrawable().apply {
            setColor(color)
            cornerRadius = radius
            if (stroke > 0) setStroke(stroke, strokeColor)
        }

    /**
     * A control's background: the fill, a blue outline that lights up when
     * it has keyboard/D-pad focus, and the platform ripple on touch.
     */
    fun surface(ctx: Context, fill: Int, radiusDp: Float, outline: Int = OUTLINE, focusOutline: Int = BLUE): Drawable {
        val r = dp(ctx, radiusDp).toFloat()
        val stroke = maxOf(1, dp(ctx, 1f))
        val states = StateListDrawable()
        states.addState(intArrayOf(android.R.attr.state_focused), rounded(PANEL_HI, r, dp(ctx, 3f), focusOutline))
        states.addState(intArrayOf(android.R.attr.state_selected), rounded(PANEL_HI, r, dp(ctx, 2f), EMBER))
        states.addState(intArrayOf(android.R.attr.state_activated), rounded(PANEL_HI, r, dp(ctx, 2f), BLUE))
        states.addState(intArrayOf(), rounded(fill, r, stroke, outline))
        val mask = rounded(Color.WHITE, r)
        return RippleDrawable(ColorStateList.valueOf(RIPPLE), states, mask)
    }

    fun pill(ctx: Context, primary: Boolean): Drawable {
        val r = dp(ctx, 28f).toFloat()
        val states = StateListDrawable()
        val fill = if (primary) EMBER else PANEL_HI
        val outline = if (primary) EMBER_DARK else OUTLINE
        states.addState(intArrayOf(-android.R.attr.state_enabled), rounded(0xFF1A2234.toInt(), r, dp(ctx, 1f), 0xFF2A3342.toInt()))
        states.addState(intArrayOf(android.R.attr.state_focused), rounded(fill, r, dp(ctx, 3f), if (primary) Color.WHITE else BLUE))
        states.addState(intArrayOf(), rounded(fill, r, dp(ctx, 1f), outline))
        return RippleDrawable(ColorStateList.valueOf(if (primary) 0x66FFFFFF else RIPPLE), states, rounded(Color.WHITE, r))
    }
}
