package io.github.dixonsolutions.xonotictouch;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Launcher screen: gets the basedir into shape, then hands off to the engine.
 *
 * This cannot live inside {@link XonoticActivity}. SDLActivity starts the native
 * thread from its own onCreate, and darkplaces scans the basedir during
 * FS_Init — long before any work we posted from Java would finish.
 */
public final class BootActivity extends Activity {

    private static final String TAG = "XonoticTouch";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private View root;
    private TextView status;
    private TextView detail;
    private ProgressBar progress;
    private Button retry;
    private Thread worker;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.boot);

        root = findViewById(R.id.boot_root);
        status = findViewById(R.id.boot_status);
        detail = findViewById(R.id.boot_detail);
        progress = findViewById(R.id.boot_progress);
        retry = findViewById(R.id.boot_retry);
        retry.setOnClickListener(v -> start());

        start();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (worker != null) {
            worker.interrupt();
        }
    }

    private void start() {
        if (worker != null && worker.isAlive()) {
            return;
        }
        retry.setVisibility(View.GONE);
        progress.setIndeterminate(true);

        GameData data = new GameData(this);
        worker = new Thread(() -> {
            AppUpdater updater = new AppUpdater(this);
            UpdateBridge bridge = new UpdateBridge(this, data.baseDir());
            String installed = updater.installedVersion();
            // Builds already installed, skipped or overtaken go; a half
            // download of the current release stays, to be resumed.
            updater.cleanCache();

            // An update the last session downloaded: this start is the "next
            // time you start" the Updates screen promised. No network needed.
            AppUpdater.Update ready = updater.readyUpdate();
            if (ready != null) {
                bridge.publishReady(installed, ready.version);
                if (AppUpdater.isAutoInstall(this)) {
                    ui.post(() -> installUpdate(updater, bridge, ready, data));
                } else {
                    ui.post(() -> offerUpdate(ready, data));
                }
                return;
            }

            // No network here. The check for a new release used to run before
            // the game could start, behind this screen, for every launch -- up
            // to 25 s on a bad connection. XonoticActivity now asks in the
            // background once the game is up; automatic updates download there
            // and install on the next start, and without them the main menu
            // says a release is out. Until that check answers, the menu shows
            // what the last one found.
            AppUpdater.Update known = updater.lastFound();
            if (known == null) {
                bridge.publishIdle(installed);
            } else if (updater.cachedBytes(known) > 0) {
                bridge.publishPaused(installed, known.version, updater.cachedPercent(known),
                        getString(R.string.update_status_paused));
            } else {
                bridge.publishAvailable(installed, known);
            }
            // Straight into the game when the data is in place: prepare()
            // reports, and so shows this screen, only when it has work to do.
            ui.post(() -> continueToGame(data));
        }, "xonotic-update-check");
        worker.start();
    }

    /**
     * Android confirms every package install itself, so this only decides
     * whether to hand the installer an APK — it can never update behind the
     * player's back, and declining drops straight into the game.
     */
    private void offerUpdate(AppUpdater.Update update, GameData data) {
        reveal();
        AppUpdater updater = new AppUpdater(this);
        UpdateBridge bridge = new UpdateBridge(this, data.baseDir());
        new AlertDialog.Builder(this)
            .setTitle(getString(R.string.update_title, update.version))
            .setMessage(getString(R.string.update_message, updater.installedVersion()))
            .setPositiveButton(R.string.update_install, (d, which) -> installUpdate(updater, bridge, update, data))
            .setNegativeButton(R.string.update_later, (d, which) -> continueToGame(data))
            .setNeutralButton(R.string.update_skip, (d, which) -> {
                updater.skip(update);
                // The status file still says `available` from the check a
                // moment ago. Left there, the in-game Updates screen spends the
                // session offering to install the release just declined here,
                // and only corrects itself once someone presses the button.
                bridge.publishUpToDate(updater.installedVersion());
                continueToGame(data);
            })
            .setOnCancelListener(d -> continueToGame(data))
            .show();
    }

    private void installUpdate(AppUpdater updater, UpdateBridge bridge,
                               AppUpdater.Update update, GameData data) {
        reveal();
        progress.setIndeterminate(true);
        status.setText(updater.isReady(update) ? R.string.update_installing : R.string.update_downloading);
        detail.setText("");
        String installed = updater.installedVersion();
        // The failure callback is a broadcast receiver, so it can land before
        // install() has returned -- at once, if launching the confirm dialog
        // throws. Publishing `installing` over it would put back the very state
        // the callback exists to clear.
        AtomicBoolean failed = new AtomicBoolean();
        worker = new Thread(() -> {
            try {
                if (!updater.install(update, (text, note, percent) -> {
                            bridge.publishDownloading(installed, update.version, percent);
                            report(text, note, percent);
                        },
                        () -> {
                            // install() returns once the session is committed,
                            // not once the player has confirmed it, so this is
                            // the only place that learns the prompt was
                            // dismissed. Without it the status file stays on
                            // `installing` and the Updates screen spends the
                            // rest of the session greyed out behind a bar that
                            // never finishes.
                            failed.set(true);
                            bridge.publishInstallFailed(installed, update.version,
                                    getString(R.string.update_status_failed));
                            ui.post(() -> continueToGame(data));
                        })) {
                    // Waiting on the "install unknown apps" toggle; the settings
                    // screen is up, so let the player back out into the game.
                    bridge.publishNeedsPermission(installed, update.version);
                    ui.post(() -> continueToGame(data));
                    return;
                }
                if (!failed.get()) {
                    bridge.publishInstalling(installed, update.version);
                }
                // On success the system takes over and restarts us as the new
                // build, so there is nothing left to do here.
            } catch (IOException | RuntimeException e) {
                // Same reasoning as the callback above: a download that broke
                // off is a reason to offer the retry, not to take it away. What
                // it fetched stays in the cache and the retry resumes it.
                Log.w(TAG, "Update install failed", e);
                if (updater.cachedBytes(update) > 0 && !updater.isReady(update)) {
                    bridge.publishPaused(installed, update.version, updater.cachedPercent(update),
                            getString(R.string.update_status_paused));
                } else {
                    bridge.publishInstallFailed(installed, update.version,
                            String.valueOf(e.getMessage()));
                }
                ui.post(() -> continueToGame(data));
            }
        }, "xonotic-update");
        worker.start();
    }

    /**
     * Several paths can end in here for the same launch -- a failed-install
     * broadcast and the exception from the same install, say -- and each used
     * to start its own preparation thread. Only the first one counts.
     */
    private final AtomicBoolean continued = new AtomicBoolean();

    private void continueToGame(GameData data) {
        if (!continued.compareAndSet(false, true)) {
            return;
        }
        worker = new Thread(() -> {
            try {
                data.prepare(this::report);
                ui.post(() -> launchEngine(data));
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "Game data preparation failed", e);
                ui.post(() -> {
                    // Retry goes through start() and may come back here.
                    continued.set(false);
                    showFailure(e);
                });
            }
        }, "xonotic-bootstrap");
        worker.start();
    }

    /** Show the screen: something is happening that the player waits for. */
    private void reveal() {
        if (root != null) {
            root.setVisibility(View.VISIBLE);
        }
    }

    /**
     * Progress only shows the screen once the work has gone on for a while.
     * Unpacking after an update writes just the files that changed and is
     * usually over in well under a second; flashing the screen up for that
     * was the slide the player saw on every update. The first download, or
     * a slow phone, still gets it.
     */
    private static final long REVEAL_AFTER_MS = 1500;
    private boolean revealScheduled;

    private void revealIfStillWorking() {
        if (revealScheduled) {
            return;
        }
        revealScheduled = true;
        ui.postDelayed(() -> {
            if (!isFinishing() && !isDestroyed()) {
                reveal();
            }
        }, REVEAL_AFTER_MS);
    }

    private void report(String text, String note, int percent) {
        ui.post(() -> {
            revealIfStillWorking();
            status.setText(text);
            detail.setText(note);
            if (percent < 0) {
                progress.setIndeterminate(true);
            } else {
                progress.setIndeterminate(false);
                progress.setProgress(percent);
            }
        });
    }

    private void launchEngine(GameData data) {
        // A screen that is already gone must not start a second engine.
        if (isFinishing() || isDestroyed()) {
            return;
        }
        Intent intent = new Intent(this, XonoticActivity.class);
        intent.putExtra(XonoticActivity.EXTRA_BASEDIR, data.baseDir().getAbsolutePath());
        startActivity(intent);
        finish();
    }

    private void showFailure(Throwable cause) {
        reveal();
        progress.setIndeterminate(false);
        progress.setProgress(0);
        status.setText(R.string.boot_failed);
        detail.setText(String.valueOf(cause.getMessage()));
        retry.setVisibility(View.VISIBLE);
    }
}
