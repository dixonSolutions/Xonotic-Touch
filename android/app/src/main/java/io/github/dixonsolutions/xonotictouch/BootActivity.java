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
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.drawable.Icon;
import android.text.format.Formatter;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.Collections;
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
 * fetched kept on disk (PackFetch keeps each pk3's chunks and their state),
 * unpacking stops between files (each lands under a temporary name and is
 * renamed when whole), and the screen then offers Resume and Quit.
 *
 * The pk3 download asks first: the setup step says how much it fetches and
 * how much space is free. With the split data set (docs/DATA_FETCH.md) that
 * is only first_play, the packs the first match needs; the game then starts
 * and {@link DataService} fetches the other maps and the music behind it,
 * without asking again. Without a manifest it is the single archive, music
 * included, as before the split.
 */
public class BootActivity extends Activity {

    private static final String TAG = "Godnotic";

    private static final String CHANNEL_PROGRESS = "downloads";
    private static final String CHANNEL_DONE = "downloads-done";
    private static final int NOTICE_PROGRESS = 1;
    private static final int NOTICE_DONE = 2;
    /** Set by the launcher's "Download music" shortcut. */
    static final String EXTRA_GET_MUSIC = "get_music";
    private static final String SHORTCUT_MUSIC = "music";

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
    /** The "Download music" shortcut opened this screen. */
    private static volatile boolean musicRequested;

    private View root;
    /** The banner alone, until the screen has something to say. */
    private View splash;
    private TextView explain;
    private TextView percentText;
    /** Notifications refused, and the like: what the player should know. */
    private TextView hint;
    private Button useMobile;
    private Button waitWifi;
    /** Waiting for an unmetered network to start an agreed download (Net.watchUnmetered). */
    private static volatile Object networkWatch;
    /** The player chose mobile data for this process's download. */
    private static volatile boolean mobileThisRun;
    private TextView status;
    private TextView detail;
    private ProgressBar progress;
    private Button retry;
    private Button cancel;
    private Button quit;
    /** The setup step: what the download costs, the music choice, Download. */
    private LinearLayout choice;
    private TextView choiceText;
    private TextView spaceText;
    private CheckBox musicBox;
    private Button download;
    private Button later;
    private Button recheck;
    /** What there is to download (GameData.Setup) while the setup step waits for the player. */
    private static volatile GameData.Setup pendingPlan;
    /** Download or Resume was pressed in this process: music on or off; null: ask. */
    private static volatile Boolean chosenMusic;
    /** The setup step is for the music pack only (the shortcut), the game is playable. */
    private static volatile boolean musicOnly;
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
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            // The system splash goes at once, without its zoom-out: the banner
            // screen underneath is already on the same black.
            getSplashScreen().setOnExitAnimationListener(view -> view.remove());
        }
        if (XonoticActivity.running) {
            // The launcher icon while the game runs in the background (the game
            // has a task of its own): back to it as it was. No update install and
            // no data preparation under a running engine, which would end the
            // player's match or join.
            // The rest of the split set carries on too, if something stopped it.
            DataService.start(this);
            startActivity(new Intent(this, XonoticActivity.class).addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION));
            overridePendingTransition(0, 0);
            finish();
            return;
        }
        buildScreen();
        retry.setOnClickListener(v -> start());
        cancel.setOnClickListener(v -> cancelPreparation());
        quit.setOnClickListener(v -> confirmQuit());
        download.setOnClickListener(v -> startDownload(true, false));
        useMobile.setOnClickListener(v -> {
            if (pendingPlan != null) {
                startDownload(true, true);
            } else {
                // Waiting for Wi-Fi: go ahead on this connection.
                mobileThisRun = true;
                saveConsent(this, consentMusic(this), true);
                DataService.setMobileAllowed(this, true);
                stopNetworkWatch();
                useMobile.setVisibility(View.GONE);
                waitWifi.setVisibility(View.GONE);
                continueToGame(new GameData(this));
            }
        });
        waitWifi.setOnClickListener(v -> {
            GameData.Setup plan = pendingPlan;
            if (plan == null) {
                return;
            }
            // Agreed, but not on this connection: it starts by itself on Wi-Fi.
            boolean music = true;
            saveConsent(this, music, false);
            DataService.setMobileAllowed(this, false);
            pendingPlan = null;
            chosenMusic = music;
            showWaitingForNetwork();
        });
        later.setOnClickListener(v -> {
            pendingPlan = null;
            musicRequested = false;
            musicOnly = false;
            launchEngine(new GameData(this));
        });
        recheck.setOnClickListener(v -> {
            GameData.Setup plan = pendingPlan;
            if (plan != null) {
                showChoice(plan);
            }
        });
        musicBox.setOnCheckedChangeListener((b, checked) -> {
            GameData.Setup plan = pendingPlan;
            if (plan != null) {
                updateChoice(plan);
            }
        });
        if (getIntent() != null && getIntent().getBooleanExtra(EXTRA_GET_MUSIC, false)) {
            musicRequested = true;
        }

        appContext = getApplicationContext();
        readyData = null;
        showing = this;
        start();
    }

    /**
     * The screen is built here, not inflated: under AndySU the layout and its
     * ids would come from the installed host's resource table, which need not
     * be this build's (see HostRes).
     *
     * Until there is something to wait for or decide, only the banner on the
     * game's dark background shows (BootSplash, the same picture
     * XonoticActivity keeps over the engine until its first frame). Then a
     * centred column at most 640 dp wide: the banner, a large title, one line
     * saying what is downloaded and how big, the bar with its percentage, the
     * transfer line (MB, speed, time left) and pill buttons.
     */
    private void buildScreen() {
        float dp = getResources().getDisplayMetrics().density;
        int screen = getResources().getDisplayMetrics().widthPixels;
        getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(BootSplash.BACKGROUND));

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = Math.round(24 * dp);
        box.setPadding(pad, pad, pad, pad);

        int width = Math.min(screen, Math.round(640 * dp));
        android.graphics.Bitmap bannerBitmap = BootSplash.banner(this, width);
        if (bannerBitmap != null) {
            android.widget.ImageView banner = new android.widget.ImageView(this);
            banner.setImageBitmap(bannerBitmap);
            banner.setAdjustViewBounds(true);
            banner.setContentDescription(HostRes.string(this, "app_name"));
            LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(
                    Math.round(width * 0.78f), LinearLayout.LayoutParams.WRAP_CONTENT);
            bp.bottomMargin = Math.round(8 * dp);
            box.addView(banner, bp);
        }

        status = new TextView(this);
        status.setTextColor(0xFFF2F4F8);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 28);
        status.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        status.setGravity(Gravity.CENTER);
        status.setText(HostRes.string(this, "boot_unpacking"));
        box.addView(status, wrap(Math.round(8 * dp)));

        explain = new TextView(this);
        explain.setTextColor(0xFFB4BAC6);
        explain.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        explain.setGravity(Gravity.CENTER);
        explain.setText(HostRes.string(this, "boot_explain_short"));
        box.addView(explain, wrap(Math.round(8 * dp)));

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        progress.setProgressTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        progress.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0xFF2A3342));
        progress.setIndeterminateTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        progress.setScaleY(1.6f);
        LinearLayout.LayoutParams bar = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, Math.round(16 * dp));
        bar.topMargin = Math.round(28 * dp);
        box.addView(progress, bar);

        percentText = new TextView(this);
        percentText.setTextColor(0xFFF2F4F8);
        percentText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 34);
        percentText.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        percentText.setGravity(Gravity.CENTER);
        percentText.setVisibility(View.GONE);
        box.addView(percentText, wrap(Math.round(12 * dp)));

        detail = new TextView(this);
        detail.setTextColor(0xFFB4BAC6);
        detail.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        detail.setGravity(Gravity.CENTER);
        detail.setText(HostRes.string(this, "boot_first_run"));
        box.addView(detail, wrap(Math.round(6 * dp)));

        // The setup step, shown instead of the bar until the player says Download.
        choice = new LinearLayout(this);
        choice.setOrientation(LinearLayout.VERTICAL);
        choice.setGravity(Gravity.CENTER_HORIZONTAL);
        choice.setVisibility(View.GONE);
        choiceText = new TextView(this);
        choiceText.setTextColor(0xFFF2F4F8);
        choiceText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 19);
        choiceText.setGravity(Gravity.CENTER);
        choice.addView(choiceText, wrap(Math.round(16 * dp)));
        musicBox = new CheckBox(this);
        musicBox.setTextColor(0xFFF2F4F8);
        musicBox.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        musicBox.setButtonTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        musicBox.setMinHeight(Math.round(56 * dp));
        musicBox.setPadding(Math.round(12 * dp), 0, Math.round(12 * dp), 0);
        choice.addView(musicBox, wrap(Math.round(12 * dp)));
        spaceText = new TextView(this);
        spaceText.setTextColor(0xFFB4BAC6);
        spaceText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        spaceText.setGravity(Gravity.CENTER);
        choice.addView(spaceText, wrap(Math.round(8 * dp)));
        box.addView(choice, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        box.addView(buttons, wrap(Math.round(28 * dp)));
        download = button(buttons, "boot_download", 0, true);
        useMobile = button(buttons, "boot_use_mobile", 0, true);
        waitWifi = button(buttons, "boot_wait_wifi", Math.round(16 * dp), false);
        recheck = button(buttons, "boot_check_again", Math.round(16 * dp), false);
        retry = button(buttons, "boot_retry", 0, true);
        cancel = button(buttons, "boot_cancel", 0, false);
        later = button(buttons, "boot_not_now", Math.round(16 * dp), false);
        quit = button(buttons, "boot_quit", Math.round(16 * dp), false);

        hint = new TextView(this);
        hint.setTextColor(0xFFFFC870);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        hint.setGravity(Gravity.CENTER);
        hint.setVisibility(View.GONE);
        box.addView(hint, wrap(Math.round(20 * dp)));

        // Comfortable on a phone held sideways: one column at most 640 dp wide,
        // scrolling when a short landscape screen cannot fit it.
        LinearLayout centre = new LinearLayout(this);
        centre.setGravity(Gravity.CENTER);
        centre.addView(box, new LinearLayout.LayoutParams(width, LinearLayout.LayoutParams.WRAP_CONTENT));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BootSplash.BACKGROUND);
        scroll.addView(centre, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        // Starts invisible: shown only when there is something to wait for
        // (unpacking, the setup step, the download, an update install, an error).
        scroll.setVisibility(View.INVISIBLE);
        root = scroll;
        android.widget.FrameLayout frame = new android.widget.FrameLayout(this);
        splash = BootSplash.view(this, 0.62f, 900);
        frame.addView(splash, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        frame.addView(scroll, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        setContentView(frame);
    }

    private static final int ACCENT = 0xFF3584E4;

    private static LinearLayout.LayoutParams wrap(int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        p.topMargin = top;
        return p;
    }

    /** A pill a thumb finds: 56 dp tall, 18 sp text, at least 160 dp wide; filled when primary. */
    private Button button(LinearLayout row, String text, int start, boolean primary) {
        float dp = getResources().getDisplayMetrics().density;
        Button b = new Button(this);
        b.setText(HostRes.string(this, text));
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        b.setTextColor(0xFFFFFFFF);
        b.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        android.graphics.drawable.GradientDrawable pill = new android.graphics.drawable.GradientDrawable();
        pill.setCornerRadius(28 * dp);
        pill.setColor(primary ? ACCENT : 0xFF273040);
        if (!primary) {
            pill.setStroke(Math.round(1 * dp), 0xFF3A4558);
        }
        b.setBackground(new android.graphics.drawable.RippleDrawable(
                android.content.res.ColorStateList.valueOf(0x40FFFFFF), pill, null));
        b.setStateListAnimator(null);
        b.setMinHeight(Math.round(56 * dp));
        b.setMinimumHeight(Math.round(56 * dp));
        b.setMinWidth(Math.round(160 * dp));
        b.setMinimumWidth(Math.round(160 * dp));
        b.setPadding(Math.round(28 * dp), 0, Math.round(28 * dp), 0);
        b.setVisibility(View.GONE);
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, Math.round(56 * dp));
        p.setMarginStart(start);
        row.addView(b, p);
        return b;
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
                        HostRes.string(this, "update_status_paused"));
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
            .setTitle(HostRes.string(this, "update_title", update.version))
            .setMessage(HostRes.string(this, "update_message", updater.installedVersion()))
            .setPositiveButton(HostRes.string(this, "update_install"), (d, which) -> installUpdate(updater, bridge, update, data))
            .setNegativeButton(HostRes.string(this, "update_later"), (d, which) -> continueToGame(data))
            .setNeutralButton(HostRes.string(this, "update_skip"), (d, which) -> {
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
        status.setText(HostRes.string(this, updater.isReady(update) ? "update_installing" : "update_downloading"));
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
                                    HostRes.string(this, "update_status_failed"));
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
                            HostRes.string(this, "update_status_paused"));
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
        GameData.Setup waiting = pendingPlan;
        if (waiting != null) {
            // The setup step was up when the last screen went: show it again.
            preparing.set(false);
            showChoice(waiting);
            return;
        }
        // Music is part of the game: a missing music pack is fetched like any
        // other. Only for an install of the single archive; with the split
        // set the music is one of the packs DataService fetches later.
        boolean music = data.musicMissing();
        Boolean agreed = chosenMusic;
        if (agreed == null && consentGiven(this) && data.assetsMissing()) {
            // Agreed in an earlier run that was quit, killed or lost its
            // connection: carry on without asking again.
            agreed = consentMusic(this);
            chosenMusic = agreed;
            Log.i(TAG, "Resuming the game data download agreed to earlier");
        }
        final Boolean chosen = agreed;
        if (chosen != null && (data.assetsMissing() || music)) {
            // Resume or Retry of a download the player already said yes to,
            // on a connection they accepted for it.
            if (!networkAllows(this)) {
                preparing.set(false);
                showWaitingForNetwork();
                return;
            }
            startService();
        } else if (data.assetsMissing() || music) {
            // The setup step comes up as soon as the file list is in.
            reveal();
        }
        Thread preparation = new Thread(() -> {
            try {
                data.prepare(BootActivity::report);
                // Split-set packs that are whole but unrecorded (a kill
                // between the rename and the save) count before the decision.
                data.session().reconcile();
                boolean missing = data.assetsMissing();
                if (!missing && !music) {
                    // Asked here, while no engine is up, so DataService knows
                    // which manifest to ask for when it starts beside one.
                    data.manifestUrl();
                    ui.post(() -> prepared(data));
                    return;
                }
                report(HostRes.string(appContext, "boot_checking"), "", -1);
                // The manifest first; the single archive when there is none.
                GameData.Setup plan = data.planSetup();
                if (plan.split != null) {
                    // What the split set needs before the game starts is
                    // first_play alone, and a complete old install needs
                    // nothing: no music step either way.
                    if (!data.assetsMissing()) {
                        ui.post(() -> prepared(data));
                        return;
                    }
                    if (chosen == null) {
                        ui.post(() -> planned(plan, false));
                        return;
                    }
                    data.download(plan, true, BootActivity::report);
                    ui.post(() -> prepared(data));
                    return;
                }
                if (chosen == null) {
                    ui.post(() -> planned(plan, !missing));
                    return;
                }
                data.download(plan, chosen || music, BootActivity::report);
                ui.post(() -> prepared(data));
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "Game data preparation failed", e);
                ui.post(() -> failed(e));
            }
        }, "xonotic-bootstrap");
        startPreparation(preparation);
    }

    private void startPreparation(Thread preparation) {
        worker = preparation;
        preparationThread = preparation;
        cancelRequested = false;
        preparation.start();
        syncCancel();
    }

    /**
     * Leaving the app mid-download must not freeze or end it (KeepAliveService);
     * started while this screen is in front, which Android 12+ requires.
     */
    private void startService() {
        reveal();
        requestNotificationPermission();
        KeepAliveService.show(this, KeepAliveService.KIND_DOWNLOAD,
                HostRes.string(this, "keepalive_download_text"));
    }

    /** The file list is in: the setup step waits for the player. */
    private static void planned(GameData.Setup plan, boolean onlyMusic) {
        preparationThread = null;
        pendingPlan = plan;
        musicOnly = onlyMusic;
        preparing.set(false);
        BootActivity live = visibleScreen();
        if (live != null) {
            live.showChoice(plan);
        }
    }

    /** The setup step: size, the music choice and the free space, then Download. */
    private void showChoice(GameData.Setup plan) {
        reveal();
        GameData data = new GameData(this);
        PackFetch.Entry music = plan.music();
        // No music choice: the music pack is always part of the download.
        musicBox.setVisibility(View.GONE);
        if (music != null) {
            musicBox.setText(HostRes.string(this, "boot_music_option", size(music.csize)));
        }
        if (musicOnly || musicRequested || data.musicWanted()) {
            musicBox.setChecked(true);
        }
        status.setText(HostRes.string(this, musicOnly ? "boot_setup_music_title" : "boot_setup_title"));
        detail.setText("");
        detail.setVisibility(View.GONE);
        percentText.setVisibility(View.GONE);
        progress.setVisibility(View.GONE);
        choice.setVisibility(View.VISIBLE);
        cancel.setVisibility(View.GONE);
        retry.setVisibility(View.GONE);
        quit.setVisibility(musicOnly ? View.GONE : View.VISIBLE);
        later.setVisibility(musicOnly ? View.VISIBLE : View.GONE);
        updateChoice(plan);
    }

    private void updateChoice(GameData.Setup plan) {
        File dir = new GameData(this).dataDir();
        PackFetch.Need need = plan.need(dir);
        long free = PackFetch.freeBytes(dir);
        StringBuilder text = new StringBuilder(HostRes.string(this,
                musicOnly ? "boot_setup_music_size" : "boot_setup_size", size(need.fetch)));
        if (need.resumed > 0) {
            text.append('\n').append(HostRes.string(this, "boot_setup_resumed", size(need.resumed)));
        }
        if (plan.rest() > 0) {
            // The split set: the choice made here (this connection, or Wi-Fi
            // only) also holds for what follows behind the game.
            text.append('\n').append(HostRes.string(this, "boot_setup_rest", size(plan.rest())));
        }
        Net.Kind net = Net.kind(this);
        boolean metered = net != Net.Kind.UNMETERED && net != Net.Kind.OFFLINE;
        if (net != Net.Kind.UNMETERED) {
            text.append("\n\n").append(Net.describe(this, net));
        }
        choiceText.setText(text);
        // Shown with the progress; here the setup text says it, with the choice.
        explain.setText(HostRes.string(this, "boot_explain", size(need.fetch + need.resumed)));
        explain.setVisibility(View.GONE);
        long needed = need.disk + PackFetch.SLACK_BYTES;
        boolean fits = free >= needed;
        spaceText.setTextColor(fits ? 0xFFB4BAC6 : 0xFFFF8A80);
        spaceText.setText(fits
                ? HostRes.string(this, "boot_space_free", size(free), size(needed))
                : HostRes.string(this, "boot_space_short", size(needed), size(free), size(needed - free)));
        download.setText(HostRes.string(this, need.resumed > 0 ? "boot_resume" : "boot_download"));
        // On mobile data or a metered Wi-Fi the choice is explicit: this
        // connection now, or start by itself on Wi-Fi.
        download.setVisibility(metered ? View.GONE : View.VISIBLE);
        download.setEnabled(fits);
        useMobile.setVisibility(metered ? View.VISIBLE : View.GONE);
        useMobile.setEnabled(fits);
        waitWifi.setVisibility(metered ? View.VISIBLE : View.GONE);
        waitWifi.setEnabled(fits);
        recheck.setVisibility(fits ? View.GONE : View.VISIBLE);
        if (metered) {
            quit.setVisibility(View.GONE); // three buttons fit a phone held sideways; Back still quits
        }
    }

    private String size(long bytes) {
        return Formatter.formatShortFileSize(this, bytes);
    }

    /** Download pressed: the planned packs, with or without music. */
    private void startDownload(boolean music, boolean mobile) {
        GameData.Setup plan = pendingPlan;
        if (plan == null || !preparing.compareAndSet(false, true)) {
            return;
        }
        pendingPlan = null;
        boolean withMusic = music || musicOnly;
        chosenMusic = withMusic;
        if (mobile) {
            mobileThisRun = true;
        }
        // Remembered until the data is in, so a quit, a kill or a lost
        // connection carries on next start without asking again.
        saveConsent(this, withMusic, mobile || consentMobile(this));
        // The consent above is forgotten once the game can start; what was
        // said about mobile data also governs the packs that follow.
        DataService.setMobileAllowed(this, mobile || consentMobile(this));
        useMobile.setVisibility(View.GONE);
        waitWifi.setVisibility(View.GONE);
        choice.setVisibility(View.GONE);
        download.setVisibility(View.GONE);
        recheck.setVisibility(View.GONE);
        later.setVisibility(View.GONE);
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(true);
        status.setText(HostRes.string(this, "boot_downloading"));
        detail.setText("");
        startService();
        GameData data = new GameData(this);
        startPreparation(new Thread(() -> {
            try {
                data.download(plan, withMusic, BootActivity::report);
                ui.post(() -> prepared(data));
            } catch (IOException | RuntimeException e) {
                Log.e(TAG, "Game data download failed", e);
                ui.post(() -> failed(e));
            }
        }, "xonotic-download"));
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
        cancel.setText(HostRes.string(this, "boot_cancelling"));
        running.interrupt();
    }

    /**
     * Quit while the download runs asks first: what is downloaded is kept,
     * and the next start carries on by itself (the consent is remembered).
     */
    private void confirmQuit() {
        Thread running = preparationThread;
        if (running == null || !running.isAlive()) {
            quitApp();
            return;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle(HostRes.string(this, "boot_quit_title"))
                .setMessage(HostRes.string(this, "boot_quit_message"))
                .setPositiveButton(HostRes.string(this, "boot_quit"), (d, w) -> quitApp())
                .setNegativeButton(HostRes.string(this, "boot_keep_downloading"), null)
                .show();
    }

    @Override
    public void onBackPressed() {
        Thread running = preparationThread;
        if (running != null && running.isAlive()) {
            confirmQuit();
            return;
        }
        super.onBackPressed();
    }

    // ------------------------------------------------------- consent, network

    private static android.content.SharedPreferences consent(Context context) {
        return context.getSharedPreferences("boot-download", Context.MODE_PRIVATE);
    }

    static void saveConsent(Context context, boolean music, boolean mobile) {
        consent(context).edit().putBoolean("agreed", true).putBoolean("music", music)
                .putBoolean("mobile", mobile).apply();
    }

    static boolean consentGiven(Context context) {
        return consent(context).getBoolean("agreed", false);
    }

    static boolean consentMusic(Context context) {
        return true; // music is always part of the game data
    }

    static boolean consentMobile(Context context) {
        return consent(context).getBoolean("mobile", false);
    }

    static void clearConsent(Context context) {
        if (context != null) {
            consent(context).edit().clear().apply();
        }
    }

    /** Unmetered, or the player accepted this connection for the download. */
    private static boolean networkAllows(Context context) {
        if (mobileThisRun || consentMobile(context)) {
            return true;
        }
        Net.Kind kind = Net.kind(context);
        // Offline goes ahead too: the download's own retries wait for a connection.
        return kind == Net.Kind.UNMETERED || kind == Net.Kind.OFFLINE;
    }

    /** An agreed download waits for Wi-Fi; it starts by itself when one comes. */
    private void showWaitingForNetwork() {
        reveal();
        choice.setVisibility(View.GONE);
        download.setVisibility(View.GONE);
        recheck.setVisibility(View.GONE);
        retry.setVisibility(View.GONE);
        later.setVisibility(View.GONE);
        percentText.setVisibility(View.GONE);
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(true);
        status.setText(HostRes.string(this, "boot_waiting_wifi"));
        detail.setVisibility(View.VISIBLE);
        detail.setText(Net.describe(this) + "\n" + HostRes.string(this, "boot_waiting_wifi_detail"));
        useMobile.setVisibility(View.VISIBLE);
        useMobile.setEnabled(true);
        waitWifi.setVisibility(View.GONE);
        quit.setVisibility(View.VISIBLE);
        Log.i(TAG, "Game data download waits for an unmetered network (" + Net.kind(this) + ")");
        if (networkWatch == null) {
            networkWatch = Net.watchUnmetered(getApplicationContext(), () -> ui.post(() -> {
                if (networkWatch == null) {
                    return;
                }
                stopNetworkWatch();
                BootActivity live = visibleScreen();
                Log.i(TAG, "Unmetered network: starting the agreed download");
                if (live != null) {
                    live.continueToGame(new GameData(live));
                }
            }));
        }
    }

    private void stopNetworkWatch() {
        Object watch = networkWatch;
        networkWatch = null;
        Net.stopWatching(getApplicationContext(), watch);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 1) {
            boolean granted = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            Log.i(TAG, "Notifications " + (granted ? "allowed" : "refused") + " for the download");
            showNotificationHint(!granted);
        }
    }

    private void showNotificationHint(boolean show) {
        if (hint == null) {
            return;
        }
        hint.setText(HostRes.string(this, "boot_notifications_off"));
        hint.setVisibility(show ? View.VISIBLE : View.GONE);
    }

    private void quitApp() {
        quitting = true;
        stopNetworkWatch();
        Thread running = preparationThread;
        if (running != null) {
            running.interrupt();
        }
        KeepAliveService.stop(this);
        finishAndRemoveTask();
    }

    private static void prepared(GameData data) {
        preparationThread = null;
        clearConsent(appContext);
        preparing.set(false);
        chosenMusic = null;
        musicRequested = false;
        musicOnly = false;
        KeepAliveService.stop(appContext);
        updateMusicShortcut(data);
        try {
            data.publishDataStatus();
        } catch (RuntimeException e) {
            Log.w(TAG, "Could not write the data status", e);
        }
        BootActivity live = visibleScreen();
        if (live != null) {
            live.launchEngine(data);
        } else {
            readyData = data;
            notifyDone(HostRes.string(appContext, "boot_notice_ready_title"),
                    HostRes.string(appContext, "boot_notice_ready_text"));
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
        if (cause instanceof PackFetch.NoSpace) {
            // Retry brings the setup step back, which says what to free.
            chosenMusic = null;
        }
        BootActivity live = visibleScreen();
        if (live != null) {
            live.showFailure(cause);
        } else {
            failedWhileAway = true;
            notifyDone(HostRes.string(appContext, "boot_notice_failed_title"),
                    HostRes.string(appContext, "boot_notice_failed_text"));
        }
    }

    /** Show the screen: something is happening that the player waits for. */
    private void reveal() {
        if (root != null) {
            root.setVisibility(View.VISIBLE);
        }
        if (splash != null) {
            splash.setVisibility(View.GONE);
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
                // The download's notification follows along while the screen
                // shows too (the service owns it), once a second.
                long now = android.os.SystemClock.elapsedRealtime();
                if (chosenMusic != null && now - lastNotified >= 1000) {
                    lastNotified = now;
                    notifyProgress(text, note, percent);
                }
            } else {
                notifyProgress(text, note, percent);
            }
        });
    }

    private static long lastNotified;

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
            cancel.setText(HostRes.string(this, "boot_cancel"));
        }
        if (working) {
            // Quit stays: it asks first (confirmQuit), and the next start
            // carries on by itself.
            quit.setVisibility(View.VISIBLE);
            retry.setVisibility(View.GONE);
        }
    }

    private void applyProgress(String text, String note, int percent) {
        syncCancel();
        if (choice.getVisibility() == View.VISIBLE) {
            // The setup step waits for the player; nothing runs behind it.
            return;
        }
        progress.setVisibility(View.VISIBLE);
        detail.setVisibility(View.VISIBLE);
        explain.setVisibility(View.VISIBLE);
        status.setText(text);
        detail.setText(note);
        if (percent < 0) {
            progress.setIndeterminate(true);
            percentText.setVisibility(View.GONE);
        } else {
            progress.setIndeterminate(false);
            progress.setProgress(percent);
            percentText.setVisibility(View.VISIBLE);
            percentText.setText(percent + "%");
        }
    }

    /** The banner has been on screen at least once (see launchEngine). */
    private boolean bannerDrawn;

    private void launchEngine(GameData data) {
        // A screen that is already gone must not start a second engine.
        if (isFinishing() || isDestroyed()) {
            return;
        }
        if (!bannerDrawn) {
            // Hand over only once the banner is on screen: the game's window
            // starts without a preview (Theme.XonoticGame), so this screen
            // stays visible until the game draws the same banner over it.
            View decor = getWindow().getDecorView();
            decor.getViewTreeObserver().addOnPreDrawListener(new android.view.ViewTreeObserver.OnPreDrawListener() {
                @Override
                public boolean onPreDraw() {
                    decor.getViewTreeObserver().removeOnPreDrawListener(this);
                    bannerDrawn = true;
                    decor.postOnAnimation(() -> launchEngine(data));
                    return true;
                }
            });
            decor.invalidate();
            return;
        }
        Intent intent = new Intent(this, XonoticActivity.class);
        intent.putExtra(XonoticActivity.EXTRA_BASEDIR, data.baseDir().getAbsolutePath());
        if (getIntent() != null && getIntent().getData() != null) {
            intent.setData(getIntent().getData()); // a godnotic://join link
        }
        // The same banner at the same place on both sides: no animation at
        // all, so the hand-off is invisible (one screen from the icon to Home).
        intent.addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION);
        startActivity(intent);
        overridePendingTransition(0, 0);
        finish();
        overridePendingTransition(0, 0);
    }

    private void showFailure(Throwable cause) {
        reveal();
        choice.setVisibility(View.GONE);
        download.setVisibility(View.GONE);
        recheck.setVisibility(View.GONE);
        later.setVisibility(View.GONE);
        progress.setVisibility(View.VISIBLE);
        quit.setVisibility(View.VISIBLE);
        // A read timeout is an InterruptedIOException too, but no pause.
        if (cancelRequested || (cause instanceof InterruptedIOException
                && !(cause instanceof java.net.SocketTimeoutException))) {
            cancelRequested = false;
            progress.setIndeterminate(false);
            status.setText(HostRes.string(this, "boot_paused"));
            // "412 / 1027 MB · 6.1 MB/s": the speed means nothing once stopped.
            String where = lastDetail;
            int speed = where.indexOf(" · ");
            if (speed > 0) {
                where = where.substring(0, speed);
            }
            detail.setText(where.isEmpty() ? HostRes.string(this, "boot_paused_detail_none")
                    : HostRes.string(this, "boot_paused_detail", where));
            retry.setText(HostRes.string(this, "boot_resume"));
            retry.setVisibility(View.VISIBLE);
            return;
        }
        retry.setText(HostRes.string(this, "boot_retry"));
        progress.setIndeterminate(false);
        progress.setProgress(0);
        status.setText(HostRes.string(this, "boot_failed"));
        detail.setText(String.valueOf(cause.getMessage()));
        retry.setVisibility(View.VISIBLE);
    }

    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission("android.permission.POST_NOTIFICATIONS")
                        != PackageManager.PERMISSION_GRANTED) {
            if (shouldShowRequestPermissionRationale("android.permission.POST_NOTIFICATIONS")
                    || !consent(this).getBoolean("asked-notifications", false)) {
                consent(this).edit().putBoolean("asked-notifications", true).apply();
                requestPermissions(new String[] { "android.permission.POST_NOTIFICATIONS" }, 1);
            } else {
                // Refused for good: Android will not ask again, so say what it costs.
                showNotificationHint(true);
            }
        }
    }

    /**
     * "Download music" on a long press of the launcher icon while the music
     * pack is missing. Built in code (no res/xml), for the same reason as the
     * screen: under AndySU the host's resources need not be this build's.
     */
    private static void updateMusicShortcut(GameData data) {
        // Music is part of the game data now: the "Download music" shortcut an
        // earlier release added goes, and none is added.
        if (Build.VERSION.SDK_INT < 25) {
            return;
        }
        try {
            ShortcutManager shortcuts = appContext.getSystemService(ShortcutManager.class);
            if (shortcuts != null) {
                shortcuts.removeDynamicShortcuts(Collections.singletonList(SHORTCUT_MUSIC));
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Could not remove the music shortcut", e);
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
                .setOnlyAlertOnce(true)
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
                    HostRes.string(appContext, "boot_notice_channel"), importance));
        }
    }

    private static Notification.Builder builder(String channel) {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(appContext, channel)
                : new Notification.Builder(appContext);
        if (Build.VERSION.SDK_INT >= 31) {
            // The download notice shows at once, not after Android 12's 10 s grace.
            b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        }
        return b;
    }

    /**
     * The app's launcher entry, as the icon starts it: GodnoticBootActivity on
     * hosts that have it, BootActivity on older ones. Brings the running task
     * to the front rather than stacking a second boot screen.
     */
    static Intent launcherIntent(Context context) {
        Intent intent = context.getPackageManager().getLaunchIntentForPackage(context.getPackageName());
        if (intent == null) {
            intent = new Intent(context, BootActivity.class);
        }
        return intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED);
    }

    private static PendingIntent openBoot() {
        Intent intent = launcherIntent(appContext);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT
                | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
        return PendingIntent.getActivity(appContext, 0, intent, flags);
    }
}
