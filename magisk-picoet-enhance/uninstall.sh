#!/system/bin/sh
# Runs at the boot after removal. The enhancement only lived in the eye-tracking service's
# memory, so a reboot restores the stock service by itself; only the saved state and the
# mode/gate files need cleaning. No system property was ever written by this module.
MODDIR=${0%/*}
# The native helper uses the verified directory FD and only unlinks our report
# leaves. Never recursively descend into unexpected directories or submounts.
if [ -x "$MODDIR/bin/picoet-inject" ]; then
    "$MODDIR/bin/picoet-inject" --prepare-runtime --reset 2>/dev/null
fi
rmdir /data/local/tmp/picoet-runtime 2>/dev/null || true
rm -f /data/local/tmp/picoet-runtime 2>/dev/null || true
state=/data/adb/picoet-enhance
rm -f "$state/boot.id" "$state/control.lock" "$state/recovery.used" \
    "$state/injection.blocked" "$state/unsafe.instance" "$state/unsafe.handled" \
    "$state/automatic.attempts" "$state/automatic.attempts.tmp" \
    "$state/picoet.log" "$state/inject.log" "$state/service.log" "$state"/setting.* 2>/dev/null || true
rmdir "$state" 2>/dev/null || true
rm -f /data/local/tmp/picoet-mode /data/local/tmp/picoet-gate /data/local/tmp/picoet-hook.log
rm -f /data/local/tmp/picoet-eye-hz /data/local/tmp/picoet-mouth-hz
resetprop --delete picoet.enhance.mode 2>/dev/null
resetprop --delete picoet.enhance.gate 2>/dev/null
