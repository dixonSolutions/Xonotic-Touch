package io.github.dixonsolutions.godnotic;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;
import java.util.zip.CRC32;

/**
 * What of the split set is in the data directory, and what may be deleted
 * (docs/DATA_FETCH.md, "What is installed").
 *
 * The files are the truth; {@code .godnotic-set} is a cache of what was
 * verified, so a start does not hash 370 MB to know where it is:
 *
 * <pre>
 * set 20261107-4c53098165d2
 * legacy 20261107-4c53098165d2
 * pack godnotic-20261107-core.pk3 64225800 c49b9836
 * </pre>
 *
 * Every deletion of a whole pack is in this class, and each one is behind the
 * same rule: an old pack goes only after the new packs that replace it are
 * verified and renamed. A complete install from before the split (the
 * official pack names) is never touched while it is what the game runs on.
 */
final class DataInstall {

    static final String STATE = ".godnotic-set";

    /** The old layout's packs: the official names, as the single archive installs them. */
    private static final Pattern OLD_PACK = Pattern.compile(
            "^(xonotic-\\d{8}(_\\d+)?-(data|maps|music|xoncompat|nexcompat)|font-[A-Za-z0-9]+-\\d{8}(_\\d+)?)\\.pk3$");
    private static final Pattern NEW_PACK = Pattern.compile("^godnotic-[A-Za-z0-9._-]+\\.pk3$");
    /** PackFetch.META, the single-archive setup's own state; spelled here so this class needs nothing of Android. */
    private static final String OLD_SETUP = ".packs-setup";
    private static final String[] PART_SUFFIXES = { ".part", ".part.state", ".part.state.tmp" };

    private final File dir;
    private final DataFetch.Env env;
    private DataManifest manifest;
    /** The set the pack lines belong to. */
    private String set;
    /** The set a complete old-layout install stands for. */
    private String legacy;
    /** install_as to "size crc32" of what was verified. */
    private final Map<String, String> lines = new LinkedHashMap<>();
    /** Names of the manifest's packs that are installed. */
    private final Set<String> installed = new HashSet<>();

    DataInstall(File dir, DataFetch.Env env) {
        this.dir = dir;
        this.env = env;
        read();
    }

    File dir() {
        return dir;
    }

    synchronized DataManifest manifest() {
        return manifest;
    }

    synchronized String legacySet() {
        return legacy;
    }

    synchronized String stateSet() {
        return set;
    }

    // ---------------------------------------------------------------- adopt

    /**
     * Takes {@code m} as the set to install. A first manifest over a complete
     * old install records what that install stands for; a manifest with
     * another set id keeps the packs that did not change. Nothing whole is
     * deleted here but through {@link #cleanup}.
     */
    synchronized void adopt(DataManifest m) {
        DataManifest before = manifest;
        manifest = m;
        if (dir.list() == null) {
            // The directory cannot be read just now (storage on its way
            // out): nothing is known, so nothing is decided or deleted.
            installed.clear();
            return;
        }
        boolean old = oldComplete();
        if (legacy != null && !old) {
            // The install that line stood for is no longer whole.
            legacy = null;
        }
        if (old && legacy == null && set == null) {
            // The first time a manifest is seen: this install is that set.
            legacy = m.set;
            env.log("A complete install from before the split stands for set " + m.set);
        }
        if (!old) {
            // A first download in the old layout that never finished: its
            // parts are of no use to anyone now.
            deleteParts(OLD_PACK);
            delete(new File(dir, OLD_SETUP));
            delete(new File(dir, OLD_SETUP + ".tmp"));
        }
        if (set != null && !set.equals(m.set)) {
            // A part of the other set under the same name would resume into a
            // file that fails its check: it goes, unless the manifest that
            // was in use says the pack is the same one. (Parts of packs the
            // new set does not list wait for first_play, with their packs.)
            // Parts first, the set line after: a kill in between finds the
            // old line and does this again.
            env.log("Data set " + set + " -> " + m.set);
            for (DataManifest.Pack p : m.packs) {
                DataManifest.Pack was = before != null ? before.pack(p.name) : null;
                boolean same = was != null && was.installAs.equals(p.installAs) && was.size == p.size
                        && was.crc32 == p.crc32 && was.sha256.equals(p.sha256);
                if (!same) {
                    for (String suffix : PART_SUFFIXES) {
                        delete(new File(dir, p.installAs + suffix));
                    }
                }
            }
        }
        if (!(legacy != null && legacy.equals(m.set))) {
            set = m.set;
        }
        installed.clear();
        Map<String, String> kept = new LinkedHashMap<>();
        for (DataManifest.Pack p : m.packs) {
            String line = line(p);
            if (line.equals(lines.get(p.installAs)) && onDisk(p)) {
                installed.add(p.name);
                kept.put(p.installAs, line);
            }
        }
        lines.clear();
        lines.putAll(kept);
        write();
        cleanup();
    }

