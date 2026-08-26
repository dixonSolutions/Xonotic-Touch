package io.github.dixonsolutions.xonotictouch;

import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.File;
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
    private Thread updateService;
    /** Last release the check found, so Install and Skip know what they mean. */
    private volatile AppUpdater.Update lastSeenUpdate;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Read before super.onCreate(): SDLActivity starts the native thread
        // there, and getArguments() is consulted on the way.
        baseDir = getIntent() != null ? getIntent().getStringExtra(EXTRA_BASEDIR) : null;
        if (baseDir == null) {
            baseDir = new GameData(this).baseDir().getAbsolutePath();
        }
        super.onCreate(savedInstanceState);
        watchSoftKeyboard();
        startUpdateService();
    }

    @Override
    protected void onDestroy() {
        stopUpdateService();
        super.onDestroy();
    }

    /**
     * Serve the menu's update requests while the game is running.
     *
     * BootActivity has already finished by the time anyone can reach the Updates
     * screen, so without something listening here every button on it would write
     * a request file that nothing ever reads. The engine owns the screen; this
     * thread owns the network and the installer.
     *
     * A poll rather than a watch: the writer is QuakeC using plain fopen, which
     * gives no inotify guarantees worth relying on, and a second of latency on a
     * button that kicks off a 30 MB download is not worth a FileObserver for.
     */
    private void startUpdateService() {
        if (baseDir == null) {
            return;
        }
        final UpdateBridge bridge = new UpdateBridge(this, new File(baseDir));
        final AppUpdater updater = new AppUpdater(this);
        updateService = new Thread(() -> {
            // Whatever BootActivity last published stands until someone asks for
            // something; re-checking here would double every launch's API call.
            while (!Thread.currentThread().isInterrupted()) {
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
                String request = bridge.takeRequest();
                if (request != null) {
                    serveUpdateRequest(bridge, updater, request);
                }
            }
        }, "xonotic-update-service");
        updateService.setDaemon(true);
        updateService.start();
    }

    private void stopUpdateService() {
        if (updateService != null) {
            updateService.interrupt();
            updateService = null;
        }
    }

    private void serveUpdateRequest(UpdateBridge bridge, AppUpdater updater, String request) {
        String installed = updater.installedVersion();
        switch (request) {
            case "auto-on":
                AppUpdater.setAutoInstall(this, true);
                // Republish so line 8 reflects the new preference immediately;
                // the menu reads its checkbox back from there.
                bridge.publishIdle(installed);
                return;
            case "auto-off":
                AppUpdater.setAutoInstall(this, false);
                bridge.publishIdle(installed);
                return;
            case "skip":
                AppUpdater.skipVersion(this, lastSeenUpdate == null ? null : lastSeenUpdate.version);
                lastSeenUpdate = null;
                bridge.publishUpToDate(installed);
                return;
            case "check":
                bridge.publishChecking(installed);
                AppUpdater.Update found = updater.findUpdate();
                lastSeenUpdate = found;
                if (found == null) {
                    bridge.publishUpToDate(installed);
                } else {
                    bridge.publishAvailable(installed, found);
                }
                return;
            case "install":
                installFromMenu(bridge, updater, installed);
                return;
            default:
                Log.w("XonoticTouch", "Ignoring unknown update request: " + request);
        }
    }

    private void installFromMenu(UpdateBridge bridge, AppUpdater updater, String installed) {
        AppUpdater.Update update = lastSeenUpdate;
        if (update == null) {
            // The menu can ask to install before anything has been found -- a
            // check that failed, or a status file left by a previous run. Look
            // again rather than refusing.
            bridge.publishChecking(installed);
            update = updater.findUpdate();
            lastSeenUpdate = update;
        }
        if (update == null) {
            bridge.publishUpToDate(installed);
            return;
        }
        final String latest = update.version;
        try {
            if (!updater.install(update,
                    (text, note, percent) -> bridge.publishDownloading(installed, latest, percent),
                    () -> bridge.publishError(installed,
                            getString(R.string.update_status_failed)))) {
                bridge.publishNeedsPermission(installed, latest);
                return;
            }
            bridge.publishInstalling(installed, latest);
        } catch (IOException | RuntimeException e) {
            Log.w("XonoticTouch", "Update install failed", e);
            bridge.publishError(installed, String.valueOf(e.getMessage()));
        }
    }

    /**
     * Show the system bars for as long as the soft keyboard is up.
     *
     * The engine runs immersive fullscreen — DP_MOBILETOUCH forces
     * SDL_WINDOW_FULLSCREEN | SDL_WINDOW_BORDERLESS — which hides the navigation
     * bar. Android's IME can only be dismissed with Back, so once a menu text
     * field opened the keyboard there was nothing left on screen to close it
     * with. Bringing the bars back while typing restores that way out, and
     * immersive mode returns the moment the keyboard does.
     */
    private void watchSoftKeyboard() {
        final View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener((view, insets) -> {
            setSystemBarsVisible(isKeyboardVisible(insets, view));
            return view.onApplyWindowInsets(insets);
        });
    }

    private static boolean isKeyboardVisible(WindowInsets insets, View decor) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return insets.isVisible(WindowInsets.Type.ime());
        }
        // Before the ime() inset existed, the keyboard is only visible as an
        // unusually deep bottom inset. A quarter of the window is well clear of
        // the navigation bar and well under any keyboard.
        return insets.getSystemWindowInsetBottom() > decor.getHeight() / 4;
    }

    private void setSystemBarsVisible(boolean visible) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller == null) {
                return;
            }
            if (visible) {
                controller.show(WindowInsets.Type.systemBars());
            } else {
                controller.hide(WindowInsets.Type.systemBars());
            }
            return;
        }
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(visible ? View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                : View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
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
