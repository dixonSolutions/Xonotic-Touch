package io.github.dixonsolutions.xonotictouch.fe

import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.drawable.Drawable

/**
 * Godnotic's icons: one set of line icons drawn as vectors on a 24-unit grid
 * with one stroke weight, tinted with the palette, mapped from the UI
 * bridge's semantic names (docs/UI_BRIDGE.md 5.1). No emoji or symbol fonts,
 * whose look changes from device to device. A name without a drawing draws
 * nothing (the spec's rule for unknown icons).
 */
object Icons {
    private val drawings: Map<String, (Path) -> Unit> = mapOf(
        "play" to { p -> p.moveTo(8f, 5f); p.lineTo(19f, 12f); p.lineTo(8f, 19f); p.close() },
        "resume" to { p -> p.addCircle(12f, 12f, 9f, Path.Direction.CW); p.moveTo(10f, 8f); p.lineTo(16f, 12f); p.lineTo(10f, 16f); p.close() },
        "pause" to { p -> p.moveTo(9f, 6f); p.lineTo(9f, 18f); p.moveTo(15f, 6f); p.lineTo(15f, 18f) },
        "singleplayer" to { p -> person(p, 12f, 1f) },
        "player" to { p -> person(p, 12f, 1f) },
        "multiplayer" to { p -> person(p, 9f, 0.85f); person(p, 16f, 0.85f) },
        "players" to { p -> person(p, 9f, 0.85f); person(p, 16f, 0.85f) },
        "settings" to { p ->
            p.addCircle(12f, 12f, 3.2f, Path.Direction.CW)
            for (i in 0 until 8) {
                val a = Math.toRadians(i * 45.0)
                val c = Math.cos(a).toFloat(); val s = Math.sin(a).toFloat()
                p.moveTo(12f + c * 6f, 12f + s * 6f); p.lineTo(12f + c * 9f, 12f + s * 9f)
            }
            p.addCircle(12f, 12f, 6f, Path.Direction.CW)
        },
        "credits" to { p -> info(p) },
        "info" to { p -> info(p) },
        "help" to { p -> p.addCircle(12f, 12f, 9f, Path.Direction.CW); p.moveTo(9.5f, 9.5f); p.cubicTo(9.5f, 6.5f, 14.5f, 6.5f, 14.5f, 9.5f); p.cubicTo(14.5f, 11.5f, 12f, 11.5f, 12f, 14f); p.moveTo(12f, 16.5f); p.lineTo(12f, 17.2f) },
        "quit" to { p -> p.moveTo(12f, 3.5f); p.lineTo(12f, 11.5f); p.addArc(RectF(4.5f, 5f, 19.5f, 20f), -60f, 300f) },
        "disconnect" to { p -> p.moveTo(10f, 5f); p.lineTo(5f, 5f); p.lineTo(5f, 19f); p.lineTo(10f, 19f); p.moveTo(9f, 12f); p.lineTo(20f, 12f); p.moveTo(16f, 8f); p.lineTo(20f, 12f); p.lineTo(16f, 16f) },
        "back" to { p -> p.moveTo(15f, 5f); p.lineTo(8f, 12f); p.lineTo(15f, 19f) },
        "chevron" to { p -> p.moveTo(9f, 5f); p.lineTo(16f, 12f); p.lineTo(9f, 19f) },
        "close" to { p -> p.moveTo(6f, 6f); p.lineTo(18f, 18f); p.moveTo(18f, 6f); p.lineTo(6f, 18f) },
        "add" to { p -> p.moveTo(12f, 5f); p.lineTo(12f, 19f); p.moveTo(5f, 12f); p.lineTo(19f, 12f) },
        "remove" to { p -> p.moveTo(5f, 12f); p.lineTo(19f, 12f) },
        "check" to { p -> p.moveTo(5f, 12.5f); p.lineTo(10f, 17.5f); p.lineTo(19f, 7f) },
        "search" to { p -> p.addCircle(10.5f, 10.5f, 5.5f, Path.Direction.CW); p.moveTo(14.5f, 14.5f); p.lineTo(20f, 20f) },
        "filter" to { p -> p.moveTo(4f, 6f); p.lineTo(20f, 6f); p.moveTo(7f, 12f); p.lineTo(17f, 12f); p.moveTo(10f, 18f); p.lineTo(14f, 18f) },
        "sort" to { p -> p.moveTo(8f, 5f); p.lineTo(8f, 19f); p.moveTo(5f, 16f); p.lineTo(8f, 19f); p.lineTo(11f, 16f); p.moveTo(16f, 19f); p.lineTo(16f, 5f); p.moveTo(13f, 8f); p.lineTo(16f, 5f); p.lineTo(19f, 8f) },
        "refresh" to { p -> p.addArc(RectF(5f, 5f, 19f, 19f), -30f, 300f); p.moveTo(19f, 4f); p.lineTo(18.1f, 8.5f); p.lineTo(13.6f, 7.6f) },
        "lock" to { p -> p.addRoundRect(RectF(5f, 11f, 19f, 20f), 2f, 2f, Path.Direction.CW); p.moveTo(8f, 11f); p.lineTo(8f, 8f); p.cubicTo(8f, 3f, 16f, 3f, 16f, 8f); p.lineTo(16f, 11f) },
        "unlock" to { p -> p.addRoundRect(RectF(5f, 11f, 19f, 20f), 2f, 2f, Path.Direction.CW); p.moveTo(8f, 11f); p.lineTo(8f, 8f); p.cubicTo(8f, 3f, 16f, 3f, 16f, 6.5f) },
        "star" to { p -> star(p) },
        "star_filled" to { p -> star(p) },
        "map" to { p -> p.moveTo(3f, 6f); p.lineTo(9f, 4f); p.lineTo(15f, 6f); p.lineTo(21f, 4f); p.lineTo(21f, 18f); p.lineTo(15f, 20f); p.lineTo(9f, 18f); p.lineTo(3f, 20f); p.close(); p.moveTo(9f, 4f); p.lineTo(9f, 18f); p.moveTo(15f, 6f); p.lineTo(15f, 20f) },
        "mode" to { p -> pad(p) },
        "game" to { p -> pad(p) },
        "gamepad" to { p -> pad(p) },
        "bot" to { p -> p.addRoundRect(RectF(5f, 8f, 19f, 19f), 3f, 3f, Path.Direction.CW); p.moveTo(12f, 8f); p.lineTo(12f, 4.5f); p.addCircle(9f, 13f, 1f, Path.Direction.CW); p.addCircle(15f, 13f, 1f, Path.Direction.CW); p.moveTo(9.5f, 16.5f); p.lineTo(14.5f, 16.5f) },
        "ping" to { p -> bars(p) },
        "wifi" to { p -> p.addArc(RectF(3f, 6f, 21f, 24f), 225f, 90f); p.addArc(RectF(6.5f, 9.5f, 17.5f, 20.5f), 225f, 90f); p.addCircle(12f, 17.5f, 1f, Path.Direction.CW) },
        "lan" to { p -> p.addRect(RectF(9f, 3f, 15f, 8f), Path.Direction.CW); p.moveTo(12f, 8f); p.lineTo(12f, 13f); p.moveTo(5f, 13f); p.lineTo(19f, 13f); p.moveTo(5f, 13f); p.lineTo(5f, 16f); p.moveTo(19f, 13f); p.lineTo(19f, 16f); p.addRect(RectF(2f, 16f, 8f, 21f), Path.Direction.CW); p.addRect(RectF(16f, 16f, 22f, 21f), Path.Direction.CW) },
        "internet" to { p -> globe(p) },
        "language" to { p -> globe(p) },
        "vpn" to { p -> shield(p) },
        "privacy" to { p -> shield(p) },
        "host" to { p -> p.moveTo(12f, 10f); p.lineTo(8f, 21f); p.moveTo(12f, 10f); p.lineTo(16f, 21f); p.addCircle(12f, 9f, 1.5f, Path.Direction.CW); p.addArc(RectF(6f, 3f, 18f, 15f), 200f, 140f); p.addArc(RectF(6f, 3f, 18f, 15f), 20f, -40f) },
        "server" to { p -> p.addRoundRect(RectF(4f, 4f, 20f, 11f), 2f, 2f, Path.Direction.CW); p.addRoundRect(RectF(4f, 13f, 20f, 20f), 2f, 2f, Path.Direction.CW); p.moveTo(7f, 7.5f); p.lineTo(9f, 7.5f); p.moveTo(7f, 16.5f); p.lineTo(9f, 16.5f) },
        "mods" to { p -> p.moveTo(5f, 9f); p.lineTo(9f, 9f); p.cubicTo(9f, 5f, 15f, 5f, 15f, 9f); p.lineTo(19f, 9f); p.lineTo(19f, 19f); p.lineTo(5f, 19f); p.close() },
        "video" to { p -> p.addRoundRect(RectF(3f, 5f, 21f, 16f), 2f, 2f, Path.Direction.CW); p.moveTo(9f, 20f); p.lineTo(15f, 20f); p.moveTo(12f, 16f); p.lineTo(12f, 20f) },
        "audio" to { p -> p.moveTo(4f, 9f); p.lineTo(8f, 9f); p.lineTo(13f, 5f); p.lineTo(13f, 19f); p.lineTo(8f, 15f); p.lineTo(4f, 15f); p.close(); p.addArc(RectF(12f, 8f, 18f, 16f), -60f, 120f); p.addArc(RectF(11f, 5f, 21f, 19f), -60f, 120f) },
        "input" to { p -> keys(p) },
        "keyboard" to { p -> keys(p) },
        "touch" to { p -> p.moveTo(10f, 20f); p.lineTo(10f, 9f); p.cubicTo(10f, 7f, 13f, 7f, 13f, 9f); p.lineTo(13f, 13f); p.lineTo(17f, 14f); p.cubicTo(18.5f, 14.4f, 18.5f, 16f, 18f, 17f); p.lineTo(16.5f, 20f) },
        "esp" to { p -> p.moveTo(2.5f, 12f); p.cubicTo(7f, 4f, 17f, 4f, 21.5f, 12f); p.cubicTo(17f, 20f, 7f, 20f, 2.5f, 12f); p.addCircle(12f, 12f, 3f, Path.Direction.CW) },
        "data" to { p -> p.addOval(RectF(5f, 3f, 19f, 8f), Path.Direction.CW); p.moveTo(5f, 5.5f); p.lineTo(5f, 18.5f); p.cubicTo(5f, 21.5f, 19f, 21.5f, 19f, 18.5f); p.lineTo(19f, 5.5f); p.moveTo(5f, 12f); p.cubicTo(5f, 15f, 19f, 15f, 19f, 12f) },
        "download" to { p -> p.moveTo(12f, 4f); p.lineTo(12f, 15f); p.moveTo(7.5f, 10.5f); p.lineTo(12f, 15f); p.lineTo(16.5f, 10.5f); p.moveTo(5f, 20f); p.lineTo(19f, 20f) },
        "update" to { p -> p.moveTo(12f, 4f); p.lineTo(12f, 15f); p.moveTo(7.5f, 10.5f); p.lineTo(12f, 15f); p.lineTo(16.5f, 10.5f); p.moveTo(5f, 20f); p.lineTo(19f, 20f) },
        "upload" to { p -> p.moveTo(12f, 15f); p.lineTo(12f, 4f); p.moveTo(7.5f, 8.5f); p.lineTo(12f, 4f); p.lineTo(16.5f, 8.5f); p.moveTo(5f, 20f); p.lineTo(19f, 20f) },
        "link" to { p -> p.addRoundRect(RectF(3f, 9f, 13f, 15f), 3f, 3f, Path.Direction.CW); p.addRoundRect(RectF(11f, 9f, 21f, 15f), 3f, 3f, Path.Direction.CW) },
        "share" to { p -> p.addCircle(18f, 6f, 2.5f, Path.Direction.CW); p.addCircle(6f, 12f, 2.5f, Path.Direction.CW); p.addCircle(18f, 18f, 2.5f, Path.Direction.CW); p.moveTo(8.2f, 10.8f); p.lineTo(15.8f, 7.2f); p.moveTo(8.2f, 13.2f); p.lineTo(15.8f, 16.8f) },
        "copy" to { p -> p.addRoundRect(RectF(8f, 8f, 20f, 20f), 2f, 2f, Path.Direction.CW); p.moveTo(16f, 8f); p.lineTo(16f, 4f); p.lineTo(4f, 4f); p.lineTo(4f, 16f); p.lineTo(8f, 16f) },
        "edit" to { p -> p.moveTo(4f, 20f); p.lineTo(5f, 15f); p.lineTo(16f, 4f); p.lineTo(20f, 8f); p.lineTo(9f, 19f); p.close() },
        "delete" to { p -> p.moveTo(4f, 7f); p.lineTo(20f, 7f); p.moveTo(9f, 7f); p.lineTo(9f, 4f); p.lineTo(15f, 4f); p.lineTo(15f, 7f); p.moveTo(6f, 7f); p.lineTo(7f, 20f); p.lineTo(17f, 20f); p.lineTo(18f, 7f) },
        "warning" to { p -> p.moveTo(12f, 3.5f); p.lineTo(21f, 19.5f); p.lineTo(3f, 19.5f); p.close(); p.moveTo(12f, 9f); p.lineTo(12f, 14f); p.moveTo(12f, 16.5f); p.lineTo(12f, 17.2f) },
        "error" to { p -> p.addCircle(12f, 12f, 9f, Path.Direction.CW); p.moveTo(12f, 7f); p.lineTo(12f, 13f); p.moveTo(12f, 16f); p.lineTo(12f, 16.7f) },
        "timer" to { p -> p.addCircle(12f, 13f, 8f, Path.Direction.CW); p.moveTo(12f, 13f); p.lineTo(12f, 9f); p.moveTo(10f, 3f); p.lineTo(14f, 3f) },
        "trophy" to { p -> p.moveTo(7f, 4f); p.lineTo(17f, 4f); p.lineTo(17f, 9f); p.cubicTo(17f, 15f, 7f, 15f, 7f, 9f); p.close(); p.moveTo(12f, 14f); p.lineTo(12f, 18f); p.moveTo(8f, 20f); p.lineTo(16f, 20f) },
        "flag" to { p -> p.moveTo(5f, 21f); p.lineTo(5f, 4f); p.lineTo(18f, 4f); p.lineTo(15f, 8.5f); p.lineTo(18f, 13f); p.lineTo(5f, 13f) },
        "crosshair" to { p -> crosshair(p) },
        "aim" to { p -> crosshair(p) },
        "hud" to { p -> p.addRoundRect(RectF(3f, 4f, 21f, 20f), 2f, 2f, Path.Direction.CW); p.moveTo(6f, 16f); p.lineTo(10f, 16f); p.moveTo(14f, 16f); p.lineTo(18f, 16f); p.moveTo(6f, 8f); p.lineTo(8f, 8f) },
        "colour" to { p -> p.addCircle(12f, 12f, 9f, Path.Direction.CW); p.addCircle(8.5f, 9.5f, 1.2f, Path.Direction.CW); p.addCircle(12f, 7.5f, 1.2f, Path.Direction.CW); p.addCircle(15.5f, 9.5f, 1.2f, Path.Direction.CW) },
        "mouse" to { p -> p.addRoundRect(RectF(7f, 3f, 17f, 21f), 5f, 5f, Path.Direction.CW); p.moveTo(12f, 6f); p.lineTo(12f, 10f) },
        "folder" to { p -> p.moveTo(3f, 6f); p.lineTo(9f, 6f); p.lineTo(11f, 8f); p.lineTo(21f, 8f); p.lineTo(21f, 19f); p.lineTo(3f, 19f); p.close() },
        "file" to { p -> p.moveTo(6f, 3f); p.lineTo(14f, 3f); p.lineTo(19f, 8f); p.lineTo(19f, 21f); p.lineTo(6f, 21f); p.close(); p.moveTo(14f, 3f); p.lineTo(14f, 8f); p.lineTo(19f, 8f) },
        "menu" to { p -> p.moveTo(4f, 7f); p.lineTo(20f, 7f); p.moveTo(4f, 12f); p.lineTo(20f, 12f); p.moveTo(4f, 17f); p.lineTo(20f, 17f) },
        "more" to { p -> p.addCircle(12f, 6f, 1f, Path.Direction.CW); p.addCircle(12f, 12f, 1f, Path.Direction.CW); p.addCircle(12f, 18f, 1f, Path.Direction.CW) },
    )

