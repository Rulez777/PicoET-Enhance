#!/system/bin/sh
# late_start: apply the saved mode once the eye-tracking service is up, then keep
# re-injecting if the service is restarted (the enhancement lives in process memory only).
# Nothing is mounted and no persistent property is written; ro.pxr.externalfunc is never
# touched. The companion app state (picoet.enhance.mode/gate) is transient and re-published
# by picoet.sh whenever the injection state changes.

MODDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
STATE_DIR=/data/adb/picoet-enhance
LOG=/data/local/tmp/picoet-hook.log             # payload-hardcoded path; keep despite the module rename
MODE_FILE=/data/local/tmp/picoet-mode
GATE_FILE=/data/local/tmp/picoet-gate

# The redirects below must not fail on a fresh install / first boot: create the
# state directory before anything writes into it.
mkdir -p "$STATE_DIR" 2>/dev/null
rm -f "$STATE_DIR/recovery.used" "$STATE_DIR/injection.blocked" "$STATE_DIR/unsafe.instance"

# clear the transient companion-app state first: a boot where the module is disabled must
# not look "enhanced"
resetprop --delete picoet.enhance.mode 2>/dev/null
resetprop --delete picoet.enhance.gate 2>/dev/null

# Preserve normal mode; migrate legacy enhanced settings to dual/on.
case "$(cat "$MODE_FILE" 2>/dev/null)" in
    off) echo off > "$GATE_FILE" || exit 1 ;;
    *)
        echo dual > "$MODE_FILE" || exit 1
        echo on > "$GATE_FILE" || exit 1 ;;
esac

# wait for the service (up to ~30 s), then apply the saved state
i=0
while [ $i -lt 15 ]; do
    if [ -n "$(pidof pxreyetrackingservice)" ]; then
        /system/bin/sh "$MODDIR/picoet.sh" ensure >> "$STATE_DIR/service.log" 2>&1
        break
    fi
    sleep 2
    i=$((i + 1))
done

# Always reconcile through the same readiness/mapping checks as a manual ensure.
# This also observes async plugin completion, clears a dead PID's properties and
# allows read-only fragments to reach the injector's bounded recovery path.
# picoet-inject's command lock serializes this with user mode/gate switches.
(
    while [ -d "$MODDIR" ]; do
        sleep 1
        /system/bin/sh "$MODDIR/picoet.sh" ensure >> "$STATE_DIR/service.log" 2>&1
    done
) &

echo "$(date) service.sh: mode=$(cat "$MODE_FILE" 2>/dev/null) gate=$(cat "$GATE_FILE" 2>/dev/null) ro.pxr.externalfunc=$(getprop ro.pxr.externalfunc)" >> "$LOG" 2>/dev/null
