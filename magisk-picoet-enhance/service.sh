#!/system/bin/sh
# late_start: defer all setup until Android has completed boot, then keep
# re-injecting if the service is restarted (the enhancement lives in process memory only).
# Nothing is mounted and no persistent property is written; ro.pxr.externalfunc is never
# touched. The companion app state (picoet.enhance.mode/gate) is transient and re-published
# by picoet.sh whenever the injection state changes.

while [ "$(getprop sys.boot_completed)" != 1 ]; do
    sleep 1
done

MODDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
STATE_DIR=/data/adb/picoet-enhance
while :; do
    [ -d "$MODDIR" ] && [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ] || exit 0
    /system/bin/sh "$MODDIR/picoet.sh" boot
    rc=$?
    [ "$rc" -eq 0 ] && break
    [ "$rc" -eq 6 ] || exit 1
    sleep 1
done

# Always reconcile through the same readiness/mapping checks as a manual ensure.
# This also observes async plugin completion, clears a dead PID's properties and
# allows read-only fragments to reach the injector's bounded recovery path.
# picoet-inject's command lock serializes this with user mode/gate switches.
while [ -d "$MODDIR" ] && [ ! -f "$MODDIR/disable" ] && [ ! -f "$MODDIR/remove" ]; do
    /system/bin/sh "$MODDIR/picoet.sh" ensure >> "$STATE_DIR/service.log" 2>&1
    rc=$?
    if [ "$rc" -eq 3 ]; then
        sleep 30
    else
        sleep 1
    fi
done
