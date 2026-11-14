package io.github.dixonsolutions.godnotic;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * The two files the download and the running game talk through
 * (docs/DATA_FETCH.md, "Talking to the running game"), both in
 * {@code <engine write dir>/touch/}, where {@code update-status.txt} already
 * lives: {@code data-status.txt} is written here and read by the engine
 * (touch_datafetch.c) about once a second; {@code data-request.txt} is written
 * by the engine ({@code touch_data_want}) and only read here.
 */
final class DataStatus {

    static final String STATUS_FILE = "data-status.txt";
    static final String REQUEST_FILE = "data-request.txt";
    static final int FORMAT = 1;

    static final String FETCHING = "fetching";
    static final String PAUSED = "paused";
    static final String WAITING = "waiting";
    static final String ERROR = "error";
    static final String COMPLETE = "complete";
    static final String LEGACY = "legacy";

    private DataStatus() {}

    /** The status file's text. {@code want} null: no want lines. {@code message} only with the error state. */
    static String text(String set, String state, long doneBytes, long totalBytes, String want,
                       long wantDoneBytes, long wantTotalBytes, String message, List<String> landed) {
        StringBuilder t = new StringBuilder();
        t.append("format ").append(FORMAT).append('\n');
        t.append("set ").append(set).append('\n');
        t.append("state ").append(state).append('\n');
        if (ERROR.equals(state) && message != null) {
            t.append("message ").append(oneLine(message)).append('\n');
        }
        t.append("done_kb ").append(doneBytes / 1024).append('\n');
        t.append("total_kb ").append(totalBytes / 1024).append('\n');
        if (want != null) {
            t.append("want ").append(want).append('\n');
            t.append("want_done_kb ").append(wantDoneBytes / 1024).append('\n');
            t.append("want_total_kb ").append(wantTotalBytes / 1024).append('\n');
        }
        for (String pack : landed) {
            t.append("landed ").append(pack).append('\n');
        }
        return t.toString();
    }

    private static String oneLine(String s) {
        String flat = s.replace('\n', ' ').replace('\r', ' ').trim();
        return flat.length() > 200 ? flat.substring(0, 200) : flat;
    }

    /** One request: the map and the number that grows with every request. */
    static final class Request {
        final String map;
        final long serial;

        Request(String map, long serial) {
            this.map = map;
            this.serial = serial;
        }
    }

    /** "want boil 3" -> the request; null for anything else. */
    static Request parseRequest(String text) {
        if (text == null) {
            return null;
        }
        for (String raw : text.split("\n")) {
            String[] w = raw.trim().split("\\s+");
            if (w.length == 3 && w[0].equals("want") && !w[1].isEmpty()) {
                try {
                    return new Request(w[1], Long.parseLong(w[2]));
                } catch (NumberFormatException e) {
                    return null;
                }
            }
        }
        return null;
    }

    /**
     * The request file's request, when it was written after {@code notBefore}
     * (the host's own start: one left by an earlier session is not this
     * one's). Never deletes the file; the engine owns it.
     */
    static Request readRequest(File file, long notBefore) {
        if (!file.isFile() || file.lastModified() < notBefore) {
            return null;
        }
        try {
            return parseRequest(readText(file, 4096));
        } catch (IOException e) {
            return null; // mid-write: the next poll reads it
        }
    }

    static String readText(File f, int limit) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[16 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                if (out.size() > limit) {
                    throw new IOException(f + " is larger than expected");
                }
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    /** Written under a temporary name and renamed: a reader sees the old text or the new, never half. */
    static void writeAtomic(File f, String text) throws IOException {
        writeAtomic(f, text, true);
    }

    /** @param sync flush to storage before the rename (not for the status file, rewritten every second) */
    static void writeAtomic(File f, String text, boolean sync) throws IOException {
        File parent = f.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs() && !parent.isDirectory()) {
            throw new IOException("Cannot create " + parent);
        }
        File tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            if (sync) {
                out.getFD().sync();
            }
        }
        if (!tmp.renameTo(f)) {
            throw new IOException("Cannot write " + f);
        }
    }
}
