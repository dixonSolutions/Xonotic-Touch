package io.github.dixonsolutions.xonotictouch;

import android.os.Bundle;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * The engine surface. {@link BootActivity} has already unpacked the basedir by
 * the time this starts.
 */
public final class XonoticActivity extends SDLActivity {

    static final String EXTRA_BASEDIR = "io.github.dixonsolutions.xonotictouch.BASEDIR";

    private String baseDir;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Read before super.onCreate(): SDLActivity starts the native thread
        // there, and getArguments() is consulted on the way.
        baseDir = getIntent() != null ? getIntent().getStringExtra(EXTRA_BASEDIR) : null;
        if (baseDir == null) {
            baseDir = new GameData(this).baseDir().getAbsolutePath();
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        // libpng is absent on purpose: image_png.c dlopen()s it by name.
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {
        // Without -basedir the engine falls back to /sdcard/xonotic, which
        // scoped storage has made unwritable since Android 10.
        return new String[] {
            "-basedir", baseDir,
            "-userdir", new File(baseDir, "userdata").getAbsolutePath(),
        };
    }
}
