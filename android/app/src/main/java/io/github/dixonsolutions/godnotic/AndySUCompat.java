package io.github.dixonsolutions.godnotic;

import android.content.Context;

/**
 * Optional AndySU integration, by reflection so the same code builds and runs without it.
 * Under an AndySU host, exposes the running payload version and update API.
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

    /** The host's API level (1, 2, 3...), 0 without a host. */
    static int apiLevel() {
        Object h = host();
        if (h == null) return 0;
        try {
            return (Integer) api().getMethod("apiLevel").invoke(h);
        } catch (Throwable t) {
            return 1; // the first hosts had no apiLevel()
        }
    }

    /** Whether the host's update client has this method: older frozen hosts lack later ones. */
    static boolean has(String method, Class<?>... args) {
        try {
            updateApi().getMethod(method, args);
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    /** "live", "restart" or "next_start"; throws on hosts without apply-policy control. */
    static String applyPolicy() throws ReflectiveOperationException {
        Object client = updateClient();
        if (client == null) throw new IllegalStateException("AndySU host is unavailable");
        return updateApi().getMethod("applyPolicy").invoke(client).toString();
    }

    static void setApplyPolicy(String policy) throws ReflectiveOperationException {
        Object client = updateClient();
        if (client == null) throw new IllegalStateException("AndySU host is unavailable");
        updateApi().getMethod("setApplyPolicy", String.class).invoke(client, policy);
    }

    /** The core's install state as JSON ({"ok": {...}} or {"error": ...}), or null. */
    static String coreState() {
        Object h = host();
        if (h == null) return null;
        try {
            return (String) api().getMethod("core", String.class, String.class).invoke(h, "state", "{}");
        } catch (Throwable t) {
            return null;
        }
    }

    /**
     * Hand the host a status listener ({@code UpdateClient.Listener}, called on the main thread
     * for every status the updater sets, "checking" included). Returns false when the host has
     * none; the caller then notices changes by polling {@link #updateStatus()}.
     */
    static boolean setStatusListener(final StatusListener listener) {
        try {
            Object client = updateClient();
            if (client == null) return false;
            final Class<?> type = Class.forName("com.andysu.api.UpdateClient$Listener");
            Object proxy = java.lang.reflect.Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] { type },
                    (self, method, args) -> {
                        switch (method.getName()) {
                            case "onStatus":
                                listener.onStatus(args != null && args.length > 0 && args[0] != null ? args[0].toString() : "");
                                return null;
                            case "hashCode": return System.identityHashCode(self);
                            case "equals": return args != null && args.length == 1 && self == args[0];
                            case "toString": return "Godnotic update listener";
                            default: return null;
                        }
                    });
            updateApi().getMethod("setListener", type).invoke(client, proxy);
            return true;
        } catch (Throwable t) {
            return false;
        }
    }

    interface StatusListener {
        void onStatus(String statusJson);
    }

    private static Class<?> api() throws ClassNotFoundException {
        return Class.forName("com.andysu.api.HostApi");
    }

    private static Class<?> updateApi() throws ClassNotFoundException {
        return Class.forName("com.andysu.api.UpdateClient");
    }
}
