#!/system/bin/sh
# PicoET-Enhance: runtime switch for the eye-tracking enhancement.
#
#   picoet.sh status
#   picoet.sh off|dual|toggle    [--force]      (on is accepted as a synonym for dual)
#   picoet.sh ensure              # (internal) inject into the running service if needed
#
# The enhancement lives only in the eye-tracking service's memory: bin/picoet-inject
# ptrace-attaches to pxreyetrackingservice and dlopens lib/libpicoet_hook.so, which installs
# our hooks (per-eye gate and per-eye independent expressions). No system file is written, no mount is
# made, no persistent property is set and ro.pxr.externalfunc is never touched. A switch
# restarts the service and drops every eye/face tracking client; that is refused while
# clients are connected unless --force is given.
#
#   /data/local/tmp/picoet-mode : off|dual (default dual; off = no injection)
#   /data/local/tmp/picoet-gate : off for normal mode, on for dual
#   /data/local/tmp/picoet-eye-hz, picoet-mouth-hz : startup-only positive integer
#       targets, capped at 90; defaults 90/90, mouth clamped to eye. Restart needed.

# Magisk may invoke ./action.sh; the injector requires an absolute payload path.
MODDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 1
BIN=$MODDIR/bin/picoet-inject
LIB=$MODDIR/lib/libpicoet_hook.so
MODE_FILE=/data/local/tmp/picoet-mode
GATE_FILE=/data/local/tmp/picoet-gate
STATE_DIR=/data/adb/picoet-enhance
RUNTIME_DIR=/data/local/tmp/picoet-runtime
RECOVERY_USED=$STATE_DIR/recovery.used
BLOCKED=$STATE_DIR/injection.blocked
UNSAFE_INSTANCE=$STATE_DIR/unsafe.instance
LOG=$STATE_DIR/picoet.log
PLOG=/data/local/tmp/picoet-hook.log            # payload-hardcoded path; keep despite the module rename
SERVICE=pxreyetrackingservice

if [ -f "$MODDIR/hashes.sh" ]; then
    . "$MODDIR/hashes.sh"
else
    INJECT_SHA=
    PAYLOAD_SHA=
fi

# Create the state directory up front: inject_now() redirects into it before any
# log() call gets a chance to mkdir it (first run / fresh install).
mkdir -p "$STATE_DIR" 2>/dev/null

log() { mkdir -p "$STATE_DIR"; echo "$*"; echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }
die() { log "ERROR: $*"; exit 1; }
sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
# Install/boot initializes dual/on; unexpected runtime mode reads still fail closed.
saved_mode() { case "$(cat "$MODE_FILE" 2>/dev/null)" in off|dual) cat "$MODE_FILE" ;; *) echo off ;; esac; }
saved_gate() { case "$(cat "$GATE_FILE" 2>/dev/null)" in on|off) cat "$GATE_FILE" ;; *) echo on ;; esac; }
clients() { dumpsys "$SERVICE" 2>/dev/null | grep -E 'There is [1-9][0-9]* clients'; }
service_pid() {
    p=$(pidof "$SERVICE")
    case "$p" in ''|*[!0-9]*) return 1 ;; esac
    echo "$p"
}
service_identity() {
    p=$(service_pid) || return 1
    stat=$(cat "/proc/$p/stat" 2>/dev/null) || return 1
    # Strip pid and the parenthesized comm, which can itself contain spaces/').
    rest=${stat##*) }
    [ "$rest" != "$stat" ] || return 1
    set -- $rest
    [ "$#" -ge 20 ] || return 1
    shift 19
    case "$1" in ''|*[!0-9]*) return 1 ;; esac
    echo "$p:$1"
}
mapped_exec() {
    [ -n "$1" ] || return 1
    grep -Eq '^[0-9a-f]+-[0-9a-f]+ [-r][-w]x[ps] .* /[^ ]*/libpicoet_hook[.]so( [(]deleted[)])?$' "/proc/$1/maps" 2>/dev/null
}

