package io.github.dixonsolutions.xonotictouch;

import android.content.Context;
import android.content.res.Resources;
import android.util.Log;
import android.util.Xml;

import org.xmlpull.v1.XmlPullParser;

import java.io.InputStream;
import java.util.HashMap;
import java.util.Map;

/**
 * Strings for code that may run on a resource table other than its own.
 *
 * Under AndySU the APK is a frozen host: a live update replaces the dex, the
 * native libraries and the assets, but {@code resources.arsc} and {@code res/}
 * stay those of whichever release the host was built from. aapt numbers
 * resources by name, so a release that adds one renumbers the others, and the
 * {@code R.*} constants compiled into the new dex then point at different
 * entries of the old host's table. That is how 1.2.151 to 1.2.155 died on start
 * ({@code R.id.boot_detail} found the host's ProgressBar) and rolled back.
 *
 * So Java code never passes an {@code R.*} id. Strings come from this build's
 * own strings.xml, which the build copies into the assets (assets do travel
 * with an update), then from the host's table by name, then the name itself.
 * Views are built in code (BootActivity).
 */
public final class HostRes {

    private static final String TAG = "Godnotic";
    /** Written by the copyStringsAsset task in build.gradle. */
    static final String ASSET = "xt/strings.xml";

    private static Map<String, String> strings;

    private HostRes() {}

    public static String string(Context context, String name, Object... args) {
        String text = lookup(context, name);
        if (args.length == 0) {
            return text;
        }
        try {
            return String.format(text, args);
        } catch (RuntimeException e) {
            Log.w(TAG, "String " + name + " does not take its arguments", e);
            return text;
        }
    }

    private static String lookup(Context context, String name) {
        String own = payloadStrings(context).get(name);
        if (own != null) {
            return own;
        }
        Resources res = context.getResources();
        // The resource package is the namespace, which is this Java package;
        // getPackageName() is the applicationId and need not match.
        int id = res.getIdentifier(name, "string", HostRes.class.getPackage().getName());
        if (id != 0) {
            try {
                return res.getString(id);
            } catch (Resources.NotFoundException ignored) {
                // fall through
            }
        }
        Log.w(TAG, "No string " + name + " in this build or in the installed host");
        return name;
    }

    private static synchronized Map<String, String> payloadStrings(Context context) {
        if (strings != null) {
            return strings;
        }
        Map<String, String> parsed = new HashMap<>();
        try (InputStream in = context.getAssets().open(ASSET)) {
            XmlPullParser parser = Xml.newPullParser();
            parser.setInput(in, "UTF-8");
            String name = null;
            StringBuilder text = new StringBuilder();
            for (int ev = parser.getEventType(); ev != XmlPullParser.END_DOCUMENT; ev = parser.next()) {
                if (ev == XmlPullParser.START_TAG && "string".equals(parser.getName())) {
                    name = parser.getAttributeValue(null, "name");
                    text.setLength(0);
                } else if (ev == XmlPullParser.TEXT && name != null) {
                    text.append(parser.getText());
                } else if (ev == XmlPullParser.END_TAG && "string".equals(parser.getName())) {
                    if (name != null) {
                        parsed.put(name, unescape(text.toString()));
                    }
                    name = null;
                }
            }
        } catch (Exception e) {
            // Only the host's table is left; it is right for every string the
            // host already had, by name.
            Log.w(TAG, "Could not read " + ASSET, e);
        }
        strings = parsed;
        return parsed;
    }

    /** The subset of aapt's string rules that strings.xml uses. */
    static String unescape(String raw) {
        String s = raw.trim();
        if (s.length() >= 2 && s.startsWith("\"") && s.endsWith("\"")) {
            s = s.substring(1, s.length() - 1);
        }
        StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length()) {
                char n = s.charAt(++i);
                switch (n) {
                    case 'n': out.append('\n'); break;
                    case 't': out.append('\t'); break;
                    default: out.append(n); break;
                }
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }
}
