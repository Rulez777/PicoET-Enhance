#!/system/bin/sh
# Magisk app "Action" button: normal (off/off) <-> enhanced (dual/on).
# Pressing it is an explicit request, so a running tracking session is restarted (--force).
MODDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
/system/bin/sh "$MODDIR/picoet.sh" toggle --force
rc=$?
/system/bin/sh "$MODDIR/picoet.sh" status
exit "$rc"
