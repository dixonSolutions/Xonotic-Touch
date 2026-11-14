package io.github.dixonsolutions.xonotictouch;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;

/**
 * Fetches the rest of the split data set behind the running game: the other
 * maps, the music, the detail and extras packs (docs/DATA_FETCH.md, "Order").
 * The boot screen only waits for first_play; this starts with the game and
 * asks nothing.
 *
 * The work is one thread of the process ({@link DataSession#background}). The
 * service is what keeps that process alive, and its network open, once the
 * player leaves the app: a foreground service of type dataSync with a quiet
 * notification ("Getting the rest of the maps: 180 of 300 MB"), up only while
 * packs are actually being fetched. It is started from the game's onCreate,
 * while the app is in front, which is the only time Android 12+ allows.
 * Android 15 gives dataSync six hours a day and then calls {@link #onTimeout}:
 * the service stops and the thread goes on only while the game is open. A
 * killed process loses nothing: parts and their chunk states are on disk, and
 * the next start of the game carries on.
 *
 * Mobile data: the remainder follows what the player said at the first
 * download ("Use mobile data" or "Wait for Wi-Fi", remembered here because
 * BootActivity forgets its own consent once the game can start). On a metered
 * network without that yes the state is {@code waiting} and nothing is
 * fetched.
 *
 * Under an old AndySU host nothing here may be assumed: its manifest does not
 * declare this service (a payload update brings code, not a manifest), and
 * libnativeui.so may be missing. An undeclared or refused service leaves the
 * thread running for as long as the game activity lives; the status file
 * tells the engine about landed packs where the JNI queue cannot.
 */
public final class DataService extends Service {

    private static final String TAG = "Godnotic";

    /** BootActivity's and KeepAliveService's download channel: low importance, no sound. */
    private static final String CHANNEL = "downloads";
    private static final int NOTICE = 4;
    private static final String PREFS = "data-fetch";

    /** The process's one background run; guarded by the class. */
    private static Thread runner;
    private static volatile boolean stopRequested;
    /** The run is fetching and wants to outlive the activity. */
    private static volatile boolean serviceWanted;
    /** startForegroundService was sent; onStartCommand has not answered yet. */
    private static volatile boolean startPending;
    /** The instance that is in the foreground, or null. */
    private static volatile DataService live;
    /** Not declared by this host, or out of its daily time: no more tries in this process. */
    private static volatile boolean serviceUnavailable;
    /** After a refusal (started from the background): not again before this. */
    private static volatile long nextServiceTry;
    private static volatile String noticeText = "";
    private static volatile int noticeDone;
    private static volatile int noticeTotal;

    private PowerManager.WakeLock wakeLock;

    // ------------------------------------------------------- the player's yes

