package io.github.dixonsolutions.xonotictouch;

import android.content.Context;

/**
 * Optional AndySU integration, by reflection so the same code builds and runs without it.
 * Under an AndySU host: the running payload's version, and the APK updater stands down (AndySU
 * updates the app; installing a full APK would replace the frozen host).
 */
final class AndySUCompat {
    private AndySUCompat() {}

    static Object host() {
        try {
            return Class.forName("com.andysu.api.Host").getMethod("current").invoke(null);
        } catch (Throwable t) {
            return null;
        }
    }

    static boolean present() {
        return host() != null;
    }

    static boolean autoEnabled(Context context) {
        if (present()) {
            try {
                return "auto".equals(mode());
            } catch (ReflectiveOperationException ignored) {
                // Older hosts keep their configured mode; the Android APK
                // updater preference remains the best available fallback.
            }
        }
        return AppUpdater.isAutoInstall(context);
    }

    static String payloadVersion() {
        Object h = host();
        if (h == null) return null;
        try {
            return (String) api().getMethod("version").invoke(h);
        } catch (Throwable t) {
            return null;
        }
    }

    static Object updateClient() throws ReflectiveOperationException {
        Object h = host();
        if (h == null) return null;
        return api().getMethod("updates").invoke(h);
    }

    static String updateStatus() throws ReflectiveOperationException {
        Object client = updateClient();
        return client == null ? null : updateApi().getMethod("status").invoke(client).toString();
    }

    static String mode() throws ReflectiveOperationException {
        Object client = updateClient();
        return client == null ? null : updateApi().getMethod("mode").invoke(client).toString();
    }

    static void setMode(String mode) throws ReflectiveOperationException {
        Object client = updateClient();
        if (client == null) throw new IllegalStateException("AndySU host is unavailable");
        updateApi().getMethod("setMode", String.class).invoke(client, mode);
    }

    static void checkNow() throws ReflectiveOperationException {
        Object client = updateClient();
        if (client == null) throw new IllegalStateException("AndySU host is unavailable");
        updateApi().getMethod("checkNow").invoke(client);
    }

    static void applyNow() throws ReflectiveOperationException {
        Object client = updateClient();
        if (client == null) throw new IllegalStateException("AndySU host is unavailable");
        updateApi().getMethod("applyNow").invoke(client);
    }

    private static Class<?> api() throws ClassNotFoundException {
        return Class.forName("com.andysu.api.HostApi");
    }

    private static Class<?> updateApi() throws ClassNotFoundException {
        return Class.forName("com.andysu.api.UpdateClient");
    }
}