# The payload atomically reports actual installed flags, not the requested mode.
# Missing/old/invalid reports fail closed. An executable mapping alone is never ready.
runtime_snapshot() {
    RUN_TOKEN=$(service_identity)
    RUN_PID=${RUN_TOKEN%%:*}
    APPLIED_MODE=off
    APPLIED_GATE=off
    EFFECTIVE_EYE_HZ=unreported
    EFFECTIVE_MOUTH_HZ=unreported
    PHASE=unmapped
    MAPPED=no
    [ -n "$RUN_TOKEN" ] || return 0
    mapped_exec "$RUN_PID" || return 0
    MAPPED=yes
    if [ "$(cat "$UNSAFE_INSTANCE" 2>/dev/null)" = "$RUN_TOKEN" ]; then
        PHASE=unsafe
        return 0
    fi
    PHASE=unreported
    if ! IFS=' ' read -r schema report_pid report_start report_phase report_mode report_gate report_eye_hz report_mouth_hz extra < "$RUNTIME_DIR/$RUN_PID.state" 2>/dev/null; then
        return 0
    fi
    [ "$schema" = 2 ] && [ "$report_pid:$report_start" = "$RUN_TOKEN" ] && [ -z "$extra" ] || return 0
    case "$report_phase" in initializing|pending|ready|failed) ;; *) return 0 ;; esac
    case "$report_mode" in off|left|right|dual) ;; *) return 0 ;; esac
    case "$report_gate" in on|off) ;; *) return 0 ;; esac
    case "$report_eye_hz" in [1-9]|[1-8][0-9]|90) ;; *) return 0 ;; esac
    case "$report_mouth_hz" in [1-9]|[1-8][0-9]|90) ;; *) return 0 ;; esac
    [ "$report_mouth_hz" -le "$report_eye_hz" ] || return 0
    [ "$(service_identity)" = "$RUN_TOKEN" ] || return 0
    PHASE=$report_phase
    APPLIED_MODE=$report_mode
    APPLIED_GATE=$report_gate
    EFFECTIVE_EYE_HZ=$report_eye_hz
    EFFECTIVE_MOUTH_HZ=$report_mouth_hz
}
clear_publish() {
    resetprop -n picoet.enhance.gate off 2>/dev/null
    resetprop -n picoet.enhance.mode off 2>/dev/null
}
publish() {
    runtime_snapshot
    if [ "$(getprop picoet.enhance.mode)" != "$APPLIED_MODE" ] ||
       [ "$(getprop picoet.enhance.gate)" != "$APPLIED_GATE" ]; then
        # Close the gate while changing the pair; never briefly announce dual
        # for a different process/mode. The public two-property protocol is unchanged.
        resetprop -n picoet.enhance.gate off 2>/dev/null
        resetprop -n picoet.enhance.mode "$APPLIED_MODE" 2>/dev/null
        resetprop -n picoet.enhance.gate "$APPLIED_GATE" 2>/dev/null
    fi
    [ "$(service_identity)" = "$RUN_TOKEN" ] || clear_publish
}

prepare_runtime() {
    [ ! -L "$RUNTIME_DIR" ] || die "runtime directory must not be a symlink"
    mkdir -p "$RUNTIME_DIR" && chown 0:0 "$RUNTIME_DIR" && chmod 0700 "$RUNTIME_DIR" || die "cannot prepare runtime directory"
}
block_injection() {
    echo "$*" > "$BLOCKED" || die "cannot persist injection stop condition"
    log "automatic injection stopped: $*; select a mode explicitly to retry"
}

# Shared by all watchdog invocations: at most one automatic recovery until the
# next boot or explicit user mode command. Interrupted injections cannot
# form a restart loop by resetting a per-command retry counter.
recover_service() {
    failed_token=$1
    clear_publish
    if [ -f "$RECOVERY_USED" ]; then
        block_injection "recovery budget exhausted after $failed_token (rc=$rc)"
        return 1
    fi
    echo "$failed_token rc=$rc" > "$RECOVERY_USED" || die "cannot persist recovery budget"
    log "recovering failed injection $failed_token (rc=$rc); one fresh-process retry"
    current=$(service_identity)
    if [ "$current" = "$failed_token" ]; then
        setprop ctl.restart "$SERVICE" || die "cannot restart service"
    elif [ -z "$current" ]; then
        setprop ctl.start "$SERVICE" || die "cannot start service"
    fi
    # Exit 5 normally caused init to start a replacement already. Do not restart
    # that new instance a second time; never retry in the failed instance.
    m=0
    while [ $m -lt 20 ]; do
        current=$(service_identity)
        if [ -n "$current" ] && [ "$current" != "$failed_token" ]; then
            sleep 2
            return 0
        fi
        sleep 0.5
        m=$((m + 1))
    done
    block_injection "service did not recover from $failed_token"
    return 1
}

verify_artifacts() {
    [ -x "$BIN" ] || die "injector missing or not executable: $BIN"
    [ -f "$LIB" ] || die "payload missing: $LIB"
    if [ -n "$INJECT_SHA" ] && [ "$(sha "$BIN")" != "$INJECT_SHA" ]; then
        die "injector sha256 mismatch ($BIN)"
    fi
    if [ -n "$PAYLOAD_SHA" ] && [ "$(sha "$LIB")" != "$PAYLOAD_SHA" ]; then
        die "payload sha256 mismatch ($LIB)"
    fi
}

