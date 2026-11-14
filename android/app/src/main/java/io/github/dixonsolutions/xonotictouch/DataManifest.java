package io.github.dixonsolutions.xonotictouch;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Pattern;

/**
 * The split data set's manifest, {@code godnotic-data-<profile>.json}
 * (docs/DATA_FETCH.md, "The manifest"): which packs make up the set, where each
 * one's bytes are in the release archive, which of them the first match needs
 * and which packs each map needs.
 *
 * {@link #parse} accepts a manifest only when everything the contract lists
 * holds. Anything else is {@link Invalid}, and the caller then does what it
 * did before there was a manifest (the single archive). The checks are here,
 * not spread over the code that fetches: a name that got past them is used as
 * a file name in the data directory, and an offset as a byte range.
 *
 * No Android class is used here, nor in the other Data* classes but
 * DataService: the JVM tests in src/test run them against a local range
 * server. The JSON reader is its own for the same reason (org.json is only in
 * the Android runtime).
 */
final class DataManifest {

    static final int FORMAT = 1;

    private static final Pattern INSTALL_AS = Pattern.compile("^godnotic-[A-Za-z0-9._-]+\\.pk3$");
    private static final Pattern CRC32 = Pattern.compile("^[0-9a-fA-F]{8}$");
    private static final Pattern SHA256 = Pattern.compile("^[0-9a-fA-F]{64}$");
    /** Names go into line-based files (the status file, the request file): no blanks, no controls. */
    private static final Pattern TOKEN = Pattern.compile("^[\\x21-\\x7e]+$");

    static final class Invalid extends Exception {
        Invalid(String message) {
            super(message);
        }
    }

    /** One pk3 of the set: a byte range of the archive and what it has to hash to. */
    static final class Pack {
        final String name;
        final String installAs;
        final long size;
        final long crc32;
        /** Lower case hex. */
        final String sha256;
        final long priority;
        final long offset;
        final List<String> depends;
        /** Position in the manifest's pack list: the tie-break for equal priorities. */
        final int index;

        Pack(String name, String installAs, long size, long crc32, String sha256, long priority,
             long offset, List<String> depends, int index) {
            this.name = name;
            this.installAs = installAs;
            this.size = size;
            this.crc32 = crc32;
            this.sha256 = sha256;
            this.priority = priority;
            this.offset = offset;
            this.depends = depends;
            this.index = index;
        }

        @Override
        public String toString() {
            return name;
        }
    }

    final String set;
    final String archiveUrl;
    final long archiveSize;
    /** In the manifest's order. */
    final List<Pack> packs;
    final List<Pack> firstPlay;
    /** Map name to the packs it needs, in the manifest's order. */
    final Map<String, List<Pack>> needs;
    /** Map name to its own pack (the one with the bsp); absent when the manifest names none. */
    final Map<String, Pack> ownPack;
    /** The text this was parsed from, kept as {@code .godnotic-data.json}. */
    final String text;
    private final Map<String, Pack> byName;

    private DataManifest(String set, String archiveUrl, long archiveSize, List<Pack> packs,
                         Map<String, Pack> byName, List<Pack> firstPlay, Map<String, List<Pack>> needs,
                         Map<String, Pack> ownPack, String text) {
        this.set = set;
        this.archiveUrl = archiveUrl;
        this.archiveSize = archiveSize;
        this.packs = packs;
        this.byName = byName;
        this.firstPlay = firstPlay;
        this.needs = needs;
        this.ownPack = ownPack;
        this.text = text;
    }

    Pack pack(String name) {
        return byName.get(name);
    }

    long totalBytes() {
        long sum = 0;
        for (Pack p : packs) {
            sum += p.size;
        }
        return sum;
    }

    /** The packs sorted by {@code priority} (lower first), manifest order among equals. */
    List<Pack> byPriority() {
        List<Pack> sorted = new ArrayList<>(packs);
        Collections.sort(sorted, (a, b) -> a.priority != b.priority
                ? Long.compare(a.priority, b.priority) : Integer.compare(a.index, b.index));
        return sorted;
    }

    // ----------------------------------------------------------------- parse

