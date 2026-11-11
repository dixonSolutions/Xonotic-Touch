package io.github.dixonsolutions.xonotictouch;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Launcher screen: gets the basedir into shape, then hands off to the engine.
 *
 * This cannot live inside {@link XonoticActivity}. SDLActivity starts the native
 * thread from its own onCreate, and darkplaces scans the basedir during
 * FS_Init — long before any work we posted from Java would finish.
 *
 * Preparation belongs to the process, not to this screen: leaving the screen
 * during a download leaves the download running. If it finishes while nobody is
 * looking, a notification says so, and tapping it comes back here.
 *
 * While it works the screen offers Cancel: the download stops with what it
 * fetched kept on disk (GameData stages it in parts), unpacking stops between
 * files (each lands under a temporary name and is renamed when whole), and
 * the screen then offers Resume and Quit.
 */
public final class BootActivity extends Activity {

    private static final String TAG = "XonoticTouch";

    private static final String CHANNEL_PROGRESS = "downloads";
    private static final String CHANNEL_DONE = "downloads-done";
    private static final int NOTICE_PROGRESS = 1;
    private static final int NOTICE_DONE = 2;

    private static final Handler ui = new Handler(Looper.getMainLooper());

    /** The screen the player is looking at, or null when none is. */
    private static volatile BootActivity showing;
    private static volatile Context appContext;
    /** One preparation at a time for the whole process, whichever screen asked. */
    private static final AtomicBoolean preparing = new AtomicBoolean();
    /** What the last progress report said, so a screen that comes back shows it. */
    private static volatile String lastStatus = "";
    private static volatile String lastDetail = "";
    private static volatile int lastPercent = -1;
    /** Finished while no boot screen was showing; the next one resumed takes it. */
    private static volatile GameData readyData;
    private static volatile boolean failedWhileAway;

    private View root;
    private TextView status;
    private TextView detail;
    private ProgressBar progress;
    private Button retry;
    private Button cancel;
    private Button quit;
    private Thread worker;
    /** The process's one preparation thread while it runs, whichever screen started it. */
    private static volatile Thread preparationThread;
    /** Cancel was pressed: the preparation's end is a pause, not a failure. */
    private static volatile boolean cancelRequested;
    /** Quit was pressed: the stopped preparation needs no notification. */
    private static volatile boolean quitting;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (XonoticActivity.running) {
            // The launcher icon while the game runs in the background (the game
            // has a task of its own): back to it as it was. No update install and
            // no data preparation under a running engine, which would end the
            // player's match or join.
            startActivity(new Intent(this, XonoticActivity.class));
            finish();
            return;
        }
        setContentView(R.layout.boot);

        root = findViewById(R.id.boot_root);
        status = findViewById(R.id.boot_status);
        detail = findViewById(R.id.boot_detail);
        progress = findViewById(R.id.boot_progress);
        retry = findViewById(R.id.boot_retry);
        retry.setOnClickListener(v -> start());
        cancel = findViewById(R.id.boot_cancel);
        cancel.setOnClickListener(v -> cancelPreparation());
        quit = findViewById(R.id.boot_quit);
        quit.setOnClickListener(v -> quitApp());

