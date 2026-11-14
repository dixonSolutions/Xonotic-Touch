package io.github.dixonsolutions.godnotic

/**
 * The launcher entry in the manifest of hosts built since the Kotlin shell.
 *
 * It is the same screen as [BootActivity], which the manifests of older
 * AndySU hosts name instead: under AndySU a live update replaces the code but
 * not the manifest, so one implementation has to serve both entries. The
 * screen, the download, its foreground service, the notifications and the
 * update install all live there.
 */
class GodnoticBootActivity : BootActivity()
