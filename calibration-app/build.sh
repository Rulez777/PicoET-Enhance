#!/usr/bin/env bash
# Builds TongueCalibration.apk with a plain Android SDK + JDK (no Gradle, no AndroidX).
# SDK: $ANDROID_SDK_ROOT / $ANDROID_HOME, else the SDK bundled with a Unity install.
# JDK: $JAVA_HOME, else Unity's bundled OpenJDK. Build tools: the newest installed version.
set -euo pipefail
cd "$(dirname "$0")"
UNITY_ANDROID=${UNITY_ANDROID:-$(ls -d "/c/Program Files/Unity/Hub/Editor/"*/Editor/Data/PlaybackEngines/AndroidPlayer 2>/dev/null | tail -n 1 || true)}
SDK=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$UNITY_ANDROID/SDK}}
J="$UNITY_ANDROID/OpenJDK/bin"   # a JDK: JAVA_HOME is used only if it has a compiler (not a JRE)
if [ -n "${JAVA_HOME:-}" ]; then
    JH=$(cygpath -u "$JAVA_HOME" 2>/dev/null || echo "$JAVA_HOME")
    { [ -f "$JH/bin/javac" ] || [ -f "$JH/bin/javac.exe" ]; } && J="$JH/bin"
fi
BT=$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -n 1)
JAR=$(ls "$SDK"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -n 1)
EXE=""; [ -f "$BT/aapt2.exe" ] && EXE=".exe"
if [ -z "$BT" ] || [ -z "$JAR" ] || { [ ! -f "$J/javac" ] && [ ! -f "$J/javac.exe" ]; }; then
    echo "Android SDK or JDK not found (set ANDROID_SDK_ROOT and JAVA_HOME to a JDK)"; exit 1
fi
rm -rf out && mkdir -p out/classes out/dex
"$J/javac" -source 1.8 -target 1.8 -bootclasspath "$JAR" -encoding UTF-8 -d out/classes $(find src -name '*.java')
"$J/java" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --min-api 29 --lib "$JAR" --output out/dex $(find out/classes -name '*.class')
"$BT/aapt2$EXE" link -o out/unsigned.apk -I "$JAR" --manifest AndroidManifest.xml
(cd out/dex && zip -q ../unsigned.apk classes.dex 2>/dev/null || "$J/jar" uf ../unsigned.apk classes.dex)
"$BT/zipalign$EXE" -f 4 out/unsigned.apk out/aligned.apk
# Local signing key (not committed). Keep it to sign updates of an installed app.
[ -f tonguecal.keystore ] || "$J/keytool" -genkeypair -keystore tonguecal.keystore -alias tonguecal -keyalg RSA -keysize 2048 -validity 10000 -storepass tonguecal -keypass tonguecal -dname "CN=PicoET tongue calibration" >/dev/null
"$J/java" -jar "$BT/lib/apksigner.jar" sign --ks tonguecal.keystore --ks-pass pass:tonguecal --out TongueCalibration.apk out/aligned.apk
"$J/java" -jar "$BT/lib/apksigner.jar" verify TongueCalibration.apk && ls -la TongueCalibration.apk