    /** "Use mobile data" (true) or "Wait for Wi-Fi" (false) at the first download. */
    static void setMobileAllowed(Context context, boolean allowed) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                .putBoolean("mobile", allowed).apply();
    }

    static boolean mobileAllowed(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean("mobile", false);
    }

    /** An unmetered network, or a metered one the player accepted. Offline is no. */
    static boolean mayFetch(Context context) {
        try {
            Net.Kind kind = Net.kind(context);
            if (kind == Net.Kind.OFFLINE) {
                return false;
            }
            return kind == Net.Kind.UNMETERED || mobileAllowed(context);
        } catch (RuntimeException e) {
            Log.w(TAG, "Cannot tell what network this is; not downloading", e);
            return false;
        }
    }

    // ---------------------------------------------------------------- the run

    /**
     * Starts the background download if there is one to do and none runs.
     * Called while the game is in front (XonoticActivity.onCreate, and the
     * boot screen when it hands back to a running game). Never throws.
     */
    static void start(Context context) {
        try {
            Context app = context.getApplicationContext();
            synchronized (DataService.class) {
                if (runner != null && runner.isAlive()) {
                    return;
                }
                stopRequested = false;
                runner = new Thread(() -> run(app), "godnotic-data");
                runner.setDaemon(true);
                runner.start();
            }
        } catch (RuntimeException | LinkageError e) {
            Log.w(TAG, "The background data download could not start", e);
        }
    }

    private static void run(Context app) {
        try {
            GameData data = new GameData(app);
            DataSession session = data.session();
            // The GPU is not asked here: the engine is bringing up EGL on its
            // own thread right now. The boot screen asked and remembered.
            String url = data.knownManifestUrl();
            if (session.manifest() == null && url == null) {
                return;
            }
            session.background(url, new RunHost(app, session));
        } catch (RuntimeException | LinkageError e) {
            Log.w(TAG, "The background data download stopped", e);
        } finally {
            dropService(app);
        }
    }

    /** What the session sees of Android. */
    private static final class RunHost implements DataSession.Host {
        private final Context app;
        private final DataSession session;
        private long gateChecked;
        private boolean gate;
        private String shown = "";

        RunHost(Context app, DataSession session) {
            this.app = app;
            this.session = session;
        }

        @Override
        public synchronized boolean mayFetch() {
            // Asked before every chunk; the answer is a binder call.
            long now = SystemClock.elapsedRealtime();
            if (gateChecked == 0 || now - gateChecked >= 1000) {
                gateChecked = now;
                gate = DataService.mayFetch(app);
            }
            return gate;
        }

        @Override
        public boolean online() {
            try {
                return Net.kind(app) != Net.Kind.OFFLINE;
            } catch (RuntimeException e) {
                return false;
            }
        }

        @Override
        public boolean keepGoing() {
            // With the service up the process is kept and the download goes
            // on without the game; without it (an old host, a refusal, the
            // daily limit) only while the game activity lives.
            return !stopRequested && (live != null || startPending || XonoticActivity.running);
        }

        @Override
        public void landed(String installAs) {
            if (!XonoticActivity.running) {
                return;
            }
            // The quick way; the status file's `landed` lines are the sure
            // one (touch_datafetch.c mounts what it has not mounted), so a
            // missing bridge or a full queue loses nothing.
            try {
                if (NativeBridge.getAvailable()) {
                    NativeBridge.enqueue("fs_mountpack " + installAs);
                }
            } catch (RuntimeException | LinkageError e) {
                Log.w(TAG, "No native bridge to mount " + installAs + " through", e);
            }
        }

        @Override
        public void changed() {
            try {
                String state = session.state();
                if (!DataStatus.FETCHING.equals(state)) {
                    // Waiting for Wi-Fi, an error, or done: no notification
                    // sits there for hours, and nothing is kept awake.
                    dropService(app);
                    shown = "";
                    return;
                }
                long[] progress = session.setProgress();
                String text = HostRes.string(app, "data_rest_progress",
                        GameData.megabytes(progress[0]), GameData.megabytes(progress[1]));
                noticeText = text;
                noticeDone = (int) (progress[0] >> 10);
                noticeTotal = (int) (progress[1] >> 10);
                wantService(app);
                DataService service = live;
                if (service != null && !text.equals(shown)) {
                    shown = text;
                    NotificationManager manager =
                            (NotificationManager) app.getSystemService(Context.NOTIFICATION_SERVICE);
                    manager.notify(NOTICE, notice(app));
                }
            } catch (RuntimeException | LinkageError e) {
                Log.w(TAG, "Could not show the download's progress", e);
            }
        }
    }

    // ------------------------------------------------------------ the service

    private static boolean declared(Context app) {
        try {
            app.getPackageManager().getServiceInfo(new ComponentName(app, DataService.class), 0);
            return true;
        } catch (PackageManager.NameNotFoundException | RuntimeException e) {
            return false;
        }
    }

    private static void wantService(Context app) {
        serviceWanted = true;
        if (live != null || startPending || serviceUnavailable
                || SystemClock.elapsedRealtime() < nextServiceTry) {
            return;
        }
        if (!declared(app)) {
            // An AndySU host from before this service: its manifest is frozen.
            serviceUnavailable = true;
            Log.i(TAG, "This host does not declare DataService: the rest of the data downloads "
                    + "only while the game is open");
            return;
        }
        try {
            startPending = true;
            Intent intent = new Intent(app, DataService.class);
            if (Build.VERSION.SDK_INT >= 26) {
                app.startForegroundService(intent);
            } else {
                app.startService(intent);
            }
        } catch (RuntimeException e) {
            // ForegroundServiceStartNotAllowedException (31+): the app is not
            // in front. Tried again later, when it may be.
            startPending = false;
            nextServiceTry = SystemClock.elapsedRealtime() + 30_000;
            Log.w(TAG, "The data download cannot run as a service now: " + e);
        }
    }

    private static void dropService(Context app) {
        serviceWanted = false;
        DataService service = live;
        if (service != null) {
            // Not stopService() while a start is pending: a service asked to
            // start in the foreground has to get there first, or Android
            // kills the app. onStartCommand sees serviceWanted and stops.
            service.stopSelf();
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        startPending = false;
        try {
            if (Build.VERSION.SDK_INT >= 29) {
                startForeground(NOTICE, notice(this), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(NOTICE, notice(this));
            }
        } catch (RuntimeException e) {
            // Refused (the daily dataSync time is used up, or not in front).
            Log.w(TAG, "Foreground service refused for the data download", e);
            nextServiceTry = SystemClock.elapsedRealtime() + 60_000;
            stopSelf();
            return START_NOT_STICKY;
        }
        Thread run;
        synchronized (DataService.class) {
            run = runner;
        }
        if (intent == null || !serviceWanted || run == null || !run.isAlive()) {
            // Restarted without its process's run, or no longer needed.
            stopSelf();
            return START_NOT_STICKY;
        }
        live = this;
        if (wakeLock == null) {
            try {
                PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
                wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Godnotic:data");
                wakeLock.setReferenceCounted(false);
                // Never longer than the service may live anyway.
                wakeLock.acquire(6 * 60 * 60 * 1000L);
            } catch (RuntimeException e) {
                Log.w(TAG, "No wake lock for the data download", e);
            }
        }
        return START_NOT_STICKY;
    }

    /** Android 15+: the dataSync time for today is used up. Stop cleanly; the parts are on disk. */
    @Override
    public void onTimeout(int startId, int fgsType) {
        Log.i(TAG, "The data download's background time for today is used up");
        serviceUnavailable = true;
        serviceWanted = false;
        stopSelf();
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // Swiped away from the recent apps: the player is done with the game.
        // The next start carries on from what is on disk.
        stopRequested = true;
        stopSelf();
    }

    @Override
    public void onDestroy() {
        if (live == this) {
            live = null;
        }
        if (wakeLock != null) {
            try {
                wakeLock.release();
            } catch (RuntimeException ignored) {
                // timed out already
            }
            wakeLock = null;
        }
        try {
            if (Build.VERSION.SDK_INT >= 24) {
                stopForeground(STOP_FOREGROUND_REMOVE);
            } else {
                stopForeground(true);
            }
        } catch (RuntimeException ignored) {
            // never got to the foreground
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    /** Ongoing, silent, low importance; gone when the service stops. No "done" notice follows. */
    private static Notification notice(Context context) {
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26) {
            manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                    HostRes.string(context, "boot_notice_channel"), NotificationManager.IMPORTANCE_LOW));
        }
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(context, CHANNEL)
                : new Notification.Builder(context);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT
                | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
        return builder
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(HostRes.string(context, "app_name"))
                .setContentText(noticeText)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setProgress(Math.max(1, noticeTotal), Math.max(0, noticeDone), noticeTotal <= 0)
                .setContentIntent(PendingIntent.getActivity(context, NOTICE,
                        BootActivity.launcherIntent(context), flags))
                .build();
    }
}
