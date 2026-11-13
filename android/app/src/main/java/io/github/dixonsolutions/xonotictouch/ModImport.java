package io.github.dixonsolutions.xonotictouch;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.Charset;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * "Import mod" in Settings → Mobile → Mods: picks a .zip with the system file
 * picker and unpacks it into the engine's Lua mods folder ({@code <userdir>/mods/<name>/},
 * docs/MODDING.md). The imported mod starts off; the player turns it on in the
 * same tab.
 *
 * The menu is QuakeC and cannot call Java, so it asks through a file, the same
 * handshake as {@link UpdateBridge}: the menu writes
 * {@code touch/mod-import-request.txt}, this class deletes it, runs the picker and
 * writes a one-line {@code touch/mod-import-status.txt}, whose presence is also
 * what makes the menu show the button at all. The format lives in
 * {@code qcsrc/menu/xonotic/dialog_touch_mods.qh}.
 *
 * The archive is untrusted: every entry name is checked (no absolute paths, no
 * {@code ..}, no drive letters or backslashes, nothing that resolves outside the
 * target), with limits on file count and sizes, and nothing is written until the
 * whole archive has been read and found to hold an {@code init.lua}.
 */
final class ModImport {

    private static final String TAG = "XonoticTouch";
    static final int REQUEST_CODE = 0x4d4f;

    private static final int MAX_FILES = 256;
    private static final long MAX_FILE = 1024 * 1024;       // the engine's own limit for one Lua file
    private static final long MAX_TOTAL = 8 * 1024 * 1024;

    private final Activity activity;
    private final File touchDir;
    private final File modsDir;
    private final File requestFile;
    private final File statusFile;
    private Thread poller;

    ModImport(Activity activity, File baseDir) {
        this.activity = activity;
        this.touchDir = new File(GameData.engineWriteDir(baseDir), "touch");
        this.modsDir = new File(GameData.userDir(baseDir), "mods");
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
        publish("Importing...");
        new Thread(() -> publish(importZip(uri)), "xonotic-mod-import-unzip").start();
        return true;
    }

    private String importZip(Uri uri) {
        Map<String, byte[]> files = new LinkedHashMap<>();
        long total = 0;
        try (InputStream raw = activity.getContentResolver().openInputStream(uri)) {
            if (raw == null) {
                return "Could not open the file";
            }
            // CP437 fallback names never fail to decode; bad names are then refused below
            ZipInputStream zip = new ZipInputStream(raw, Charset.forName("UTF-8"));
            ZipEntry entry;
            byte[] buf = new byte[16384];
            while ((entry = zip.getNextEntry()) != null) {
                if (entry.isDirectory()) {
                    continue;
                }
                String name = entry.getName();
                if (name.startsWith("__MACOSX/") || name.endsWith("/.DS_Store")) {
                    continue;
                }
                String bad = checkPath(name);
                if (bad != null) {
                    return "Refused: " + bad;
                }
                if (files.size() >= MAX_FILES) {
                    return "Refused: more than " + MAX_FILES + " files";
                }
                ByteArrayOutputStream out = new ByteArrayOutputStream();
                int n;
                long size = 0;
                while ((n = zip.read(buf)) > 0) {
                    size += n;
                    total += n;
                    if (size > MAX_FILE) {
                        return "Refused: " + name + " is over 1 MB";
                    }
                    if (total > MAX_TOTAL) {
                        return "Refused: the mod is over 8 MB";
                    }
                    out.write(buf, 0, n);
                }
                if (files.put(name, out.toByteArray()) != null) {
                    return "Refused: " + name + " is in the zip twice";
                }
            }
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "Mod import failed", e);
            return "Not a readable .zip";
        }

        // init.lua at the top, or in one folder that holds everything
        String prefix = "";
        String modName = displayName(uri);
        if (!files.containsKey("init.lua")) {
            String top = null;
            for (String name : files.keySet()) {
                int slash = name.indexOf('/');
                String first = slash < 0 ? "" : name.substring(0, slash);
                if (top == null) {
                    top = first;
                } else if (!top.equals(first)) {
                    top = "";
                }
            }
            if (top == null || top.isEmpty() || !files.containsKey(top + "/init.lua")) {
                return "Refused: no init.lua in the zip";
            }
            prefix = top + "/";
            modName = top;
        }
        String name = sanitize(modName);
        if (name.isEmpty()) {
            return "Refused: no usable mod name";
        }

        File tmp = new File(modsDir, ".import-" + name);
        File target = new File(modsDir, name);
        try {
            deleteTree(tmp);
            String tmpRoot = tmp.getCanonicalPath() + File.separator;
            for (Map.Entry<String, byte[]> e : files.entrySet()) {
                File f = new File(tmp, e.getKey().substring(prefix.length()));
                if (!f.getCanonicalPath().startsWith(tmpRoot)) {
                    deleteTree(tmp);
                    return "Refused: " + e.getKey();
                }
                File parent = f.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                    throw new IOException("mkdirs " + parent);
                }
                try (OutputStream out = new FileOutputStream(f)) {
                    out.write(e.getValue());
                }
            }
            // an update keeps what the mod saved (its data/ folder)
            File oldData = new File(target, "data");
            if (oldData.isDirectory() && !new File(tmp, "data").exists()) {
                if (!oldData.renameTo(new File(tmp, "data"))) {
                    Log.w(TAG, "Could not keep " + oldData);
                }
            }
            deleteTree(target);
            if (!tmp.renameTo(target)) {
                throw new IOException("rename " + tmp + " -> " + target);
            }
        } catch (IOException e) {
            Log.w(TAG, "Mod import failed", e);
            deleteTree(tmp);
            return "Could not write the mod";
        }
        Log.i(TAG, "Imported mod " + name + " (" + files.size() + " files)");
        return "Imported " + name + ": switch it on below";
    }

    /** Why this zip entry name is unsafe, or null. */
    static String checkPath(String name) {
        if (name.isEmpty() || name.startsWith("/") || name.contains("\\") || name.contains(":")
                || name.indexOf('\0') >= 0) {
            return name;
        }
        for (String part : name.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) {
                return name;
            }
        }
        return null;
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

    private static void deleteTree(File f) {
        File[] children = f.listFiles();
        if (children != null) {
            for (File c : children) {
                deleteTree(c);
            }
        }
        if (f.exists() && !f.delete()) {
            Log.w(TAG, "Could not delete " + f);
        }
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
