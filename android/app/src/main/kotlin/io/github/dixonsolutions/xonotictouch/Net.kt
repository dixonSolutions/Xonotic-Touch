package io.github.dixonsolutions.xonotictouch

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.os.Build
import android.util.Log

/**
 * What kind of connection a download would use now, told apart finely enough
 * to say something true about it. "Waiting for Wi-Fi" while the tablet showed
 * Wi-Fi connected was the case that needed this: the Wi-Fi had no internet
 * (local only), so Android routed everything over mobile data, which is
 * metered.
 */
object Net {
    enum class Kind {
        /** No connection at all. */
        OFFLINE,
        /** Wi-Fi, Ethernet or a plan the carrier marks unmetered: download freely. */
        UNMETERED,
        /** Mobile data. */
        MOBILE,
        /** Mobile data, because the connected Wi-Fi has no internet access. */
        MOBILE_WIFI_NO_INTERNET,
        /** Wi-Fi (or another network) the user or the system marked as metered. */
        METERED_WIFI,
    }

    @JvmStatic
    fun kind(context: Context): Kind {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return Kind.OFFLINE
        if (Build.VERSION.SDK_INT < 23) {
            @Suppress("DEPRECATION")
            val info = cm.activeNetworkInfo
            return when {
                info == null || !info.isConnected -> Kind.OFFLINE
                !cm.isActiveNetworkMetered -> Kind.UNMETERED
                else -> Kind.MOBILE
            }
        }
        val active = cm.activeNetwork ?: return Kind.OFFLINE
        val caps = cm.getNetworkCapabilities(active) ?: return Kind.OFFLINE
        if (!caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)) return Kind.OFFLINE
        val unmetered = caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED) ||
            (Build.VERSION.SDK_INT >= 30 && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_TEMPORARILY_NOT_METERED))
        if (unmetered) return Kind.UNMETERED
        if (!caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) return Kind.METERED_WIFI
        @Suppress("DEPRECATION")
        val wifiWithoutInternet = cm.allNetworks.any { n ->
            val c = cm.getNetworkCapabilities(n)
            c != null && c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) &&
                !(c.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET) &&
                    c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED))
        }
        return if (wifiWithoutInternet) Kind.MOBILE_WIFI_NO_INTERNET else Kind.MOBILE
    }

    @JvmStatic
    fun unmetered(context: Context): Boolean = kind(context) == Kind.UNMETERED

    /** One true sentence about the connection, for screens that wait for an unmetered one. */
    @JvmStatic
    @JvmOverloads
    fun describe(context: Context, kind: Kind = kind(context)): String = HostRes.string(context, when (kind) {
        Kind.OFFLINE -> "net_offline"
        Kind.UNMETERED -> "net_unmetered"
        Kind.MOBILE -> "net_mobile"
        Kind.MOBILE_WIFI_NO_INTERNET -> "net_wifi_no_internet"
        Kind.METERED_WIFI -> "net_metered_wifi"
    })

    /**
     * Calls [onUnmetered] (on a binder thread) once an unmetered network with
     * internet comes up. Returns a handle for [stopWatching]; null when the
     * platform cannot watch (API < 24).
     */
    @JvmStatic
    fun watchUnmetered(context: Context, onUnmetered: Runnable): Any? {
        if (Build.VERSION.SDK_INT < 24) return null
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return null
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onCapabilitiesChanged(network: Network, caps: NetworkCapabilities) {
                if (kind(context) == Kind.UNMETERED) onUnmetered.run()
            }
        }
        return try {
            cm.registerDefaultNetworkCallback(callback)
            callback
        } catch (e: RuntimeException) {
            Log.w("Godnotic", "Cannot watch the network", e)
            null
        }
    }

    @JvmStatic
    fun stopWatching(context: Context, handle: Any?) {
        val callback = handle as? ConnectivityManager.NetworkCallback ?: return
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as? ConnectivityManager ?: return
        try {
            cm.unregisterNetworkCallback(callback)
        } catch (e: RuntimeException) {
            // already gone
        }
    }
}
