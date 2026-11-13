package io.github.dixonsolutions.xonotictouch

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.GLES20

/**
 * Asks the GPU what it can decode, before any game data is chosen.
 *
 * The slim data set stores its textures as ASTC. A GPU without
 * GL_KHR_texture_compression_astc_ldr cannot draw them, so such a device gets
 * the full set instead. The answer comes from a throwaway 1x1 pbuffer context
 * on the calling thread; call it off the UI thread.
 */
object GpuProbe {
    private const val ASTC = "GL_KHR_texture_compression_astc_ldr"

    /** True when ASTC textures can be used; false when not, or when the GPU could not be asked. */
    fun astc(): Boolean = try {
        extensions().split(' ').contains(ASTC)
    } catch (e: RuntimeException) {
        android.util.Log.w("Godnotic", "GPU probe failed; assuming no ASTC", e)
        false
    }

    private fun extensions(): String {
        val display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        check(display != EGL14.EGL_NO_DISPLAY) { "no EGL display" }
        val version = IntArray(2)
        check(EGL14.eglInitialize(display, version, 0, version, 1)) { "eglInitialize" }
        val configs = arrayOfNulls<EGLConfig>(1)
        val count = IntArray(1)
        val want = intArrayOf(
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
            EGL14.EGL_NONE,
        )
        check(EGL14.eglChooseConfig(display, want, 0, configs, 0, 1, count, 0) && count[0] > 0) { "no EGL config" }
        val context = EGL14.eglCreateContext(
            display, configs[0], EGL14.EGL_NO_CONTEXT,
            intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0,
        )
        check(context != EGL14.EGL_NO_CONTEXT) { "no EGL context" }
        val surface = EGL14.eglCreatePbufferSurface(
            display, configs[0], intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0,
        )
        try {
            check(surface != EGL14.EGL_NO_SURFACE) { "no pbuffer" }
            check(EGL14.eglMakeCurrent(display, surface, surface, context)) { "eglMakeCurrent" }
            return GLES20.glGetString(GLES20.GL_EXTENSIONS) ?: ""
        } finally {
            // The display itself stays initialised: SDL shares it later.
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface)
            EGL14.eglDestroyContext(display, context)
        }
    }
}
