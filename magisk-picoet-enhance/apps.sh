#!/system/bin/sh
# Installs or updates the apps bundled in this module's app/ directory:
#   app/<package>.apk      the APK
#   app/<package>.version  its versionCode
#   app/<package>.grants   optional runtime permissions the app needs (one per line)
# An app is installed only when it is missing or older than the bundled one (never downgraded).
# If the installed copy is signed with a different key (e.g. another debug build), it is removed
# first. The APK is streamed to the package manager on stdin, so system_server never needs to
# read the module directory. Permissions are granted on every run, not only after an install: a
# reboot right after the install can lose a grant that was not yet written to disk.
# Safe to run repeatedly (customize.sh at install time, service.sh after every boot).
MODDIR=${1:-${0%/*}}
rc=0
for APK in "$MODDIR"/app/*.apk; do
    [ -f "$APK" ] || continue
    pkg=$(basename "$APK" .apk)
    want=$(cat "$MODDIR/app/$pkg.version" 2>/dev/null)
    have=$(dumpsys package "$pkg" 2>/dev/null | sed -n 's/.*versionCode=\([0-9]*\).*/\1/p' | head -n 1)
    if ! { [ -n "$want" ] && [ -n "$have" ] && [ "$have" -ge "$want" ] 2>/dev/null; }; then
        size=$(stat -c %s "$APK") || { rc=1; continue; }
        out=$(cmd package install -r -S "$size" < "$APK" 2>&1)
        case "$out" in
            *INSTALL_FAILED_UPDATE_INCOMPATIBLE*|*INSTALL_FAILED_SHARED_USER_INCOMPATIBLE*)
                pm uninstall "$pkg" >/dev/null 2>&1
                out=$(cmd package install -S "$size" < "$APK" 2>&1) ;;
        esac
        case "$out" in *Success*) ;; *) rc=1; continue ;; esac
    fi
    if [ -f "$MODDIR/app/$pkg.grants" ]; then
        for perm in $(cat "$MODDIR/app/$pkg.grants"); do
            pm grant "$pkg" "$perm" >/dev/null 2>&1 </dev/null
        done
    fi
done
exit $rc