    /**
     * A file of the right size with no line (a kill between the rename and
     * the save) is hashed once and adopted or deleted. Reads up to the whole
     * set: never on the UI thread.
     */
    void reconcile() {
        List<DataManifest.Pack> orphans = new ArrayList<>();
        synchronized (this) {
            if (manifest == null) {
                return;
            }
            for (DataManifest.Pack p : manifest.packs) {
                if (!installed.contains(p.name) && onDisk(p)) {
                    orphans.add(p);
                }
            }
        }
        for (DataManifest.Pack p : orphans) {
            File f = new File(dir, p.installAs);
            boolean good;
            try {
                good = verify(f, p);
            } catch (IOException e) {
                good = false;
            }
            synchronized (this) {
                if (manifest == null || manifest.pack(p.name) != p) {
                    return;
                }
                if (good && ready(p)) {
                    env.log("Adopting " + p.installAs + ": whole, though not recorded");
                    recordInstalled(p);
                } else if (!good) {
                    env.log("Removing " + p.installAs + ": the right size but not the right bytes");
                    delete(f);
                }
            }
        }
        synchronized (this) {
            cleanup();
        }
    }

    // ---------------------------------------------------------------- state

    synchronized boolean installed(DataManifest.Pack p) {
        return installed.contains(p.name);
    }

    synchronized boolean firstPlayInstalled() {
        return manifest != null && allInstalled(manifest.firstPlay);
    }

    synchronized boolean complete() {
        return manifest != null && allInstalled(manifest.packs);
    }

    private boolean allInstalled(List<DataManifest.Pack> packs) {
        for (DataManifest.Pack p : packs) {
            if (!installed.contains(p.name)) {
                return false;
            }
        }
        return true;
    }

    /** The old layout is whole: data, maps and music under their official names. */
    boolean oldComplete() {
        return oldComplete(dir);
    }

    static boolean oldComplete(File dir) {
        return hasOld(dir, "-data.pk3") && hasOld(dir, "-maps.pk3") && hasOld(dir, "-music.pk3");
    }

    private static boolean hasOld(File dir, String suffix) {
        String[] names = dir.list();
        if (names == null) {
            return false;
        }
        for (String name : names) {
            if (name.startsWith("xonotic-") && name.endsWith(suffix) && new File(dir, name).isFile()) {
                return true;
            }
        }
        return false;
    }

    /** A complete old install that is the published set: nothing to fetch, now or later. */
    synchronized boolean legacyCurrent() {
        return manifest != null && legacy != null && legacy.equals(manifest.set) && oldComplete();
    }

    /** The game may start: first_play is in, or the old packs keep it whole. */
    synchronized boolean playable() {
        return manifest != null && (allInstalled(manifest.firstPlay) || oldComplete());
    }

    synchronized boolean nothingToFetch() {
        return manifest == null || allInstalled(manifest.packs) || legacyCurrent();
    }

    synchronized long installedBytes() {
        long sum = 0;
        if (manifest != null) {
            for (DataManifest.Pack p : manifest.packs) {
                if (installed.contains(p.name)) {
                    sum += p.size;
                }
            }
        }
        return sum;
    }

    // ---------------------------------------------------------------- order

    /**
     * What to fetch, in order: the wanted map's missing needs, then
     * first_play in list order, then (unless {@code firstPlayOnly}) the rest
     * by priority. A map's own pack always comes after the other packs that
     * map needs, whatever their priorities say.
     */
    synchronized List<DataManifest.Pack> order(String want, boolean firstPlayOnly) {
        LinkedHashSet<DataManifest.Pack> out = new LinkedHashSet<>();
        if (manifest == null) {
            return new ArrayList<>(out);
        }
        if (want != null && !firstPlayOnly) {
            addNeeds(out, want);
        }
        for (DataManifest.Pack p : manifest.firstPlay) {
            add(out, p);
        }
        if (!firstPlayOnly) {
            for (DataManifest.Pack p : manifest.byPriority()) {
                add(out, p);
            }
        }
        List<DataManifest.Pack> list = new ArrayList<>();
        for (DataManifest.Pack p : out) {
            if (!installed.contains(p.name)) {
                list.add(p);
            }
        }
        return list;
    }

