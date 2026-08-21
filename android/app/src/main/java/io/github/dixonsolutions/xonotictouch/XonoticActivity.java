package io.github.dixonsolutions.xonotictouch;

import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

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
        // -xonotic is not optional here. darkplaces picks its game from argv[0],
        // and SDL sets that to "app_process" on Android, so the engine otherwise
        // starts as plain Quake looking for an id1/ that will never exist.
        List<String> args = new ArrayList<>(Arrays.asList(
            "-xonotic",
            "-basedir", baseDir,
            "-userdir", new File(baseDir, "userdata").getAbsolutePath()));
        args.addAll(extraArguments());
        return args.toArray(new String[0]);
    }

    /**
     * Extra engine arguments from {@code <basedir>/xonotic.args}, one per line
     * or whitespace separated, {@code #} for comments.
     *
     * A phone has no launcher command line to edit, so without this the only way
     * to turn on engine logging (+developer 1, which is what gates darkplaces'
     * logcat output) or point at a different gamedir is to rebuild the APK.
     */
    private List<String> extraArguments() {
        List<String> extra = new ArrayList<>();
        File file = new File(baseDir, "xonotic.args");
        if (!file.isFile()) {
            return extra;
        }
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = reader.readLine()) != null) {
                int comment = line.indexOf('#');
                if (comment >= 0) {
                    line = line.substring(0, comment);
                }
                for (String token : line.trim().split("\\s+")) {
                    if (!token.isEmpty()) {
                        extra.add(token);
                    }
                }
            }
            Log.i("XonoticTouch", "Extra engine arguments: " + extra);
        } catch (IOException e) {
            Log.w("XonoticTouch", "Could not read " + file, e);
        }
        return extra;
    }
}