# Inject into the currently running service (no restart). Used at boot and by the watchdog.
inject_now() {
    # Refresh even in off/no-service/blocked states, and before any slow work.
    publish
    [ "$(saved_mode)" = off ] && return 0
    [ -f "$BLOCKED" ] && return 1
    if [ "$MAPPED" = yes ]; then
        case "$PHASE" in ready|pending) return 0 ;; *) return 1 ;; esac
    fi
    [ -n "$RUN_TOKEN" ] || return 1
    verify_artifacts
    prepare_runtime
    n=0
    rc=1
    while [ $n -lt 20 ]; do
        attempt_token=$(service_identity)
        [ -n "$attempt_token" ] || return 1
        pid=${attempt_token%%:*}
        clear_publish
        "$BIN" "$pid" "$LIB" >> "$STATE_DIR/inject.log" 2>&1
        rc=$?
        if [ $rc -eq 5 ]; then
            echo "$attempt_token" > "$UNSAFE_INSTANCE" || die "cannot record unsafe service instance"
        fi
        publish
        if [ $rc -eq 0 ] && [ "$RUN_TOKEN" = "$attempt_token" ] && [ "$MAPPED" = yes ]; then
            log "payload pid=$pid phase=$PHASE applied_mode=$APPLIED_MODE applied_gate=$APPLIED_GATE effective_eye_hz=$EFFECTIVE_EYE_HZ effective_mouth_hz=$EFFECTIVE_MOUTH_HZ"
            [ "$PHASE" != failed ] && [ "$PHASE" != unreported ] && [ "$PHASE" != initializing ]
            return $?
        fi
        n=$((n + 1))
        if [ $rc -eq 4 ] || [ $rc -eq 5 ]; then
            recover_service "$attempt_token" || return 1
        elif [ $rc -eq 3 ]; then
            sleep 1
        else
            block_injection "pid=$pid rc=$rc; no automatic restart for a safe/unknown failure"
            return 1
        fi
    done
    block_injection "startup retry limit reached (pid=$pid rc=$rc)"
    return 1
}

# Restart the service (unloads any previous hook) and apply the saved mode.
restart_and_inject() {
    clear_publish
    old=$(service_pid)
    if [ -n "$old" ]; then
        setprop ctl.restart "$SERVICE" || die "cannot restart service"
    else
        setprop ctl.start "$SERVICE" || die "cannot start service"
    fi
    i=0
    new=
    while [ $i -lt 20 ]; do
        new=$(service_pid)
        [ -n "$new" ] && [ "$new" != "$old" ] && break
        sleep 0.5
        i=$((i + 1))
    done
    [ -n "$new" ] && [ "$new" != "$old" ] || die "service did not restart (old pid ${old:-none})"
    if [ "$(saved_mode)" = off ]; then
        log "switched off: service pid ${old:-none} -> $new, no injection"
    else
        inject_now
    fi
}

[ "$(id -u)" = 0 ] || die "must run as root"

MODE=$1
case "$MODE" in
    status|ensure|off|dual|on|toggle) ;;
    *) echo "usage: $0 status | off|dual|on|toggle [--force] | ensure"; exit 2 ;;
esac
# `on` is the companion-property spelling; normalize it to the saved `dual` mode.
[ "$MODE" = on ] && MODE=dual
if [ "$MODE" != status ]; then
    if [ "${PICOET_CONTROL_LOCKED:-}" != 1 ]; then
        [ -x "$BIN" ] || die "injector/command lock helper missing: $BIN"
        exec "$BIN" --run-locked "$STATE_DIR/control.lock" "$MODDIR/picoet.sh" "$@"
    fi
    # Covers every failure/early exit, including failed restart/artifact checks.
    trap 'publish' 0
fi
case "$MODE" in
    status)
        runtime_snapshot
        echo "saved_mode=$(saved_mode) saved_gate=$(saved_gate) injected=$MAPPED service_pid=${RUN_PID:-none} phase=$PHASE applied_mode=$APPLIED_MODE applied_gate=$APPLIED_GATE effective_eye_hz=$EFFECTIVE_EYE_HZ effective_mouth_hz=$EFFECTIVE_MOUTH_HZ externalfunc=$(getprop ro.pxr.externalfunc)"
        [ -f "$BLOCKED" ] && echo "automatic_injection_blocked=$(cat "$BLOCKED")"
        if [ -f "$PLOG" ]; then
            echo "log_tail:"
            tail -n 5 "$PLOG"
        fi
        clients
        exit 0 ;;
    ensure)
        inject_now
        exit $? ;;
    off|dual|toggle)
        FORCE=0
        for a in "$@"; do
            [ "$a" = --force ] && FORCE=1
        done
        if [ "$FORCE" = 0 ] && [ -n "$(clients)" ]; then
            die "tracking clients connected, restart would drop them; rerun with --force: $(clients | tr '\n' ' ')"
        fi
        if [ "$MODE" = toggle ]; then
            # Decide under the control lock from actual readiness. A failed
            # dual request must retry enabling, not masquerade as enhanced.
            runtime_snapshot
            if [ "$PHASE" = ready ] && [ "$APPLIED_MODE" = dual ] && [ "$APPLIED_GATE" = on ]; then
                MODE=off
            else
                MODE=dual
            fi
        fi
        GATE=off
        [ "$MODE" = dual ] && GATE=on
        log "switching to mode=$MODE gate=$GATE (eye/face tracking restarts)"
        echo "$MODE" > "$MODE_FILE" || die "cannot write $MODE_FILE"
        echo "$GATE" > "$GATE_FILE" || die "cannot write $GATE_FILE"
        rm -f "$BLOCKED" "$RECOVERY_USED" || die "cannot reset recovery budget"
        restart_and_inject || die "mode application failed; see status/inject.log"
        publish
        log "mode=$MODE gate=$(saved_gate) service pid ${old:-none} -> ${new:-none} phase=$PHASE applied_mode=$APPLIED_MODE applied_gate=$APPLIED_GATE"
        exit 0 ;;
esac
