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
    private TextView status;
    private TextView detail;
    private ProgressBar progress;
    private Button retry;
    private Thread worker;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.boot);

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
            // Ask about a new build before unpacking anything: an update
            // replaces the payload we would otherwise be extracting.
            AppUpdater.Update update = new AppUpdater(this).findUpdate();
            if (update != null) {
                ui.post(() -> offerUpdate(update, data));
            } else {
                ui.post(() -> continueToGame(data));
            }
        }, "xonotic-update-check");
        worker.start();
    }

    /**
     * Android confirms every package install itself, so this only decides
     * whether to hand the installer an APK — it can never update behind the
     * player's back, and declining drops straight into the game.
     */
    private void offerUpdate(AppUpdater.Update update, GameData data) {
        AppUpdater updater = new AppUpdater(this);
        new AlertDialog.Builder(this)
            .setTitle(getString(R.string.update_title, update.version))
            .setMessage(getString(R.string.update_message, updater.installedVersion()))
            .setPositiveButton(R.string.update_install, (d, which) -> installUpdate(updater, update, data))
            .setNegativeButton(R.string.update_later, (d, which) -> continueToGame(data))
            .setNeutralButton(R.string.update_skip, (d, which) -> {
                updater.skip(update);
                continueToGame(data);
            })
            .setOnCancelListener(d -> continueToGame(data))
            .show();
    }

    private void installUpdate(AppUpdater updater, AppUpdater.Update update, GameData data) {
        progress.setIndeterminate(true);
        status.setText(R.string.update_downloading);
        detail.setText("");
        worker = new Thread(() -> {
            try {
                if (!updater.install(update, this::report,
                        () -> ui.post(() -> continueToGame(data)))) {
                    // Waiting on the "install unknown apps" toggle; the settings
                    // screen is up, so let the player back out into the game.
                    ui.post(() -> continueToGame(data));
                }
                // On success the system takes over and restarts us as the new
                // build, so there is nothing left to do here.
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "Update install failed", e);
                ui.post(() -> continueToGame(data));
            }
        }, "xonotic-update");
        worker.start();
    }

    private void continueToGame(GameData data) {
        worker = new Thread(() -> {
            try {
                data.prepare(this::report);
                ui.post(() -> launchEngine(data));
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "Game data preparation failed", e);
                ui.post(() -> showFailure(e));
            }
        }, "xonotic-bootstrap");
        worker.start();
    }

    private void report(String text, String note, int percent) {
        ui.post(() -> {
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
        Intent intent = new Intent(this, XonoticActivity.class);
        intent.putExtra(XonoticActivity.EXTRA_BASEDIR, data.baseDir().getAbsolutePath());
        startActivity(intent);
        finish();
    }

    private void showFailure(Throwable cause) {
        progress.setIndeterminate(false);
        progress.setProgress(0);
        status.setText(R.string.boot_failed);
        detail.setText(String.valueOf(cause.getMessage()));
        retry.setVisibility(View.VISIBLE);
    }
}