    private fun person(p: Path, x: Float, s: Float) {
        p.addCircle(x, 8f, 3.3f * s, Path.Direction.CW)
        p.moveTo(x - 6f * s, 20f); p.cubicTo(x - 6f * s, 13f, x + 6f * s, 13f, x + 6f * s, 20f)
    }
    private fun info(p: Path) { p.addCircle(12f, 12f, 9f, Path.Direction.CW); p.moveTo(12f, 11f); p.lineTo(12f, 17f); p.moveTo(12f, 7.5f); p.lineTo(12f, 8.2f) }
    private fun star(p: Path) {
        for (i in 0 until 10) {
            val r = if (i % 2 == 0) 9f else 4f
            val a = Math.toRadians(-90.0 + i * 36.0)
            val x = 12f + r * Math.cos(a).toFloat(); val y = 12.5f + r * Math.sin(a).toFloat()
            if (i == 0) p.moveTo(x, y) else p.lineTo(x, y)
        }
        p.close()
    }
    private fun pad(p: Path) { p.addRoundRect(RectF(3f, 7f, 21f, 17f), 5f, 5f, Path.Direction.CW); p.moveTo(7f, 12f); p.lineTo(10f, 12f); p.moveTo(8.5f, 10.5f); p.lineTo(8.5f, 13.5f); p.addCircle(15.5f, 11f, 0.8f, Path.Direction.CW); p.addCircle(17.5f, 13f, 0.8f, Path.Direction.CW) }
    private fun bars(p: Path) { p.moveTo(5f, 19f); p.lineTo(5f, 15f); p.moveTo(10f, 19f); p.lineTo(10f, 11f); p.moveTo(15f, 19f); p.lineTo(15f, 7f); p.moveTo(20f, 19f); p.lineTo(20f, 4f) }
    private fun globe(p: Path) { p.addCircle(12f, 12f, 9f, Path.Direction.CW); p.moveTo(3f, 12f); p.lineTo(21f, 12f); p.addOval(RectF(8f, 3f, 16f, 21f), Path.Direction.CW) }
    private fun shield(p: Path) { p.moveTo(12f, 3f); p.lineTo(19f, 6f); p.lineTo(19f, 11f); p.cubicTo(19f, 16f, 15.5f, 19.5f, 12f, 21f); p.cubicTo(8.5f, 19.5f, 5f, 16f, 5f, 11f); p.lineTo(5f, 6f); p.close() }
    private fun keys(p: Path) { p.addRoundRect(RectF(2f, 6f, 22f, 18f), 2f, 2f, Path.Direction.CW); p.moveTo(6f, 10f); p.lineTo(7f, 10f); p.moveTo(10f, 10f); p.lineTo(11f, 10f); p.moveTo(14f, 10f); p.lineTo(15f, 10f); p.moveTo(7f, 14f); p.lineTo(17f, 14f) }
    private fun crosshair(p: Path) { p.addCircle(12f, 12f, 7f, Path.Direction.CW); p.moveTo(12f, 2f); p.lineTo(12f, 7f); p.moveTo(12f, 17f); p.lineTo(12f, 22f); p.moveTo(2f, 12f); p.lineTo(7f, 12f); p.moveTo(17f, 12f); p.lineTo(22f, 12f) }

