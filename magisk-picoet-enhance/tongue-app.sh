#!/system/bin/sh
# Installs or updates the bundled Tongue Calibration app (dev.picoet.tonguecal) when the installed
# version differs from the one shipped in this module. Streams the APK to the package manager on
# stdin, so the module directory never has to be readable by system_server. Safe to run repeatedly.
MODDIR=${1:-${0%/*}}
APK="$MODDIR/app/TongueCalibration.apk"
[ -f "$APK" ] || exit 0
want=$(cat "$MODDIR/app/version" 2>/dev/null)
have=$(dumpsys package dev.picoet.tonguecal 2>/dev/null | sed -n 's/.*versionCode=\([0-9]*\).*/\1/p' | head -n 1)
[ -n "$want" ] && [ "$have" = "$want" ] && exit 0
size=$(stat -c %s "$APK") || exit 1
cmd package install -r -S "$size" < "$APK" >/dev/null 2>&1
