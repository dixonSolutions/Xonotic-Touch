// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GTK 4 / libadwaita launcher, Kotlin/Native calling the C API through
// cinterop. Headers and libraries come from a GNOME SDK/Platform tree (a
// Flatpak runtime's files/ directory), never from the build host, so the
// x86_64 and the cross-compiled aarch64 binary link against exactly what the
// Flatpak runs them on. launcher/build.sh finds those trees and passes:
//   -Pxt.gtk.include=<SDK files dir>          headers (any arch)
//   -Pxt.gtk.lib.x64=<files>/lib/x86_64-linux-gnu
//   -Pxt.gtk.lib.arm64=<files>/lib/aarch64-linux-gnu   (enables linuxArm64)
// Kotlin/Native has no Linux aarch64 host, hence the cross build.
import org.jetbrains.kotlin.gradle.plugin.mpp.KotlinNativeTarget

plugins { kotlin("multiplatform") }

val gtkInclude = providers.gradleProperty("xt.gtk.include").orElse("/usr").get()
val libX64 = providers.gradleProperty("xt.gtk.lib.x64").orElse("/usr/lib/x86_64-linux-gnu").get()
val libArm64 = providers.gradleProperty("xt.gtk.lib.arm64").orNull

fun gtkIncludeFlags(triplet: String) = listOf(
    "include/libadwaita-1", "include/gtk-4.0", "include/pango-1.0", "include/harfbuzz",
    "include/gdk-pixbuf-2.0", "include/glycin-2", "include/cairo", "include/freetype2", "include/libpng16",
    "include/pixman-1", "include/graphene-1.0", "lib/$triplet/graphene-1.0/include", "include/fribidi",
    "include/glib-2.0", "lib/$triplet/glib-2.0/include",
).map { "-I$gtkInclude/$it" } +
    // glibconfig.h / graphene-config.h are per arch; an SDK of the other arch
    // still has the x86_64 copies, which match on every LP64 target we build.
    listOf("-I$gtkInclude/lib/x86_64-linux-gnu/glib-2.0/include", "-I$gtkInclude/lib/x86_64-linux-gnu/graphene-1.0/include")

fun KotlinNativeTarget.gtkApp(triplet: String, libDir: String) {
    // Each target compiles the same sources against its own cinterop
    // bindings: no cinterop commonization to go wrong between arches.
    compilations.getByName("main").defaultSourceSet.kotlin.srcDir("src/gtk/kotlin")
    compilations.getByName("main").cinterops.create("gtk") {
        definitionFile.set(project.file("src/nativeInterop/cinterop/gtk.def"))
        compilerOpts(*gtkIncludeFlags(triplet).toTypedArray())
    }
    binaries.executable {
        baseName = "xt-launcher"
        entryPoint = "xt.ui.main"
        linkerOpts(listOf(
            "--as-needed", "--allow-shlib-undefined", "-L$libDir",
            "-l:libadwaita-1.so.0", "-l:libgtk-4.so.1", "-l:libgio-2.0.so.0",
            "-l:libgobject-2.0.so.0", "-l:libglib-2.0.so.0",
        ))
    }
}

kotlin {
    linuxX64 { gtkApp("x86_64-linux-gnu", libX64) }
    if (libArm64 != null) linuxArm64 { gtkApp("aarch64-linux-gnu", libArm64) }
    sourceSets.commonMain.dependencies { implementation(project(":core")) }
}
