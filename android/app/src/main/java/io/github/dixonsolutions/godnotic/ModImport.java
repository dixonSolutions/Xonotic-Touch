package io.github.dixonsolutions.godnotic;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.util.Locale;

/**
 * "Pick file" in Settings → Mobile → Mods → Install: picks a .zip with the system
 * file picker and hands it to the engine's installer (docs/PLUGIN_STANDARD.md),
 * which checks it, shows what it asks for, and installs it switched off with
 * nothing granted.
 *
 * The menu is QuakeC and cannot call Java, so it asks through a file, the same
 * handshake as {@link UpdateBridge}: the menu writes
 * {@code touch/mod-import-request.txt}, this class deletes it, runs the picker and
 * writes a one-line {@code touch/mod-import-status.txt}, whose presence is also
 * what makes the menu show the button at all. The format lives in
 * {@code qcsrc/menu/xonotic/dialog_touch_mods.qh}.
 *
 * The archive is untrusted and this class does not open it: it only copies the
 * bytes (at most 8 MB) to {@code <userdir>/luastate/incoming/}, where no mod can
 * reach, and the engine does every check (names, sizes, zip bombs, symlinks).
 */
final class ModImport {

    private static final String TAG = "Godnotic";
    static final int REQUEST_CODE = 0x4d4f;

    private static final long MAX_TOTAL = 8 * 1024 * 1024; // the engine's limit for a package

    private final Activity activity;
    private final File touchDir;
    private final File incomingDir;
    private final File requestFile;
    private final File statusFile;
    private Thread poller;

    ModImport(Activity activity, File baseDir) {
        this.activity = activity;
        this.touchDir = new File(GameData.engineWriteDir(baseDir), "touch");
        this.incomingDir = new File(new File(GameData.userDir(baseDir), "luastate"), "incoming");
        this.requestFile = new File(touchDir, "mod-import-request.txt");
        this.statusFile = new File(touchDir, "mod-import-status.txt");
    }

    void start() {
        publish("");
        poller = new Thread(() -> {
            while (!Thread.currentThread().isInterrupted()) {
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
                if (requestFile.isFile()) {
                    // consumed before acting: a request that fails is not retried forever
                    if (!requestFile.delete()) {
                        Log.w(TAG, "Could not delete " + requestFile);
                    }
                    activity.runOnUiThread(this::pick);
                }
            }
        }, "xonotic-mod-import");
        poller.setDaemon(true);
        poller.start();
    }

    void stop() {
        if (poller != null) {
            poller.interrupt();
            poller = null;
        }
    }

    private void pick() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
            "application/zip", "application/x-zip-compressed", "application/octet-stream"});
        try {
            activity.startActivityForResult(intent, REQUEST_CODE);
            publish("Pick the mod's .zip");
        } catch (ActivityNotFoundException e) {
            publish("No file picker on this device");
        }
    }

    /** From the activity's onActivityResult: true when the result was ours. */
    boolean onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_CODE) {
            return false;
        }
        final Uri uri = data != null ? data.getData() : null;
        if (resultCode != Activity.RESULT_OK || uri == null) {
            publish("Import cancelled");
            return true;
        }
        publish("Reading the file...");
        new Thread(() -> publish(importZip(uri)), "xonotic-mod-import-unzip").start();
        return true;
    }

    /**
     * Copies the picked file, unchanged, to {@code <userdir>/luastate/incoming/<name>.zip}
     * and tells the menu "picked <time> <path>"; the menu then runs
     * {@code lua_mod_install <path>}. Checking and unpacking are the engine's
     * (touch_lua_pkg.c), the same code path as a file, a link or the console, with
     * the same review before anything is installed (docs/PLUGIN_STANDARD.md). Only
     * the size is limited here, so a huge file is not copied at all.
     */
    private String importZip(Uri uri) {
        String name = sanitize(displayName(uri));
        if (name.isEmpty()) {
            name = "imported_mod";
        }
        File dir = incomingDir;
        File tmp = new File(dir, name + ".zip.part");
        File target = new File(dir, name + ".zip");
        try (InputStream raw = activity.getContentResolver().openInputStream(uri)) {
            if (raw == null) {
                return "Could not open the file";
            }
            // only the latest pick waits here
            File[] old = dir.listFiles();
            if (old != null) {
                for (File f : old) {
                    if (!f.delete()) {
                        Log.w(TAG, "Could not delete " + f);
                    }
                }
            }
            if (!dir.isDirectory() && !dir.mkdirs()) {
                return "Could not write the file";
            }
            long total = 0;
            byte[] buf = new byte[16384];
            try (OutputStream out = new FileOutputStream(tmp)) {
                int n;
                while ((n = raw.read(buf)) > 0) {
                    total += n;
                    if (total > MAX_TOTAL) {
                        out.close();
                        tmp.delete();
                        return "Refused: the file is over 8 MB";
                    }
                    out.write(buf, 0, n);
                }
            }
            if (!tmp.renameTo(target)) {
                throw new IOException("rename " + tmp + " -> " + target);
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Mod import failed", e);
            tmp.delete();
            return "Could not read the file";
        }
        Log.i(TAG, "Picked mod package " + target);
        return "picked " + System.currentTimeMillis() + " " + target.getAbsolutePath();
    }

    /** The engine only loads folders named a-z, 0-9 and _, up to 32 long. */
    static String sanitize(String raw) {
        String s = raw.toLowerCase(Locale.ROOT);
        if (s.endsWith(".zip")) {
            s = s.substring(0, s.length() - 4);
        }
        StringBuilder b = new StringBuilder();
        for (char c : s.toCharArray()) {
            if (b.length() >= 32) {
                break;
            }
            b.append((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '_');
        }
        return b.toString().replaceAll("^_+|_+$", "");
    }

    private String displayName(Uri uri) {
        try (Cursor c = activity.getContentResolver().query(uri,
                new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String s = c.getString(0);
                if (s != null) {
                    return s;
                }
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "No display name for " + uri, e);
        }
        return "imported_mod";
    }

    /** One line for the menu; an empty line means "ready". */
    private void publish(String line) {
        try {
            if (!touchDir.isDirectory() && !touchDir.mkdirs()) {
                return;
            }
            File tmp = new File(touchDir, "mod-import-status.txt.tmp");
            try (OutputStream out = new FileOutputStream(tmp)) {
                out.write((line + "\n").getBytes(Charset.forName("UTF-8")));
            }
            if (!tmp.renameTo(statusFile)) {
                Log.w(TAG, "Could not publish mod import status");
            }
        } catch (IOException e) {
            Log.w(TAG, "Could not publish mod import status", e);
        }
    }
}
