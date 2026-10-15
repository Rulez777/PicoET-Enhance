#!/system/bin/sh
# Magisk install hook: refuse on any unsupported firmware. This module ships only our own
# artifacts (bin/picoet-inject + lib/libpicoet_hook.so) and never contains PICO bytes; the
# device libraries are only referenced by name/offset at runtime.
SERVICE_SHA=7d39b99ee36ab13a5f39fefa2cfe6677ccc419b65d07e710831a569678f94a5c
PLUGIN_SHA=0ce64527abb923b4eb8bb98f9df8b1c3e73ad95a460460f4140157c0a7363ba1
SLIB=/system/lib64/libpxreyetrackingservice.so
LIB=/system/lib64/libpxreyetracking.phoenix.so

actual=$(sha256sum "$SLIB" | cut -d' ' -f1)
[ "$actual" = "$SERVICE_SHA" ] || abort "! $SLIB sha256 $actual is not the supported build"
actual=$(sha256sum "$LIB" | cut -d' ' -f1)
[ "$actual" = "$PLUGIN_SHA" ] || abort "! $LIB sha256 $actual is not the supported build"

set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/picoet.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755
set_perm "$MODPATH/bin/picoet-inject" 0 0 0755
set_perm "$MODPATH/lib/libpicoet_hook.so" 0 0 0644
set_perm "$MODPATH/hashes.sh" 0 0 0644

# An upgrade is staged until reboot. Keep the running instance's actual state;
# post-fs-data.sh clears it at boot and service.sh republishes the replacement.

# Fresh installs enable the pupil gate and the per-eye expression path.
# Preserve normal mode; migrate legacy enhanced settings to dual/on.
case "$(cat /data/local/tmp/picoet-mode 2>/dev/null)" in
    off) echo off > /data/local/tmp/picoet-gate || abort "! Cannot save normal mode" ;;
    *)
        echo dual > /data/local/tmp/picoet-mode || abort "! Cannot save enhanced mode"
        echo on > /data/local/tmp/picoet-gate || abort "! Cannot save enhanced gate" ;;
esac

ui_print "- PicoET-Enhance (runtime ptrace injection; no PICO bytes, no system writes)"
ui_print "- Fresh-install default: dual/on; upgrades keep off as off/off, otherwise use dual/on."
ui_print "- Saved state: mode=$(cat /data/local/tmp/picoet-mode) gate=$(cat /data/local/tmp/picoet-gate)"
ui_print "- ro.pxr.externalfunc is never touched."
ui_print "- Switch (root, restarts eye/face tracking):"
ui_print "    sh /data/adb/modules/picoet-enhance/picoet.sh dual|off"
ui_print "  or the Action button in the Magisk app (normal off/off <-> enhanced dual/on)"
ui_print "- Reboot to apply at boot; verify: picoet.sh status"