    /** The packs of {@code map} that are not installed, its own pack last; empty for a map not in the set. */
    synchronized List<DataManifest.Pack> missingNeeds(String map) {
        LinkedHashSet<DataManifest.Pack> out = new LinkedHashSet<>();
        if (manifest != null && map != null) {
            addNeeds(out, map);
        }
        List<DataManifest.Pack> list = new ArrayList<>();
        for (DataManifest.Pack p : out) {
            if (!installed.contains(p.name)) {
                list.add(p);
            }
        }
        return list;
    }

    private void addNeeds(LinkedHashSet<DataManifest.Pack> out, String map) {
        List<DataManifest.Pack> needs = manifest.needs.get(map);
        if (needs == null) {
            return;
        }
        DataManifest.Pack own = manifest.ownPack.get(map);
        for (DataManifest.Pack p : needs) {
            if (p != own) {
                out.add(p);
            }
        }
        if (own != null) {
            out.add(own);
        }
    }

    private void add(LinkedHashSet<DataManifest.Pack> out, DataManifest.Pack p) {
        if (out.contains(p)) {
            return;
        }
        for (Map.Entry<String, DataManifest.Pack> e : manifest.ownPack.entrySet()) {
            if (e.getValue() == p) {
                addNeeds(out, e.getKey());
            }
        }
        out.add(p);
    }

    /**
     * Whether {@code p} may be renamed into place: for every map whose own
     * pack it is, every other pack that map needs is installed. "The bsp is
     * there" then means "the map is playable", which is what the menu and
     * the engine test.
     */
    synchronized boolean ready(DataManifest.Pack p) {
        if (manifest == null) {
            return false;
        }
        for (Map.Entry<String, DataManifest.Pack> e : manifest.ownPack.entrySet()) {
            if (e.getValue() != p) {
                continue;
            }
            for (DataManifest.Pack need : manifest.needs.get(e.getKey())) {
                if (need != p && !installed.contains(need.name)) {
                    return false;
                }
            }
        }
        return true;
    }

    // -------------------------------------------------------------- publish

    /** Renames a verified part into place and records it. The caller has checked size, crc32 and sha256. */
    synchronized void publish(DataManifest.Pack p, File part) throws IOException {
        if (manifest == null || manifest.pack(p.name) != p) {
            throw new IOException(p.installAs + " is not of the current set");
        }
        if (!ready(p)) {
            throw new IOException(p.installAs + " has to wait for the other packs of its map");
        }
        File dest = new File(dir, p.installAs);
        if (installed.contains(p.name) && onDisk(p)) {
            // Installed already: there is nothing to replace it with.
            delete(part);
            return;
        }
        if (!part.isFile() || part.length() != p.size) {
            // Before anything in place is touched: the fallback below
            // deletes the file it is about to replace.
            throw new IOException(part + " is not the whole pack");
        }
        // rename(2) replaces an existing file in one step; the delete is for
        // a filesystem that will not.
        if (!part.renameTo(dest)) {
            if (dest.exists() && !dest.delete()) {
                throw new IOException("Cannot replace " + dest);
            }
            if (!part.renameTo(dest)) {
                throw new IOException("Cannot move " + part + " to " + dest);
            }
        }
        recordInstalled(p);
        env.log("Installed " + p.installAs + " (" + (p.size >> 10) + " KB)");
        cleanup();
    }

    private void recordInstalled(DataManifest.Pack p) {
        installed.add(p.name);
        lines.put(p.installAs, line(p));
        write();
    }

    // -------------------------------------------------------------- cleanup

    /**
     * Deletes what the installed packs have replaced. An old layout that is
     * whole (it has a {@code legacy} line) goes only when every pack of the
     * new set is in; what is left of an unfinished one goes when first_play
     * is. Packs of an earlier set that this one does not list go when
     * first_play is in.
     */
    private void cleanup() {
        if (manifest == null) {
            return;
        }
        // Not the cache alone: the files are looked at again before anything
        // they replace is deleted.
        boolean first = allInstalled(manifest.firstPlay) && allOnDisk(manifest.firstPlay);
        boolean all = allInstalled(manifest.packs) && allOnDisk(manifest.packs);
        if (legacy != null) {
            if (all) {
                env.log("The new set is complete: removing the packs it replaces");
                deleteOldLayout();
                legacy = null;
                write();
            }
        } else if (first) {
            deleteOldLayout();
        }
        if (first) {
            deleteUnlisted();
        }
    }

