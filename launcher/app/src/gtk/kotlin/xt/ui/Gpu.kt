// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import gtk.*
import kotlinx.cinterop.*
import xt.core.fetch.DataSets
import xt.core.fetch.GpuInfo
import xtnative.epoxy_has_gl_extension
import xtnative.xt_gl_string

/**
 * Asks the GPU, before the engine ever runs, whether it can draw ASTC
 * textures: a GL context made from the launcher window's own surface (the
 * same display, driver and GPU the game will get), made current just long
 * enough to read its extensions through libepoxy.
 *
 * Desktop OpenGL first, because that is what the desktop engine
 * (darkplaces-sdl) asks for and an extension list can differ between a
 * driver's GL and GLES contexts; OpenGL ES when desktop GL is not there
 * (phones, Ubuntu Touch). No GL at all leaves `astc` unknown, and the caller
 * picks the full set, which every GPU draws.
 */
object Gpu {
    private var cached: GpuInfo? = null

    fun probe(window: Widget?): GpuInfo {
        cached?.let { return it }
        val r = runCatching { probeNow(window) }.getOrElse { GpuInfo(null, error = it.message ?: "probe failed") }
        if (r.astc != null || r.error != "the window is not shown yet") cached = r
        eprintln("GPU: ${r.renderer ?: "unknown"} (${r.api ?: "no GL"}): ASTC ${r.astc ?: "unknown"}${r.error?.let { " — $it" }.orEmpty()}")
        return r
    }

    private fun probeNow(window: Widget?): GpuInfo {
        val surface = window?.let { gtk_native_get_surface(it.reinterpret()) }
            ?: return GpuInfo(null, error = "the window is not shown yet")
        var lastError = "no OpenGL"
        for ((api, name) in listOf(GDK_GL_API_GL to "OpenGL", GDK_GL_API_GLES to "OpenGL ES")) {
            memScoped {
                val err = allocPointerTo<GError>()
                val ctx = gdk_surface_create_gl_context(surface, err.ptr)
                if (ctx == null) {
                    lastError = err.value?.pointed?.message?.toKString() ?: "no OpenGL"
                    err.value?.let { g_error_free(it) }
                    return GpuInfo(null, error = lastError)
                }
                try {
                    gdk_gl_context_set_allowed_apis(ctx, api)
                    if (gdk_gl_context_realize(ctx, err.ptr) == 0) {
                        lastError = err.value?.pointed?.message?.toKString() ?: "$name unavailable"
                        err.value?.let { g_error_free(it) }
                        return@memScoped
                    }
                    gdk_gl_context_make_current(ctx)
                    val astc = epoxy_has_gl_extension(DataSets.ASTC_EXTENSION)
                    val renderer = xt_gl_string(0x1F01u)?.toKString() // GL_RENDERER
                    gdk_gl_context_clear_current()
                    return GpuInfo(astc, renderer, name)
                } finally {
                    g_object_unref(ctx)
                }
            }
        }
        return GpuInfo(null, error = lastError)
    }
}

/** The banner and icon, built into the binary (see embedBranding in app/build.gradle.kts). */
object Branding {
    private fun texture(parts: List<String>): CPointer<GdkTexture>? = runCatching {
        @OptIn(kotlin.io.encoding.ExperimentalEncodingApi::class)
        val bytes = kotlin.io.encoding.Base64.decode(parts.joinToString(""))
        val gb = bytes.usePinned { g_bytes_new(it.addressOf(0), bytes.size.convert()) }
        val tex = gdk_texture_new_from_bytes(gb, null)
        g_bytes_unref(gb)
        tex
    }.getOrNull()

    val banner by lazy { texture(Embedded.banner) }
    val icon by lazy { texture(Embedded.icon) }

    /** A picture of [tex] that keeps its aspect and asks for [height] px. */
    fun picture(tex: CPointer<GdkTexture>?, height: Int, alt: String): Widget? {
        tex ?: return null
        val p = gtk_picture_new_for_paintable(tex.reinterpret())!!
        gtk_picture_set_content_fit(p.reinterpret(), GtkContentFit.GTK_CONTENT_FIT_CONTAIN)
        gtk_picture_set_can_shrink(p.reinterpret(), 1)
        gtk_picture_set_alternative_text(p.reinterpret(), alt)
        gtk_widget_set_size_request(p, -1, height)
        return p
    }
}
