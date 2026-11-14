#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The Updates screen's decisions on an AndySU install (AndySUState.java): mode for the
# Automatic / Wi-Fi only switches, Check now and Download now, the status record, download
# progress from the host's .part files. Without a host (AppUpdateState.java): when the app
# updater may download, why a found release waits, the last check, the record's details.
# Plain JVM, no Android SDK: javac + JUnit 4.
#
#   scripts/test-android-updates.sh        (JUNIT_JAR / HAMCREST_JAR to point at the jars;
#                                           otherwise they are looked up in ~/.gradle and ~/.m2)
# The same test also runs under Gradle: ./gradlew testDebugUnitTest in android/.
set -euo pipefail
cd "$(dirname "$0")/.."
find_jar() {
	find ~/.gradle ~/.m2 /usr/share/java -name "$1" -print -quit 2>/dev/null || true
}
junit=${JUNIT_JAR:-$(find_jar 'junit-4.13*.jar')}
hamcrest=${HAMCREST_JAR:-$(find_jar 'hamcrest-core-1.3.jar')}
[ -n "$junit" ] && [ -n "$hamcrest" ] || { echo "JUnit 4 and hamcrest-core jars not found; set JUNIT_JAR and HAMCREST_JAR" >&2; exit 1; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/androidupd.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
pkg=io/github/dixonsolutions/godnotic
javac -Xlint:all -Werror -d "$tmp" -cp "$junit" \
	android/app/src/main/java/$pkg/AndySUState.java \
	android/app/src/main/java/$pkg/AppUpdateState.java \
	android/app/src/test/java/$pkg/AndySUStateTest.java \
	android/app/src/test/java/$pkg/AppUpdateStateTest.java
java -cp "$tmp:$junit:$hamcrest" org.junit.runner.JUnitCore \
	io.github.dixonsolutions.godnotic.AndySUStateTest io.github.dixonsolutions.godnotic.AppUpdateStateTest
