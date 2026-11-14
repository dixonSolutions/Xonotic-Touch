package io.github.dixonsolutions.godnotic;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

/**
 * The system notification for an AndySU update download: a progress bar with size, speed and
 * time left, on the "Game downloads" channel the game data download already uses. Not a
 * foreground service: the host does the downloading on its own thread, and this only shows it.
 */
final class UpdateNotice {
    private static final String TAG = "Godnotic";
    /** 1 boot download, 3 connection, 4 ready to play (KeepAliveService). */
    private static final int NOTICE_UPDATE = 5;
    private static final String CHANNEL = "downloads";

    private UpdateNotice() {}

    static void show(Context context, String version, int percent, boolean known, String line) {
        try {
            NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
            if (manager == null) return;
            if (Build.VERSION.SDK_INT >= 26) {
                manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                        HostRes.string(context, "boot_notice_channel"), NotificationManager.IMPORTANCE_LOW));
            }
            Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                    ? new Notification.Builder(context, CHANNEL)
                    : new Notification.Builder(context);
            Intent open = new Intent(context, XonoticActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT
                    | (Build.VERSION.SDK_INT >= 23 ? PendingIntent.FLAG_IMMUTABLE : 0);
            manager.notify(NOTICE_UPDATE, builder
                    .setSmallIcon(android.R.drawable.stat_sys_download)
                    .setContentTitle(HostRes.string(context, "update_notice_title", version))
                    .setContentText(line)
                    .setProgress(100, Math.max(0, Math.min(100, percent)), !known)
                    .setOnlyAlertOnce(true)
                    .setOngoing(true)
                    .setContentIntent(PendingIntent.getActivity(context, NOTICE_UPDATE, open, flags))
                    .build());
        } catch (RuntimeException e) {
            Log.w(TAG, "Could not post the update download notification", e);
        }
    }

    static void cancel(Context context) {
        try {
            NotificationManager manager = (NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
            if (manager != null) manager.cancel(NOTICE_UPDATE);
        } catch (RuntimeException ignored) {
            // nothing to take down
        }
    }
}
