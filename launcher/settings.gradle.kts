// SPDX-License-Identifier: GPL-3.0-or-later
pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}
dependencyResolutionManagement {
    repositories { mavenCentral() }
}
rootProject.name = "godnotic-launcher"
include(":core", ":app")
