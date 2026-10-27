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
set_perm "$MODPATH/tongue-app.sh" 0 0 0755

# An upgrade is staged until reboot. Keep the running instance's actual state;
# service.sh clears stale reports and republishes the replacement after boot.

# Fresh installs enable the pupil gate and the per-eye expression path.
# Preserve normal mode; migrate legacy enhanced settings to dual/on.
mode=$("$MODPATH/bin/picoet-inject" --normalize-mode) || abort "! Cannot normalize mode/gate under the control lock"

# Only the first upgrade from before 2.4.0 resets development-era overrides.
# Reinstalls and later upgrades keep settings chosen through the new interface.
installed_version_code=$(sed -n 's/^versionCode=//p' /data/adb/modules/picoet-enhance/module.prop 2>/dev/null)
if [ "${installed_version_code:-0}" -lt 18 ]; then
    rm -f /data/local/tmp/picoet-eye-hz /data/local/tmp/picoet-mouth-hz || abort "! Cannot reset expression rate overrides"
    ui_print "- First 2.4.0 migration: eye/mouth targets reset to 90/90 Hz."
fi

# Tongue left/right (on-headset model): enabled by default. The payload sends it over Wi-Fi to
# UnifiedPicoModule (UDP 19031, the PC subscribes). Delete this file to switch it off.
if [ ! -e /data/local/tmp/tongue-live.ip ]; then
    echo on > /data/local/tmp/tongue-live.ip && chmod 600 /data/local/tmp/tongue-live.ip && chown 0:0 /data/local/tmp/tongue-live.ip
fi
# Bundled Tongue Calibration app: install now; service.sh re-checks it after every boot.
sh "$MODPATH/tongue-app.sh" "$MODPATH" && ui_print "- Tongue Calibration app installed (apps list: Tongue Calibration)." \
    || ui_print "- Tongue Calibration app will be installed after the reboot."

ui_print "- PicoET-Enhance (runtime ptrace injection; no PICO bytes, no system writes)"
ui_print "- Fresh-install default: dual/on; upgrades keep off as off/off, otherwise use dual/on."
ui_print "- Enhanced eye/mouth defaults: 90/90 Hz."
ui_print "- Saved state: mode=$mode"
ui_print "- ro.pxr.externalfunc is never touched."
ui_print "- Tongue L/R: on (needs UnifiedPicoModule with tongue receiver on the PC)."
ui_print "  Off: rm /data/local/tmp/tongue-live.ip"
ui_print "- Switch (root, restarts eye/face tracking):"
ui_print "    sh /data/adb/modules/picoet-enhance/picoet.sh dual|off"
ui_print "  or the Action button in the Magisk app (normal off/off <-> enhanced dual/on)"
ui_print "- Reboot to apply at boot; verify: picoet.sh status"