    private void deleteOldLayout() {
        String[] names = dir.list();
        if (names == null) {
            return;
        }
        for (String name : names) {
            String base = stripPart(name);
            if (OLD_PACK.matcher(base).matches()) {
                File f = new File(dir, name);
                if (f.isFile()) {
                    env.log("Removing " + name + ": the split set replaces it");
                    delete(f);
                }
            }
        }
        delete(new File(dir, OLD_SETUP));
    }

    private void deleteUnlisted() {
        String[] names = dir.list();
        if (names == null) {
            return;
        }
        Set<String> listed = new HashSet<>();
        for (DataManifest.Pack p : manifest.packs) {
            listed.add(p.installAs);
        }
        for (String name : names) {
            String base = stripPart(name);
            if (NEW_PACK.matcher(base).matches() && !listed.contains(base)) {
                File f = new File(dir, name);
                if (f.isFile()) {
                    env.log("Removing " + name + ": not part of set " + manifest.set);
                    delete(f);
                }
            }
        }
    }

    private void deleteParts(Pattern packs) {
        String[] names = dir.list();
        if (names == null) {
            return;
        }
        for (String name : names) {
            String base = stripPart(name);
            if (!base.equals(name) && packs.matcher(base).matches()) {
                delete(new File(dir, name));
            }
        }
    }

    /** "x.pk3.part.state" -> "x.pk3"; a name without such a suffix comes back as it is. */
    private static String stripPart(String name) {
        for (String suffix : PART_SUFFIXES) {
            if (name.endsWith(suffix)) {
                return name.substring(0, name.length() - suffix.length());
            }
        }
        return name;
    }

    // ------------------------------------------------------------ the files

    private boolean onDisk(DataManifest.Pack p) {
        File f = new File(dir, p.installAs);
        return f.isFile() && f.length() == p.size;
    }

    private boolean allOnDisk(List<DataManifest.Pack> packs) {
        for (DataManifest.Pack p : packs) {
            if (!onDisk(p)) {
                return false;
            }
        }
        return true;
    }

    private static String line(DataManifest.Pack p) {
        return p.size + " " + String.format(java.util.Locale.ROOT, "%08x", p.crc32);
    }

    /** Size, CRC32 and SHA-256 of {@code f} against the manifest's, in one read. */
    static boolean verify(File f, DataManifest.Pack p) throws IOException {
        if (f.length() != p.size) {
            return false;
        }
        MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException("No SHA-256 on this device", e);
        }
        CRC32 crc = new CRC32();
        byte[] buf = new byte[256 * 1024];
        long read = 0;
        try (InputStream in = new FileInputStream(f)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                crc.update(buf, 0, n);
                sha.update(buf, 0, n);
                read += n;
            }
        }
        if (read != p.size || crc.getValue() != p.crc32) {
            return false;
        }
        StringBuilder hex = new StringBuilder(64);
        for (byte b : sha.digest()) {
            hex.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        }
        return hex.toString().equals(p.sha256);
    }

    private void read() {
        File f = new File(dir, STATE);
        if (!f.isFile()) {
            return;
        }
        try {
            for (String raw : DataStatus.readText(f, 1 << 20).split("\n")) {
                String line = raw.trim();
                if (line.startsWith("set ")) {
                    set = line.substring(4).trim();
                } else if (line.startsWith("legacy ")) {
                    legacy = line.substring(7).trim();
                } else if (line.startsWith("pack ")) {
                    String[] w = line.split(" ");
                    if (w.length == 4) {
                        lines.put(w[1], w[2] + " " + w[3]);
                    }
                }
            }
        } catch (IOException e) {
            // Unreadable: nothing counts as verified, and the files that are
            // whole are adopted again by their hashes (reconcile).
            env.log("Could not read " + f + ": " + e);
            set = null;
            legacy = null;
            lines.clear();
        }
    }

    private void write() {
        StringBuilder text = new StringBuilder();
        if (set != null) {
            text.append("set ").append(set).append('\n');
        }
        if (legacy != null) {
            text.append("legacy ").append(legacy).append('\n');
        }
        for (Map.Entry<String, String> e : lines.entrySet()) {
            text.append("pack ").append(e.getKey()).append(' ').append(e.getValue()).append('\n');
        }
        try {
            DataStatus.writeAtomic(new File(dir, STATE), text.toString());
        } catch (IOException e) {
            // The files are the truth: an unsaved line costs one hash later.
            env.log("Could not save " + STATE + ": " + e);
        }
    }

    private void delete(File f) {
        if (f.exists() && !f.delete()) {
            env.log("Could not delete " + f);
        }
    }
}
