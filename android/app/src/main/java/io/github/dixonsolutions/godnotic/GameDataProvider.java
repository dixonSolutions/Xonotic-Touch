package io.github.dixonsolutions.godnotic;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.Arrays;

/**
 * The game data, read only, in the system file picker: "Godnotic" in its
 * list of places, with the game packs and the player's config.cfg.
 *
 * It is how the web game (https://godnotic.vercel.app) gets the packs this
 * app already downloaded instead of downloading them again: the page's
 * "Use the game data from the Godnotic app" opens the picker that
 * {@code <input type=file>} opens, the player chooses Godnotic and selects
 * every file, and the browser copies them into its own storage (web/appdata.js,
 * docs/WEB.md "An existing install"). The packs live under Android/data,
 * which the picker cannot open since Android 11, so this provider is the
 * only way in.
 *
 * Only the system's document UI can query it (MANAGE_DOCUMENTS); the
 * browser gets read access to exactly the files the player picked, and
 * nothing can be written, renamed or deleted through it.
 */
public final class GameDataProvider extends DocumentsProvider {
    private static final String ROOT = "godnotic";
    private static final String DIR = "data";
    private static final String CONFIG = "config.cfg";
    private static final String PACK_PREFIX = "pk3:";
    private static final String CONFIG_ID = "cfg:" + CONFIG;

    private static final String[] ROOT_COLUMNS = {
        Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_SUMMARY,
        Root.COLUMN_DOCUMENT_ID, Root.COLUMN_ICON, Root.COLUMN_MIME_TYPES,
    };
    private static final String[] DOC_COLUMNS = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_FLAGS, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED,
    };

    @Override
    public boolean onCreate() {
        return true;
    }

    private GameData data() {
        return new GameData(getContext());
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        MatrixCursor.RowBuilder r = c.newRow();
        r.add(Root.COLUMN_ROOT_ID, ROOT);
        r.add(Root.COLUMN_FLAGS, Root.FLAG_LOCAL_ONLY);
        r.add(Root.COLUMN_TITLE, "Godnotic");
        r.add(Root.COLUMN_SUMMARY, "Game data");
        r.add(Root.COLUMN_DOCUMENT_ID, DIR);
        // By name, never an R id: under AndySU the resource table is the
        // host's (HostRes). 0 shows the picker's generic icon.
        r.add(Root.COLUMN_ICON, getContext().getResources().getIdentifier("icon", "mipmap", getContext().getPackageName()));
        r.add(Root.COLUMN_MIME_TYPES, "*/*");
        return c;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        if (DIR.equals(documentId)) {
            MatrixCursor.RowBuilder r = c.newRow();
            r.add(Document.COLUMN_DOCUMENT_ID, DIR);
            r.add(Document.COLUMN_DISPLAY_NAME, "Godnotic game data");
            r.add(Document.COLUMN_MIME_TYPE, Document.MIME_TYPE_DIR);
            r.add(Document.COLUMN_FLAGS, 0);
            return c;
        }
        addFile(c, documentId, fileFor(documentId));
        return c;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        if (!DIR.equals(parentDocumentId)) {
            throw new FileNotFoundException(parentDocumentId);
        }
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        File[] packs = data().dataDir().listFiles();
        if (packs != null) {
            Arrays.sort(packs);
            for (File f : packs) {
                // Whole packs only: a download in progress is "<name>.pk3.part".
                if (f.isFile() && f.getName().endsWith(".pk3") && !f.getName().startsWith(".")) {
                    addFile(c, PACK_PREFIX + f.getName(), f);
                }
            }
        }
        File cfg = configFile();
        if (cfg.isFile()) {
            addFile(c, CONFIG_ID, cfg);
        }
        return c;
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        if (mode == null || !mode.equals("r")) {
            throw new SecurityException("Godnotic's game data is read only");
        }
        return ParcelFileDescriptor.open(fileFor(documentId), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    private File configFile() {
        // config.cfg is written to the userdir's copy of the game directory.
        return new File(new File(GameData.userDir(data().baseDir()), DIR), CONFIG);
    }

    /** The file behind an id, never outside the data folder (no "/" or ".." in names). */
    private File fileFor(String documentId) throws FileNotFoundException {
        if (CONFIG_ID.equals(documentId)) {
            File cfg = configFile();
            if (cfg.isFile()) {
                return cfg;
            }
        } else if (documentId != null && documentId.startsWith(PACK_PREFIX)) {
            String name = documentId.substring(PACK_PREFIX.length());
            if (name.endsWith(".pk3") && !name.contains("/") && !name.startsWith(".")) {
                File f = new File(data().dataDir(), name);
                if (f.isFile()) {
                    return f;
                }
            }
        }
        throw new FileNotFoundException(documentId);
    }

    private static void addFile(MatrixCursor c, String id, File f) {
        MatrixCursor.RowBuilder r = c.newRow();
        r.add(Document.COLUMN_DOCUMENT_ID, id);
        r.add(Document.COLUMN_DISPLAY_NAME, f.getName());
        r.add(Document.COLUMN_MIME_TYPE, f.getName().endsWith(".cfg") ? "text/plain" : "application/zip");
        r.add(Document.COLUMN_FLAGS, 0);
        r.add(Document.COLUMN_SIZE, f.length());
        r.add(Document.COLUMN_LAST_MODIFIED, f.lastModified());
    }
}