        appContext = getApplicationContext();
        readyData = null;
        showing = this;
        start();
    }

    @Override
    protected void onResume() {
        super.onResume();
        showing = this;
        GameData ready = readyData;
        if (ready != null) {
            readyData = null;
            launchEngine(ready);
        } else if (failedWhileAway) {
            failedWhileAway = false;
            start();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (showing == this) {
            showing = null;
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (showing == this) {
            showing = null;
        }
    }

    private void start() {
        if (worker != null && worker.isAlive()) {
            return;
        }
        retry.setVisibility(View.GONE);
        quit.setVisibility(View.GONE);
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

    private void continueToGame(GameData data) {
        if (!preparing.compareAndSet(false, true)) {
            // Already preparing, started by an earlier screen: show where it is.
            if (data.assetsMissing()) {
                reveal();
            }
            applyProgress(lastStatus, lastDetail, lastPercent);
            return;
        }
        if (data.assetsMissing()) {
            // Something to wait for from the first frame: the screen shows at
            // once, not after the delay that hides the quick unpack.
            reveal();
            requestNotificationPermission();
            // Leaving the app mid-download must not freeze or end it
            // (KeepAliveService); started now, while this screen is in front.
            KeepAliveService.show(this, KeepAliveService.KIND_DOWNLOAD,
                    getString(R.string.keepalive_download_text));
        }
        Thread preparation = new Thread(() -> {
            try {
                data.prepare(BootActivity::report);
                ui.post(() -> prepared(data));
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "Game data preparation failed", e);
                ui.post(() -> failed(e));
            }
        }, "xonotic-bootstrap");
        worker = preparation;
        preparationThread = preparation;
        cancelRequested = false;
        preparation.start();
        syncCancel();
    }

    /** The screen the player can see right now, or null once it has gone. */
    private static BootActivity visibleScreen() {
        BootActivity live = showing;
        return live != null && !live.isFinishing() && !live.isDestroyed() ? live : null;
    }

    /** Stop the download or unpack; it ends in failed() with an InterruptedIOException. */
    private void cancelPreparation() {
        Thread running = preparationThread;
        if (running == null || !running.isAlive()) {
            return;
        }
        cancelRequested = true;
        quitting = false;
        cancel.setEnabled(false);
        cancel.setText(R.string.boot_cancelling);
        running.interrupt();
    }

    private void quitApp() {
        quitting = true;
        Thread running = preparationThread;
        if (running != null) {
            running.interrupt();
        }
        KeepAliveService.stop(this);
        finishAndRemoveTask();
    }

    private static void prepared(GameData data) {
        preparationThread = null;
        preparing.set(false);
        KeepAliveService.stop(appContext);
        BootActivity live = visibleScreen();
        if (live != null) {
            live.launchEngine(data);
        } else {
            readyData = data;
            notifyDone(appContext.getString(R.string.boot_notice_ready_title),
                    appContext.getString(R.string.boot_notice_ready_text));
        }
    }

    private static void failed(Throwable cause) {
        preparationThread = null;
        if (quitting) {
            quitting = false;
            preparing.set(false);
            KeepAliveService.stop(appContext);
            return;
        }
        preparing.set(false);
        KeepAliveService.stop(appContext);
        BootActivity live = visibleScreen();
        if (live != null) {
            live.showFailure(cause);
        } else {
            failedWhileAway = true;
            notifyDone(appContext.getString(R.string.boot_notice_failed_title),
                    appContext.getString(R.string.boot_notice_failed_text));
        }
    }

    /** Show the screen: something is happening that the player waits for. */
    private void reveal() {
        if (root != null) {
            root.setVisibility(View.VISIBLE);
        }
        syncCancel();
    }

    /**
     * Progress only shows the screen once the work has gone on for a while.
     * Unpacking after an update writes just the files that changed and is
     * usually over in well under a second; flashing the screen up for that
     * was the slide the player saw on every update. A download always shows
     * at once (continueToGame), so this only covers the quick unpack.
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

    private static void report(String text, String note, int percent) {
        lastStatus = text;
        lastDetail = note;
        lastPercent = percent;
        ui.post(() -> {
            BootActivity live = visibleScreen();
            if (live != null) {
                live.revealIfStillWorking();
                live.applyProgress(text, note, percent);
            } else {
                notifyProgress(text, note, percent);
            }
        });
    }

    /** Cancel while a preparation runs and has not been cancelled yet. */
    private void syncCancel() {
        Thread running = preparationThread;
        boolean working = running != null && running.isAlive();
        if (cancel == null) {
            return;
        }
        cancel.setVisibility(working ? View.VISIBLE : View.GONE);
        if (working && !cancelRequested) {
            cancel.setEnabled(true);
            cancel.setText(R.string.boot_cancel);
        }
        if (working) {
            quit.setVisibility(View.GONE);
            retry.setVisibility(View.GONE);
        }
    }

    private void applyProgress(String text, String note, int percent) {
        syncCancel();
        status.setText(text);
        detail.setText(note);
        if (percent < 0) {
            progress.setIndeterminate(true);
        } else {
            progress.setIndeterminate(false);
            progress.setProgress(percent);
        }
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
        quit.setVisibility(View.VISIBLE);
        if (cancelRequested || cause instanceof InterruptedIOException) {
            cancelRequested = false;
            progress.setIndeterminate(false);
            status.setText(R.string.boot_paused);
            detail.setText(lastDetail.isEmpty() ? getString(R.string.boot_paused_detail_none)
                    : getString(R.string.boot_paused_detail, lastDetail));
            retry.setText(R.string.boot_resume);
            retry.setVisibility(View.VISIBLE);
            return;
        }
        retry.setText(R.string.boot_retry);
        progress.setIndeterminate(false);
        progress.setProgress(0);
        status.setText(R.string.boot_failed);
        detail.setText(String.valueOf(cause.getMessage()));
        retry.setVisibility(View.VISIBLE);
    }

    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                        != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, 1);
        }
    }

    private static void notifyProgress(String text, String note, int percent) {
        NotificationManager manager = notifications();
        createChannel(manager, CHANNEL_PROGRESS, NotificationManager.IMPORTANCE_LOW);
        manager.notify(NOTICE_PROGRESS, builder(CHANNEL_PROGRESS)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(text)
                .setContentText(note)
                .setOngoing(true)
                .setProgress(100, Math.max(0, percent), percent < 0)
                .setContentIntent(openBoot())
                .build());
    }

    private static void notifyDone(String title, String text) {
        NotificationManager manager = notifications();
        manager.cancel(NOTICE_PROGRESS);
        createChannel(manager, CHANNEL_DONE, NotificationManager.IMPORTANCE_DEFAULT);
        manager.notify(NOTICE_DONE, builder(CHANNEL_DONE)
                .setSmallIcon(android.R.drawable.stat_sys_download_done)
                .setContentTitle(title)
                .setContentText(text)
                .setAutoCancel(true)
                .setContentIntent(openBoot())
                .build());
    }

    private static NotificationManager notifications() {
        return (NotificationManager) appContext.getSystemService(Context.NOTIFICATION_SERVICE);
    }

    private static void createChannel(NotificationManager manager, String id, int importance) {
        if (Build.VERSION.SDK_INT >= 26) {
            manager.createNotificationChannel(new NotificationChannel(id,
                    appContext.getString(R.string.boot_notice_channel), importance));
        }
    }

    private static Notification.Builder builder(String channel) {
        return Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(appContext, channel)
                : new Notification.Builder(appContext);
    }

    private static PendingIntent openBoot() {
        Intent intent = new Intent(appContext, BootActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT
                | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
        return PendingIntent.getActivity(appContext, 0, intent, flags);
    }
}
