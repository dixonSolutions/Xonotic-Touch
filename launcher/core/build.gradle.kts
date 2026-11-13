// SPDX-License-Identifier: GPL-3.0-or-later
//
// Launcher logic with no UI and no platform calls: config, profiles, launch
// arguments, mods, pk3 map lists. Plain common Kotlin so the Android app can
// take it as a dependency later (add androidTarget() here); the JVM target is
// what CI tests run on.
plugins { kotlin("multiplatform") }

kotlin {
    jvm()
    linuxX64()
    if (providers.gradleProperty("xt.gtk.lib.arm64").isPresent) linuxArm64()
    sourceSets {
        commonTest.dependencies { implementation(kotlin("test")) }
    }
}