    fun has(name: String) = drawings.containsKey(name)

    /** The icon [name] as a drawable [sizePx] square in [color]; null for a name without a drawing. */
    fun drawable(name: String, sizePx: Int, color: Int): Drawable? {
        val draw = drawings[name] ?: return null
        val path = Path().also(draw)
        val filled = name == "play" || name == "star_filled"
        return IconDrawable(path, sizePx, color, filled)
    }

    /** Text for an icon where only text fits: icons are drawables now, so just the text. */
    fun label(icon: String, text: String) = text
    fun glyph(name: String) = ""
}

class IconDrawable(private val path: Path, private val size: Int, color: Int, private val filled: Boolean) : Drawable() {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = color
        style = if (filled) Paint.Style.FILL_AND_STROKE else Paint.Style.STROKE
        strokeWidth = 2f
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    init { setBounds(0, 0, size, size) }
    override fun draw(c: Canvas) {
        val b = bounds
        c.save()
        c.translate(b.left.toFloat(), b.top.toFloat())
        c.scale(b.width() / 24f, b.height() / 24f)
        c.drawPath(path, paint)
        c.restore()
    }
    override fun setAlpha(alpha: Int) { paint.alpha = alpha }
    override fun setColorFilter(cf: ColorFilter?) { paint.colorFilter = cf }
    @Deprecated("Deprecated in Java")
    override fun getOpacity() = PixelFormat.TRANSLUCENT
    override fun getIntrinsicWidth() = size
    override fun getIntrinsicHeight() = size
    fun tint(color: Int) { paint.color = color; invalidateSelf() }
}
