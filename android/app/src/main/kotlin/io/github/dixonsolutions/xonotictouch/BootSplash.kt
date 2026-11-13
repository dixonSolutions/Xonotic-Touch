package io.github.dixonsolutions.xonotictouch

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.FrameLayout
import android.widget.ImageView

/**
 * The Godnotic banner on the game's dark background, from the first boot
 * screen until the engine has drawn its first frame, so a start never shows
 * black in between.
 *
 * The banner is an asset (copied from branding/ by build.gradle), never an
 * R.drawable: under AndySU the resource table is the installed host's, the
 * assets are this build's (see HostRes).
 */
object BootSplash {
    private const val TAG = "Godnotic"
    const val BACKGROUND = 0xFF000000.toInt() // the engine loading picture's black: no seam
    private const val ASSET = "xt/godnotic-banner.png"
    /** Longest the cover waits for the engine before it lifts anyway. */
    private const val COVER_MAX_MS = 25_000L

    private var banner: Bitmap? = null

    /** The banner scaled for a screen about [width] px wide; null when the asset is missing. */
    @JvmStatic
    @Synchronized
    fun banner(activity: Activity, width: Int): Bitmap? {
        banner?.let { return it }
        return try {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            activity.assets.open(ASSET).use { BitmapFactory.decodeStream(it, null, bounds) }
            var sample = 1
            while (bounds.outWidth / (sample * 2) >= width) sample *= 2
            val opts = BitmapFactory.Options().apply { inSampleSize = sample }
            activity.assets.open(ASSET).use { BitmapFactory.decodeStream(it, null, opts) }.also { banner = it }
        } catch (e: Exception) {
            Log.w(TAG, "No boot banner: ${e.message}")
            null
        }
    }

    /** A full-screen dark view with the banner centred at [fraction] of the width (at most [maxDp] dp). */
    @JvmStatic
    fun view(activity: Activity, fraction: Float, maxDp: Int): FrameLayout {
        val metrics = activity.resources.displayMetrics
        val frame = FrameLayout(activity)
        frame.setBackgroundColor(BACKGROUND)
        val width = minOf((metrics.widthPixels * fraction).toInt(), (maxDp * metrics.density).toInt())
        val bitmap = banner(activity, width)
        if (bitmap != null) {
            val image = ImageView(activity)
            image.setImageBitmap(bitmap)
            image.adjustViewBounds = true
            image.scaleType = ImageView.ScaleType.FIT_CENTER
            image.contentDescription = "Godnotic"
            frame.addView(image, FrameLayout.LayoutParams(width, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        }
        return frame
    }

    /**
     * Over the SDL surface of [activity] until the engine reports its first
     * buffer swap (NativeBridge.framePresented), then fades out. Without the
     * bridge nothing would report it, so nothing is added.
     */
    @JvmStatic
    fun coverUntilFirstFrame(activity: Activity) {
        if (!NativeBridge.available) return
        val cover = view(activity, 0.62f, 900)
        cover.isClickable = true
        activity.addContentView(cover, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        val handler = Handler(Looper.getMainLooper())
        val started = SystemClock.elapsedRealtime()
        handler.post(object : Runnable {
            override fun run() {
                if (activity.isFinishing || activity.isDestroyed) return
                val waited = SystemClock.elapsedRealtime() - started
                if (NativeBridge.framePresented() || waited > COVER_MAX_MS) {
                    Log.i(TAG, "Boot cover lifted after $waited ms" + if (waited > COVER_MAX_MS) " (no frame reported)" else "")
                    cover.animate().alpha(0f).setDuration(180).withEndAction {
                        (cover.parent as? ViewGroup)?.removeView(cover)
                    }.start()
                    return
                }
                handler.postDelayed(this, 30)
            }
        })
    }

    /**
     * Tells the engine which part of the window display cutouts and rounded
     * corners cover, and the display density, as `_touch_safe_insets "left top
     * right bottom density"` in window pixels (touch_shellbtn.c places the
     * console button inside it). Through the engine's command queue: nothing
     * touches the engine from this thread.
     */
    @JvmStatic
    fun reportSafeInsets(activity: Activity, insets: WindowInsets) {
        var l = 0; var t = 0; var r = 0; var b = 0
        if (Build.VERSION.SDK_INT >= 28) {
            insets.displayCutout?.let { c ->
                l = c.safeInsetLeft; t = c.safeInsetTop; r = c.safeInsetRight; b = c.safeInsetBottom
            }
        }
        if (Build.VERSION.SDK_INT >= 31) {
            // A rounded corner eats into the top right too.
            for (pos in intArrayOf(android.view.RoundedCorner.POSITION_TOP_RIGHT)) {
                val corner = insets.getRoundedCorner(pos) ?: continue
                val inset = (corner.radius * 0.3f).toInt()
                r = maxOf(r, inset); t = maxOf(t, inset)
            }
        }
        val density = activity.resources.displayMetrics.density
        val value = "$l $t $r $b ${"%.3f".format(java.util.Locale.US, density)}"
        if (value == lastInsets) return
        if (NativeBridge.enqueue("set _touch_safe_insets \"$value\"")) {
            lastInsets = value
            Log.i(TAG, "Safe insets for the engine: $value")
        }
    }

    @Volatile private var lastInsets: String? = null
}