    static DataManifest parse(String text) throws Invalid {
        Object root;
        try {
            root = new Json(text).document();
        } catch (RuntimeException e) {
            throw new Invalid("not JSON: " + e.getMessage());
        }
        Map<String, Object> top = object(root, "the manifest");
        Object format = top.get("format");
        if (!(format instanceof Long) || (Long) format != FORMAT) {
            throw new Invalid("format " + format + " is not " + FORMAT);
        }
        String set = string(top.get("set"), "set");
        if (!TOKEN.matcher(set).matches()) {
            throw new Invalid("set is empty or has blanks");
        }
        Map<String, Object> archive = object(top.get("archive"), "archive");
        String url = string(archive.get("url"), "archive.url");
        if (!(url.startsWith("https://") || url.startsWith("http://")) || url.length() < 12
                || !TOKEN.matcher(url).matches()) {
            throw new Invalid("archive.url is not an http(s) URL");
        }
        long archiveSize = number(archive.get("size"), "archive.size");
        if (archiveSize <= 0) {
            throw new Invalid("archive.size is not positive");
        }

        List<Pack> packs = new ArrayList<>();
        Map<String, Pack> byName = new HashMap<>();
        Map<String, Pack> byFile = new HashMap<>();
        List<Object> list = array(top.get("packs"), "packs");
        for (Object item : list) {
            Map<String, Object> o = object(item, "a pack");
            String name = string(o.get("name"), "pack name");
            if (!TOKEN.matcher(name).matches()) {
                throw new Invalid("pack name is empty or has blanks");
            }
            String installAs = string(o.get("install_as"), name + ".install_as");
            if (!INSTALL_AS.matcher(installAs).matches()) {
                throw new Invalid(name + ".install_as is not a godnotic-*.pk3 name");
            }
            long size = number(o.get("size"), name + ".size");
            if (size <= 0) {
                throw new Invalid(name + ".size is not positive");
            }
            String crc = string(o.get("crc32"), name + ".crc32");
            if (!CRC32.matcher(crc).matches()) {
                throw new Invalid(name + ".crc32 is not 8 hex digits");
            }
            String sha = string(o.get("sha256"), name + ".sha256");
            if (!SHA256.matcher(sha).matches()) {
                throw new Invalid(name + ".sha256 is not 64 hex digits");
            }
            long priority = number(o.get("priority"), name + ".priority");
            long offset = number(o.get("offset"), name + ".offset");
            // Written so that a huge offset cannot overflow its way past the check.
            if (offset < 0 || offset > archiveSize || size > archiveSize - offset) {
                throw new Invalid(name + " does not lie inside the archive");
            }
            List<String> depends = new ArrayList<>();
            if (o.get("depends") != null) {
                for (Object d : array(o.get("depends"), name + ".depends")) {
                    depends.add(string(d, name + ".depends"));
                }
            }
            Pack pack = new Pack(name, installAs, size, Long.parseLong(crc, 16),
                    sha.toLowerCase(java.util.Locale.ROOT), priority, offset,
                    Collections.unmodifiableList(depends), packs.size());
            // Two packs under one name, or two installed as one file, would
            // have the second rename over the first: never a set to accept.
            if (byName.put(name, pack) != null) {
                throw new Invalid("two packs are named " + name);
            }
            if (byFile.put(installAs, pack) != null) {
                throw new Invalid("two packs install as " + installAs);
            }
            packs.add(pack);
        }
        if (packs.isEmpty()) {
            throw new Invalid("no packs");
        }
        for (Pack p : packs) {
            for (String d : p.depends) {
                if (!byName.containsKey(d)) {
                    throw new Invalid(p.name + " depends on " + d + ", which is not a pack");
                }
            }
        }

        List<Pack> firstPlay = new ArrayList<>();
        for (Object n : array(top.get("first_play"), "first_play")) {
            Pack p = byName.get(string(n, "first_play"));
            if (p == null) {
                throw new Invalid("first_play names " + n + ", which is not a pack");
            }
            if (!firstPlay.contains(p)) {
                firstPlay.add(p);
            }
        }
        if (firstPlay.isEmpty()) {
            // "The game may start when every one of them is installed": with
            // none listed that would be an empty data directory.
            throw new Invalid("first_play is empty");
        }

        Map<String, List<Pack>> needs = new LinkedHashMap<>();
        Map<String, Pack> ownPack = new HashMap<>();
        if (top.get("maps") != null) {
            for (Map.Entry<String, Object> e : object(top.get("maps"), "maps").entrySet()) {
                String map = e.getKey();
                if (!TOKEN.matcher(map).matches()) {
                    throw new Invalid("a map name is empty or has blanks");
                }
                Map<String, Object> m = object(e.getValue(), "maps." + map);
                List<Pack> mapNeeds = new ArrayList<>();
                for (Object n : array(m.get("needs"), "maps." + map + ".needs")) {
                    Pack p = byName.get(string(n, "maps." + map + ".needs"));
                    if (p == null) {
                        throw new Invalid("map " + map + " needs " + n + ", which is not a pack");
                    }
                    if (!mapNeeds.contains(p)) {
                        mapNeeds.add(p);
                    }
                }
                if (m.get("pack") != null) {
                    Pack own = byName.get(string(m.get("pack"), "maps." + map + ".pack"));
                    if (own == null) {
                        throw new Invalid("map " + map + " names a pack that is not one");
                    }
                    ownPack.put(map, own);
                }
                needs.put(map, Collections.unmodifiableList(mapNeeds));
            }
        }
        return new DataManifest(set, url, archiveSize, Collections.unmodifiableList(packs), byName,
                Collections.unmodifiableList(firstPlay), needs, ownPack, text);
    }

    @SuppressWarnings("unchecked")
    private static Map<String, Object> object(Object value, String what) throws Invalid {
        if (!(value instanceof Map)) {
            throw new Invalid(what + " is not an object");
        }
        return (Map<String, Object>) value;
    }

