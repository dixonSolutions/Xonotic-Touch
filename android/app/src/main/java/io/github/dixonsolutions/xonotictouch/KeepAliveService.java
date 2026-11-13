package io.github.dixonsolutions.xonotictouch;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

/**
 * Keeps the process running while the game is in the background and has work
 * that must not stop: connected to a server, joining one (with its downloads),
 * or the first-launch game data download.
 *
 * Android freezes a cached background app's process within seconds (the cached
 * apps freezer, on by default since Android 14) and kills it under memory
 * pressure; Doze cuts its network. A foreground service, with its notification,
 * is what keeps the process and its sockets alive, and a partial wake lock
 * keeps the CPU running with the screen off, so a server does not time the
 * player out. It runs only while there is such work and stops as soon as there
 * is none. Nothing else here: the engine itself keeps its main loop going in
 * the background (touch_android.c) and says what it is doing in
 * touch_netstate, which {@link XonoticActivity} reads; the download is
 * {@link BootActivity}'s.
 */
public final class KeepAliveService extends Service {

    private static final String TAG = "XonoticTouch";

    static final String KIND_CONNECTION = "connection";
    static final String KIND_DOWNLOAD = "download";

    private static final String EXTRA_KIND = "kind";
    private static final String EXTRA_TEXT = "text";
    private static final String CHANNEL_CONNECTION = "connection";
    /** The boot screen's own progress notification: the service takes it over. */
    static final int NOTICE_DOWNLOAD = 1;
    private static final int NOTICE_CONNECTION = 3;

    /** What the running service shows, so an unchanged state is not re-posted. */
    private static volatile String running;
    /** The engine is connected, joining or downloading (touch_netstate). */
    static volatile boolean engineBusy;

    private PowerManager.WakeLock wakeLock;

    /**
     * Start the service, or change its text. Returns false when Android will
     * not let it start (from the background on Android 12+): the work then
     * carries on only as long as the process does.
     */
    static boolean show(Context context, String kind, String text) {
        String key = kind + "\n" + text;
        String now = running;
        if (key.equals(now)) {
            return true;
        }
        if (now != null && now.startsWith(kind + "\n")) {
            // Already in the foreground: only the text changes. Starting the
            // service again would be refused from the background (Android 12+).
            boolean download = KIND_DOWNLOAD.equals(kind);
            NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
            manager.notify(download ? NOTICE_DOWNLOAD : NOTICE_CONNECTION, notice(context, download, text));
            running = key;
            return true;
        }
        Intent intent = new Intent(context, KeepAliveService.class)
                .putExtra(EXTRA_KIND, kind)
                .putExtra(EXTRA_TEXT, text);
        try {
            if (Build.VERSION.SDK_INT >= 26) {
                context.startForegroundService(intent);
            } else {
                context.startService(intent);
            }
            running = key;
            return true;
        } catch (RuntimeException e) {
            // ForegroundServiceStartNotAllowedException (31+) or
            // IllegalStateException: not allowed from the background.
            Log.w(TAG, "Could not keep the game alive in the background", e);
            return false;
        }
    }

    static void stop(Context context) {
        if (running == null) {
            return;
        }
        running = null;
        context.stopService(new Intent(context, KeepAliveService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String kind = intent != null ? intent.getStringExtra(EXTRA_KIND) : null;
        String text = intent != null ? intent.getStringExtra(EXTRA_TEXT) : null;
        if (kind == null) {
            // Restarted by the system after the process died: the work is gone.
            stopSelf();
            return START_NOT_STICKY;
        }
        boolean download = KIND_DOWNLOAD.equals(kind);
        Notification notice = notice(this, download, text);
        int id = download ? NOTICE_DOWNLOAD : NOTICE_CONNECTION;
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(id, notice, download
                        ? ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                        : ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
            } else if (Build.VERSION.SDK_INT >= 29) {
                startForeground(id, notice, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
            } else {
                startForeground(id, notice);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Foreground service refused", e);
            running = null;
            stopSelf();
            return START_NOT_STICKY;
        }
        if (wakeLock == null) {
            PowerManager power = (PowerManager) getSystemService(Context.POWER_SERVICE);
            wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "XonoticTouch:keepalive");
            wakeLock.setReferenceCounted(false);
            wakeLock.acquire();
        }
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        if (wakeLock != null) {
            wakeLock.release();
            wakeLock = null;
        }
        if (Build.VERSION.SDK_INT >= 24) {
            stopForeground(STOP_FOREGROUND_REMOVE);
        } else {
            stopForeground(true);
        }
        super.onDestroy();
    }

    @Override
    public void onTaskRemoved(Intent rootIntent) {
        // Swiped away from the recent apps: the player is done with the game.
        running = null;
        stopSelf();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private static Notification notice(Context context, boolean download, String text) {
        NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
        String channel = download ? "downloads" : CHANNEL_CONNECTION;
        if (Build.VERSION.SDK_INT >= 26) {
            manager.createNotificationChannel(new NotificationChannel(channel,
                    HostRes.string(context, download ? "boot_notice_channel" : "keepalive_channel"),
                    NotificationManager.IMPORTANCE_LOW));
        }
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(context, channel)
                : new Notification.Builder(context);
        if (Build.VERSION.SDK_INT >= 31) {
            // Shown at once rather than after Android 12's ten-second grace.
            builder.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE);
        }
        Intent open = new Intent(context, download ? BootActivity.class : XonoticActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT
                | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
        return builder
                .setSmallIcon(download ? android.R.drawable.stat_sys_download : android.R.drawable.stat_notify_sync)
                .setContentTitle(HostRes.string(context, download ? "keepalive_download_title" : "keepalive_connection_title"))
                .setContentText(text != null ? text : "")
                .setOngoing(true)
                .setContentIntent(PendingIntent.getActivity(context, download ? 1 : 3, open, flags))
                .build();
    }
}
