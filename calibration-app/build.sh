#!/usr/bin/env bash
# Builds TongueCalibration.apk with Unity's bundled Android SDK/JDK (no Gradle, no AndroidX).
set -euo pipefail
cd "$(dirname "$0")"
U="/c/Program Files/Unity/Hub/Editor/2022.3.22f1/Editor/Data/PlaybackEngines/AndroidPlayer"
SDK="$U/SDK"; BT="$SDK/build-tools/32.0.0"; JAR="$SDK/platforms/android-31/android.jar"; J="$U/OpenJDK/bin"
rm -rf out && mkdir -p out/classes out/dex
"$J/javac" -source 1.8 -target 1.8 -bootclasspath "$JAR" -encoding UTF-8 -d out/classes $(find src -name '*.java')
"$J/java" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --min-api 29 --lib "$JAR" --output out/dex $(find out/classes -name '*.class')
"$BT/aapt2.exe" link -o out/unsigned.apk -I "$JAR" --manifest AndroidManifest.xml
(cd out/dex && zip -q ../unsigned.apk classes.dex 2>/dev/null || "$J/jar" uf ../unsigned.apk classes.dex)
"$BT/zipalign.exe" -f 4 out/unsigned.apk out/aligned.apk
[ -f tonguecal.keystore ] || "$J/keytool" -genkeypair -keystore tonguecal.keystore -alias tonguecal -keyalg RSA -keysize 2048 -validity 10000 -storepass tonguecal -keypass tonguecal -dname "CN=PicoET tongue calibration" >/dev/null
"$J/java" -jar "$BT/lib/apksigner.jar" sign --ks tonguecal.keystore --ks-pass pass:tonguecal --out TongueCalibration.apk out/aligned.apk
"$J/java" -jar "$BT/lib/apksigner.jar" verify TongueCalibration.apk && ls -la TongueCalibration.apk