    @SuppressWarnings("unchecked")
    private static List<Object> array(Object value, String what) throws Invalid {
        if (!(value instanceof List)) {
            throw new Invalid(what + " is not a list");
        }
        return (List<Object>) value;
    }

    private static String string(Object value, String what) throws Invalid {
        if (!(value instanceof String)) {
            throw new Invalid(what + " is not a string");
        }
        return (String) value;
    }

    /** A whole number; 12.0 or 1e3 is not one a size or an offset is written as. */
    private static long number(Object value, String what) throws Invalid {
        if (!(value instanceof Long)) {
            throw new Invalid(what + " is not a whole number");
        }
        return (Long) value;
    }

    // ------------------------------------------------------------------ JSON

    /**
     * Just enough JSON for the manifest: objects as LinkedHashMap, arrays as
     * ArrayList, strings, whole numbers as Long, other numbers as Double,
     * true/false/null. Errors are RuntimeExceptions, turned into Invalid above.
     */
    private static final class Json {
        private final String s;
        private int p;
        private int depth;

        Json(String s) {
            this.s = s;
        }

        Object document() {
            Object value = value();
            blanks();
            if (p != s.length()) {
                throw new IllegalArgumentException("text after the document at " + p);
            }
            return value;
        }

        private void blanks() {
            while (p < s.length()) {
                char c = s.charAt(p);
                if (c != ' ' && c != '\n' && c != '\r' && c != '\t' && c != '﻿') {
                    break;
                }
                p++;
            }
        }

        private char peek() {
            blanks();
            if (p >= s.length()) {
                throw new IllegalArgumentException("ends early");
            }
            return s.charAt(p);
        }

        private Object value() {
            char c = peek();
            if (c == '{' || c == '[') {
                if (++depth > 32) {
                    throw new IllegalArgumentException("nested too deep");
                }
                Object v = c == '{' ? object() : array();
                depth--;
                return v;
            }
            if (c == '"') {
                return string();
            }
            if (s.startsWith("true", p)) {
                p += 4;
                return Boolean.TRUE;
            }
            if (s.startsWith("false", p)) {
                p += 5;
                return Boolean.FALSE;
            }
            if (s.startsWith("null", p)) {
                p += 4;
                return null;
            }
            return number();
        }

        private Map<String, Object> object() {
            Map<String, Object> map = new LinkedHashMap<>();
            p++;
            if (peek() == '}') {
                p++;
                return map;
            }
            while (true) {
                if (peek() != '"') {
                    throw new IllegalArgumentException("a key is expected at " + p);
                }
                String key = string();
                if (peek() != ':') {
                    throw new IllegalArgumentException("':' is expected at " + p);
                }
                p++;
                map.put(key, value());
                char c = peek();
                p++;
                if (c == '}') {
                    return map;
                }
                if (c != ',') {
                    throw new IllegalArgumentException("',' or '}' is expected at " + (p - 1));
                }
            }
        }

        private List<Object> array() {
            List<Object> list = new ArrayList<>();
            p++;
            if (peek() == ']') {
                p++;
                return list;
            }
            while (true) {
                list.add(value());
                char c = peek();
                p++;
                if (c == ']') {
                    return list;
                }
                if (c != ',') {
                    throw new IllegalArgumentException("',' or ']' is expected at " + (p - 1));
                }
            }
        }

        private String string() {
            StringBuilder out = new StringBuilder();
            p++;
            while (true) {
                if (p >= s.length()) {
                    throw new IllegalArgumentException("a string does not end");
                }
                char c = s.charAt(p++);
                if (c == '"') {
                    return out.toString();
                }
                if (c != '\\') {
                    out.append(c);
                    continue;
                }
                if (p >= s.length()) {
                    throw new IllegalArgumentException("a string does not end");
                }
                char e = s.charAt(p++);
                switch (e) {
                    case 'n': out.append('\n'); break;
                    case 't': out.append('\t'); break;
                    case 'r': out.append('\r'); break;
                    case 'b': out.append('\b'); break;
                    case 'f': out.append('\f'); break;
                    case 'u':
                        if (p + 4 > s.length()) {
                            throw new IllegalArgumentException("a \\u escape is cut short");
                        }
                        out.append((char) Integer.parseInt(s.substring(p, p + 4), 16));
                        p += 4;
                        break;
                    case '"': case '\\': case '/':
                        out.append(e);
                        break;
                    default:
                        throw new IllegalArgumentException("unknown escape \\" + e);
                }
            }
        }

        private Object number() {
            int start = p;
            boolean whole = true;
            while (p < s.length()) {
                char c = s.charAt(p);
                if (c == '.' || c == 'e' || c == 'E') {
                    whole = false;
                } else if (!(c >= '0' && c <= '9') && c != '-' && c != '+') {
                    break;
                }
                p++;
            }
            if (p == start) {
                throw new IllegalArgumentException("unexpected '" + s.charAt(p) + "' at " + p);
            }
            String text = s.substring(start, p);
            return whole ? (Object) Long.valueOf(Long.parseLong(text)) : (Object) Double.valueOf(text);
        }
    }
}
