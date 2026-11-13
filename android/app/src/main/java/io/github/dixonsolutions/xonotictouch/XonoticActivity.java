package io.github.dixonsolutions.xonotictouch;

import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * The engine surface. {@link BootActivity} has already unpacked the basedir by
 * the time this starts.
 */
public final class XonoticActivity extends SDLActivity {

    static final String EXTRA_BASEDIR = "io.github.dixonsolutions.xonotictouch.BASEDIR";
    /** The game is up, in front or behind: BootActivity goes straight back to it. */
    static volatile boolean running;

    private String baseDir;
    private Thread updateService;
    private AndySUBridge andysuBridge;
    private ModImport modImport;
    /** Last release the check found, so Install and Skip know what they mean. */
    private volatile AppUpdater.Update lastSeenUpdate;
    /** When this process last asked the release feed (elapsedRealtime). Never,
     *  at first: the boot screen no longer checks, so the first onResume does,
     *  in the background, while the game starts. */
    private volatile long lastCheckMs = -RECHECK_AFTER_MS;
    /** A game left in the background for hours comes back through onResume(),
     *  not through BootActivity. Looking again after this long keeps a phone
     *  that is never cold-started from missing every release. */
    private static final long RECHECK_AFTER_MS = 60L * 60L * 1000L;
    private Thread resumeCheck;
    /** Reads the engine's touch_netstate and keeps {@link KeepAliveService} in step. */
    private Thread netStateWatch;
    private static final long NETSTATE_POLL_MS = 2000;
    /** One update download at a time, whoever asked for it. */
    private final AtomicBoolean downloading = new AtomicBoolean();
    private InputManager inputManager;
    /** The native interface over the SDL surface (fe/FrontendHost.kt); null when the QuakeC menu is in charge. */
    private io.github.dixonsolutions.xonotictouch.fe.FrontendHost frontend;
    /** Decided before the engine starts: getArguments() passes touch_native_frontend from it. */
    private boolean nativeUi;
    /** Last state handed to the engine, so logcat only hears about changes. */
    private int reportedInputState = -1;
    private final InputManager.InputDeviceListener inputDeviceListener =
            new InputManager.InputDeviceListener() {
                @Override
                public void onInputDeviceAdded(int deviceId) {
                    reportInputDevices();
                }

                @Override
                public void onInputDeviceRemoved(int deviceId) {
                    reportInputDevices();
                }

                @Override
                public void onInputDeviceChanged(int deviceId) {
                    reportInputDevices();
                }
            };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Read before super.onCreate(): SDLActivity starts the native thread
        // there, and getArguments() is consulted on the way.
        baseDir = getIntent() != null ? getIntent().getStringExtra(EXTRA_BASEDIR) : null;
        if (baseDir == null) {
            baseDir = new GameData(this).baseDir().getAbsolutePath();
        }
        nativeUi = nativeUiWanted(new File(baseDir));
        andysuBridge = new AndySUBridge(this, new File(baseDir));
        andysuBridge.publish();
        // The host may stage a live update as soon as its activity resumes.
        // Register before SDL starts its native thread in super.onCreate().
        EngineSwap.register(this);
        super.onCreate(savedInstanceState);
        running = true;
        // The boot screen's banner stays up over the SDL surface until the
        // engine has drawn its first frame: no black between the two.
        getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(BootSplash.BACKGROUND));
        if (nativeUi) {
            // The native interface's skeleton is the cover now: Home appears
            // at once and the engine starts behind it (fe/FrontendHost.kt).
            try {
                frontend = new io.github.dixonsolutions.xonotictouch.fe.FrontendHost(this, new File(baseDir));
                frontend.install();
                frontend.handleJoinLink(getIntent() != null ? getIntent().getData() : null);
            } catch (RuntimeException | LinkageError e) {
                Log.e("Godnotic", "Native UI could not start; the QuakeC menu is in charge", e);
                frontend = null;
                BootSplash.coverUntilFirstFrame(this);
            }
        } else {
            BootSplash.coverUntilFirstFrame(this);
        }
        watchSoftKeyboard();
        watchInputDevices();
        startUpdateService();
        modImport = new ModImport(this, new File(baseDir)); // Settings → Mobile → Mods → Import mod
        modImport.start();
        watchNetState();
    }

    /**
     * The native interface, unless this build's resident bridge predates it
     * (an AndySU engine swap can pair a new dex with an old libnativeui.so)
     * or the player turned it off (a file {@code no-native-ui} in the basedir,
     * for now; the QuakeC menu is then in charge exactly as before).
     */
    private static boolean nativeUiWanted(File base) {
        try {
            if (new File(base, "no-native-ui").exists()) {
                return false;
            }
            return NativeBridge.getFrontendVersion() >= 1;
        } catch (RuntimeException | LinkageError e) {
            return false;
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // A native screen takes every key but the volume ones while it shows;
        // nothing reaches the engine behind it.
        if (frontend != null) {
            try {
                if (frontend.dispatchKey(event)) {
                    return true;
                }
            } catch (RuntimeException e) {
                Log.e("Godnotic", "Native UI key handling failed", e);
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (frontend != null && intent != null) {
            frontend.handleJoinLink(intent.getData());
        }
    }

    @Override
    protected void onPause() {
        if (frontend != null) {
            frontend.onPause();
        }
        super.onPause();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (modImport != null && modImport.onActivityResult(requestCode, resultCode, data)) {
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    @Override
    protected void onDestroy() {
        running = false;
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(inputDeviceListener);
        }
        stopUpdateService();
        if (modImport != null) {
            modImport.stop();
        }
        if (netStateWatch != null) {
            netStateWatch.interrupt();
        }
        KeepAliveService.stop(this);
        LanLocks.release();
        if (frontend != null) {
            frontend.onDestroy();
        }
        EngineSwap.unregister();
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (frontend != null) {
            frontend.onResume();
        }
        reportInputDevices();
        recheckUpdateIfStale();
        autoDownloadIfDue();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // keyboard and keyboardHidden are in the manifest's configChanges, so a
        // keyboard attached, folded away or slid shut arrives here.
        reportInputDevices();
    }

    /**
     * Tell the engine which input hardware is attached, and keep telling it.
     *
     * This is the engine's source of truth for what the player has; what
     * they press never changes it (vid_touchdetect.c). The on-screen
     * controls show for a touch device with no keyboard listed here and hide
     * while one is; touch and a listed mouse work in both states. InputManager's listener covers USB and Bluetooth
     * hot-plug; configuration changes cover a keyboard folded away. None of
     * this needs a permission.
     */
    private void watchInputDevices() {
        inputManager = (InputManager) getSystemService(INPUT_SERVICE);
        if (inputManager != null) {
            // A null handler delivers the callbacks on the main looper.
            inputManager.registerInputDeviceListener(inputDeviceListener, null);
        }
        reportInputDevices();
    }

    private void reportInputDevices() {
        boolean touch = getPackageManager().hasSystemFeature(PackageManager.FEATURE_TOUCHSCREEN);
        boolean keyboard = false;
        boolean mouse = false;
        List<InputDevice> pointers = new ArrayList<>();
        StringBuilder names = new StringBuilder();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device == null || device.isVirtual()) {
                continue;
            }
            if (Build.VERSION.SDK_INT >= 27 && !device.isEnabled()) {
                continue;
            }
            int sources = device.getSources();
            if (hasSource(sources, InputDevice.SOURCE_TOUCHSCREEN)) {
                touch = true;
            }
            if (isRealKeyboard(device, sources)) {
                keyboard = true;
                names.append(" keyboard=\"").append(device.getName()).append('"');
            }
            if (isRealPointer(device, sources)) {
                pointers.add(device);
            }
        }
        // A keyboard the system itself calls hidden -- a lid or slider shut,
        // a Chromebook folded into a tablet -- is not one the player can use,
        // and neither is the chassis pad that goes face-down with it. Only a
        // listed keyboard makes that folding: hardKeyboardHidden also reads
        // YES on every device with no hard keys at all, where a touchpad the
        // player attached is the pointer they aim with.
        boolean folded = keyboard && getResources().getConfiguration().hardKeyboardHidden
                == Configuration.HARDKEYBOARDHIDDEN_YES;
        if (folded) {
            keyboard = false;
        }
        for (InputDevice pointer : pointers) {
            if (folded && isChassisPad(pointer)) {
                continue;
            }
            if (!keyboard && isBuiltInTouchpad(pointer)) {
                continue;
            }
            mouse = true;
            names.append(" mouse=\"").append(pointer.getName()).append('"');
        }

        int state = (touch ? 1 : 0) | (keyboard ? 2 : 0) | (mouse ? 4 : 0);
        if (state != reportedInputState) {
            reportedInputState = state;
            Log.i("Godnotic", "Input devices: touch=" + touch + " keyboard=" + keyboard
                    + " mouse=" + mouse + names);
        }
        try {
            nativeInputDevices(touch, keyboard, mouse);
        } catch (UnsatisfiedLinkError e) {
            // libmain did not load; SDLActivity is already showing why.
        }
    }

    private static boolean hasSource(int sources, int source) {
        return (sources & source) == source;
    }

    /**
     * A keyboard someone types on: alphabetic, not virtual, not a game
     * controller (plenty of Bluetooth pads report letter keys), and not one
     * of the button sets and sensors phones register as keyboards.
     */
    private static boolean isRealKeyboard(InputDevice device, int sources) {
        if (!hasSource(sources, InputDevice.SOURCE_KEYBOARD)
                || device.getKeyboardType() != InputDevice.KEYBOARD_TYPE_ALPHABETIC) {
            return false;
        }
        if (hasSource(sources, InputDevice.SOURCE_GAMEPAD)
                || hasSource(sources, InputDevice.SOURCE_JOYSTICK)) {
            return false;
        }
        return !hasIgnoredName(device);
    }

    /**
     * A mouse or touchpad that moves a pointer: not a touchscreen
     * or a pen on one (those are touch), and not a sensor that some phones
     * register as a touchpad.
     */
    private static boolean isRealPointer(InputDevice device, int sources) {
        if (hasSource(sources, InputDevice.SOURCE_TOUCHSCREEN)
                || hasSource(sources, InputDevice.SOURCE_STYLUS)) {
            return false;
        }
        boolean pointer = hasSource(sources, InputDevice.SOURCE_MOUSE)
                || hasSource(sources, InputDevice.SOURCE_TOUCHPAD)
                || (Build.VERSION.SDK_INT >= 26
                        && hasSource(sources, InputDevice.SOURCE_MOUSE_RELATIVE));
        return pointer && !hasIgnoredName(device);
    }

    /**
     * A pad that folds away with the keys -- a Type Cover's, a convertible
     * base's -- rather than a mouse the player set down beside the screen.
     * Only the pad names itself one. Vendor and product cannot separate the
     * two: a combo receiver hands its keyboard and its mouse a single pair,
     * both read 0 when unknown, and a base usually drives its pad from a
     * second controller with IDs of its own.
     */
    private static boolean isChassisPad(InputDevice pointer) {
        return hasName(pointer, CHASSIS_POINTER_NAMES);
    }

    /**
     * A touchpad built into the device, on a device with no keyboard listed:
     * a Galaxy Tab's sec_touchpad, which sits on the touchscreen's own
     * controller and is never a pad anyone aims with. A laptop's built-in pad
     * comes with its built-in keyboard, so it still counts there.
     */
    private static boolean isBuiltInTouchpad(InputDevice pointer) {
        return Build.VERSION.SDK_INT >= 29 && !pointer.isExternal()
                && hasSource(pointer.getSources(), InputDevice.SOURCE_TOUCHPAD);
    }

    private static boolean hasIgnoredName(InputDevice device) {
        return hasName(device, IGNORED_KEYBOARD_NAMES);
    }

    private static boolean hasName(InputDevice device, String[] fragments) {
        String name = device.getName() == null ? "" : device.getName().toLowerCase(Locale.ROOT);
        for (String fragment : fragments) {
            if (name.contains(fragment)) {
                return true;
            }
        }
        return false;
    }

    /** Fingerprint readers, GPIO buttons, hall sensors and uinput helpers that
     *  some phones register as alphabetic keyboards or touchpads. */
    private static final String[] IGNORED_KEYBOARD_NAMES = {
        "uinput", "fingerprint", "fpc", "goodix", "gf_input", "gpio", "_pon",
        "hall", "power", "volume", "headset", "virtual", "sensor",
    };

    /** Pads built into a chassis or into the cover it folds away with. */
    private static final String[] CHASSIS_POINTER_NAMES = { "touchpad", "trackpad" };

    /** vid_touchdetect.c: what the engine's Auto touch mode decides from. */
    private static native void nativeInputDevices(boolean touch, boolean keyboard, boolean mouse);

    /**
     * Ask the feed again after a long stretch in the background.
     *
     * Serves the same auto-install preference as the boot check: found and
     * automatic means the download starts; found and manual means the menu's
     * Updates screen shows it the next time it is opened. Nothing here stops
     * play -- the check is on its own thread and a failure changes nothing.
     */
    private void recheckUpdateIfStale() {
        if (android.os.SystemClock.elapsedRealtime() - lastCheckMs < RECHECK_AFTER_MS) {
            return;
        }
        if (resumeCheck != null && resumeCheck.isAlive()) {
            return;
        }
        if (!AppUpdater.isEnabled(this) || baseDir == null) {
            return;
        }
        lastCheckMs = android.os.SystemClock.elapsedRealtime();
        final UpdateBridge bridge = new UpdateBridge(this, new File(baseDir));
        final AppUpdater updater = new AppUpdater(this);
        resumeCheck = new Thread(() -> {
            String installed = updater.installedVersion();
            if (downloading.get()) {
                return;
            }
            bridge.publishChecking(installed);
            AppUpdater.Update update = updater.findUpdate();
            publishFindings(bridge, updater, installed, update);
            if (update != null && AppUpdater.isAutoInstall(this)) {
                startDownload(bridge, updater, update, false, false);
            }
        }, "xonotic-update-recheck");
        resumeCheck.setDaemon(true);
        resumeCheck.start();
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
                AndySUBridge payloadBridge = andysuBridge;
                if (payloadBridge != null) {
                    String payloadRequest = payloadBridge.takeRequest();
                    if (payloadRequest != null) payloadBridge.serve(payloadRequest);
                    payloadBridge.publish();
                }
            }
        }, "xonotic-update-service");
        updateService.setDaemon(true);
        updateService.start();
        autoDownloadIfDue();
    }

    /**
     * Automatic updates: fetch the release the last check found into the
     * update cache while the game runs, so it installs the next time the app
     * starts. Only on Wi-Fi or another unmetered connection; a stopped download
     * resumes from the cache the next time this runs (start, resume, recheck).
     */
    private void autoDownloadIfDue() {
        if (baseDir == null || !AppUpdater.isEnabled(this) || !AppUpdater.isAutoInstall(this)) {
            return;
        }
        AppUpdater updater = new AppUpdater(this);
        AppUpdater.Update found = updater.lastFound();
        if (found == null || updater.isReady(found)) {
            return;
        }
        startDownload(new UpdateBridge(this, new File(baseDir)), updater, found, false, false);
    }

    /**
     * Download an update into the cache on a thread of its own, then either
     * leave it there to install on the next start (ready) or install it now.
     *
     * @param asked the player pressed a button for this, so it goes ahead on
     *              mobile data too; a background download waits for Wi-Fi
     * @param installAfter install as soon as it is downloaded
     */
    private void startDownload(UpdateBridge bridge, AppUpdater updater, AppUpdater.Update update,
                               boolean asked, boolean installAfter) {
        String installed = updater.installedVersion();
        if (updater.isReady(update)) {
            if (installAfter) {
                installCachedFromMenu(bridge, updater, installed, update);
            } else {
                bridge.publishReady(installed, update.version);
            }
            return;
        }
        Net.Kind network = Net.kind(this);
        if (!asked && network != Net.Kind.UNMETERED) {
            // Say what the connection really is: "waiting for Wi-Fi" while the
            // Wi-Fi icon shows was a Wi-Fi without internet, with mobile data
            // carrying everything.
            Log.i("Godnotic", "Update download waits: network " + network);
            bridge.publishPaused(installed, update.version, updater.cachedPercent(update),
                    network == Net.Kind.MOBILE
                            ? HostRes.string(this, "update_status_waiting_wifi")
                            : HostRes.string(this, "update_status_waiting_network", Net.describe(this, network)));
            return;
        }
        if (!downloading.compareAndSet(false, true)) {
            return;
        }
        Thread worker = new Thread(() -> {
            try {
                updater.download(update,
                        (text, note, percent) -> bridge.publishDownloading(installed, update.version, Math.max(0, percent)));
                if (installAfter) {
                    installCachedFromMenu(bridge, updater, installed, update);
                } else {
                    bridge.publishReady(installed, update.version);
                }
            } catch (IOException | RuntimeException e) {
                Log.w("Godnotic", "Update download stopped", e);
                if (updater.cachedBytes(update) > 0) {
                    bridge.publishPaused(installed, update.version, updater.cachedPercent(update),
                            HostRes.string(this, "update_status_paused"));
                } else {
                    bridge.publishInstallFailed(installed, update.version, String.valueOf(e.getMessage()));
                }
            } finally {
                downloading.set(false);
            }
        }, "xonotic-update-download");
        worker.setDaemon(true);
        worker.start();
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
            case "auto-off":
                AppUpdater.setAutoInstall(this, "auto-on".equals(request));
                return;
            case "skip": {
                AppUpdater.Update target = targetUpdate(bridge, updater, installed);
                if (target == null) {
                    publishFindings(bridge, updater, installed, null);
                    return;
                }
                updater.skip(target);
                lastSeenUpdate = null;
                bridge.publishUpToDate(installed);
                return;
            }
            case "check": {
                if (downloading.get()) {
                    return;
                }
                bridge.publishChecking(installed);
                AppUpdater.Update found = updater.findUpdate();
                publishFindings(bridge, updater, installed, found);
                if (found != null && AppUpdater.isAutoInstall(this)) {
                    startDownload(bridge, updater, found, false, false);
                }
                return;
            }
            case "download": {
                AppUpdater.Update target = targetUpdate(bridge, updater, installed);
                if (target == null) {
                    publishFindings(bridge, updater, installed, null);
                    return;
                }
                startDownload(bridge, updater, target, true, false);
                return;
            }
            case "install":
                installFromMenu(bridge, updater, installed);
                return;
            default:
                Log.w("Godnotic", "Ignoring unknown update request: " + request);
        }
    }

    /**
     * The release Install and Skip are acting on.
     *
     * The menu enables both from the published status, which the boot check may
     * have written before this process existed, so the field is routinely empty
     * when the first press arrives. Fall back to what that check recorded, and
     * only then to asking the feed again.
     */
    private AppUpdater.Update targetUpdate(UpdateBridge bridge, AppUpdater updater,
                                           String installed) {
        AppUpdater.Update update = lastSeenUpdate;
        if (update == null) {
            update = updater.lastFound();
        }
        if (update == null) {
            bridge.publishChecking(installed);
            update = updater.findUpdate();
        }
        lastSeenUpdate = update;
        return update;
    }

    /**
     * Publish what a check learned.
     *
     * A check that could not reach the feed has learned nothing, and saying
     * "up to date" on its behalf both misinforms the player and drops the
     * release the last successful check found — which is the one thing on this
     * screen they might have been about to install.
     */
    private void publishFindings(UpdateBridge bridge, AppUpdater updater,
                                 String installed, AppUpdater.Update found) {
        if (found != null) {
            lastSeenUpdate = found;
            if (updater.isReady(found)) {
                bridge.publishReady(installed, found.version);
            } else if (updater.cachedBytes(found) > 0) {
                bridge.publishPaused(installed, found.version, updater.cachedPercent(found),
                        HostRes.string(this, "update_status_paused"));
            } else {
                bridge.publishAvailable(installed, found);
            }
            return;
        }
        if (updater.lastCheckFailed()) {
            lastSeenUpdate = lastSeenUpdate != null ? lastSeenUpdate : updater.lastFound();
            bridge.publishCheckFailed(installed, lastSeenUpdate);
            return;
        }
        lastSeenUpdate = null;
        bridge.publishUpToDate(installed);
    }

    /**
     * Install now: from the cache when the update is already downloaded,
     * otherwise after downloading it (resuming whatever is cached). The
     * player asked, so mobile data is fine.
     */
    private void installFromMenu(UpdateBridge bridge, AppUpdater updater, String installed) {
        AppUpdater.Update update = targetUpdate(bridge, updater, installed);
        if (update == null) {
            publishFindings(bridge, updater, installed, null);
            return;
        }
        startDownload(bridge, updater, update, true, true);
    }

    private void installCachedFromMenu(UpdateBridge bridge, AppUpdater updater, String installed,
                                       AppUpdater.Update update) {
        final String latest = update.version;
        // The failure callback is a broadcast receiver, so it can land before
        // installCached() has returned -- at once, if launching the confirm
        // dialog throws. Publishing `installing` over it would leave the screen
        // busy behind a bar that never finishes.
        final AtomicBoolean failed = new AtomicBoolean();
        try {
            if (!updater.installCached(update, () -> {
                        failed.set(true);
                        bridge.publishInstallFailed(installed, latest,
                                HostRes.string(this, "update_status_failed"));
                    })) {
                bridge.publishNeedsPermission(installed, latest);
                return;
            }
            if (!failed.get()) {
                bridge.publishInstalling(installed, latest);
            }
        } catch (IOException | RuntimeException e) {
            // A refused or broken install keeps the download: the retry
            // installs from the cache without fetching it again.
            Log.w("Godnotic", "Update install failed", e);
            bridge.publishInstallFailed(installed, latest,
                    String.valueOf(e.getMessage()));
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
     *
     * Only the navigation bar: Back is what dismisses the keyboard. The status
     * bar would cover the top of the screen, where the engine's console and
     * chat sheets keep CLOSE and KEYBOARD (a player who chose HIDE KEYS types
     * on the system keyboard and still needs them).
     */
    private void watchSoftKeyboard() {
        final View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener((view, insets) -> {
            setSystemBarsVisible(isKeyboardVisible(insets, view));
            BootSplash.reportSafeInsets(this, insets); // the engine's console button keeps clear of cutouts
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
                controller.show(WindowInsets.Type.navigationBars());
                controller.hide(WindowInsets.Type.statusBars());
            } else {
                controller.hide(WindowInsets.Type.systemBars());
            }
            return;
        }
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(visible ? View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                : View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    protected String[] getLibraries() {
        // libpng is absent on purpose: image_png.c dlopen()s it by name. libmain is not loaded here
        // any more: the resident loader dlopen()s it (AndySU live engine swap, see
        // testbeds/patches/xonotic-touch/LIVE_SWAP.md) and answers nativeInputDevices for it.
        return new String[] { "SDL2", "andysu_xonotic_loader" };
    }

    @Override
    protected String getMainSharedObject() {
        return getApplicationInfo().nativeLibraryDir + "/libandysu_xonotic_loader.so";
    }

    /**
     * While the engine is connected to a server, joining one or downloading
     * (touch_netstate, written by vid_sdl.c when that changes), a foreground
     * service keeps the process alive should the player leave the app; it
     * stops as soon as the engine is idle. Started while the game is in front,
     * the only time Android 12+ allows it, and kept through the background.
     */
    private void watchNetState() {
        File state = new File(GameData.userDir(new File(baseDir)), "data/touch_netstate");
        // Left by a process that was killed while connected: not this session's.
        state.delete();
        netStateWatch = new Thread(() -> {
            String shown = "";
            while (!Thread.currentThread().isInterrupted()) {
                String kind = "idle";
                String text = "";
                try (BufferedReader reader = new BufferedReader(new FileReader(state))) {
                    String line = reader.readLine();
                    if (line != null) {
                        kind = line.trim();
                        String second = reader.readLine();
                        text = second != null ? second.trim() : "";
                    }
                } catch (IOException e) {
                    // Not written yet: nothing to keep alive.
                }
                String now = kind + "\n" + text;
                boolean idle = kind.equals("idle") || kind.isEmpty();
                LanLocks.hosting(this, kind.equals("hosting")); // LAN game: Wi-Fi locks while hosting
                KeepAliveService.engineBusy = !idle;
                if (!now.equals(shown)) {
                    if (idle) {
                        KeepAliveService.stop(this);
                    } else {
                        KeepAliveService.show(this, KeepAliveService.KIND_CONNECTION, text);
                    }
                    shown = now;
                }
                try {
                    Thread.sleep(NETSTATE_POLL_MS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }, "xonotic-netstate");
        netStateWatch.setDaemon(true);
        netStateWatch.start();
    }

    @Override
    protected String getMainFunction() {
        return "AndySU_SDL_main";
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
            "-userdir", GameData.userDir(new File(baseDir)).getAbsolutePath()));
        // The Touch config chain packaging/start.sh runs on Linux.
        args.addAll(GameData.startupArguments(this, new File(baseDir)));
        if (nativeUi) {
            // the engine lets the native interface own the menu once it attaches
            args.add("+set");
            args.add("touch_native_frontend");
            args.add("1");
        }
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
            Log.i("Godnotic", "Extra engine arguments: " + extra);
        } catch (IOException e) {
            Log.w("Godnotic", "Could not read " + file, e);
        }
        return extra;
    }
}
