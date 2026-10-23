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
UNSAFE_HANDLED=$STATE_DIR/unsafe.handled
AUTO_ATTEMPTS=$STATE_DIR/automatic.attempts
LOG=$STATE_DIR/picoet.log
PLOG=/data/local/tmp/picoet-hook.log            # payload-hardcoded path; keep despite the module rename
SERVICE=pxreyetrackingservice

if [ -f "$MODDIR/hashes.sh" ]; then
    . "$MODDIR/hashes.sh"
else
    INJECT_SHA=
    PAYLOAD_SHA=
fi

log() { mkdir -p "$STATE_DIR"; echo "$*"; echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }
die() { log "ERROR: $*"; exit 1; }
sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
# Install/boot initializes dual/on; unexpected runtime mode reads still fail closed.
saved_mode() { value=$("$BIN" --read-file "$MODE_FILE" 2>/dev/null); case "$value" in off|dual) echo "$value" ;; *) echo off ;; esac; }
saved_gate() { value=$("$BIN" --read-file "$GATE_FILE" 2>/dev/null); case "$value" in on|off) echo "$value" ;; *) echo on ;; esac; }
clients() { dumpsys -t 1 "$SERVICE" 2>/dev/null | grep -E 'There is [1-9][0-9]* clients'; }
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
payload_unsafe_token() {
    value=$("$BIN" --read-file "$RUNTIME_DIR/unsafe" 2>/dev/null) || return 1
    unsafe_pid=${value%%:*}
    unsafe_start=${value#*:}
    case "$unsafe_pid" in ''|*[!0-9]*) return 1 ;; esac
    case "$unsafe_start" in ''|*[!0-9]*) return 1 ;; esac
    [ "$unsafe_pid:$unsafe_start" = "$value" ] || return 1
    printf '%s\n' "$value"
}
acknowledge_unsafe() {
    unsafe_token=$(payload_unsafe_token)
    if [ -n "$unsafe_token" ]; then
        echo "$unsafe_token" > "$UNSAFE_INSTANCE" || die "cannot record unsafe service instance"
        echo "$unsafe_token" > "$UNSAFE_HANDLED" || die "cannot acknowledge unsafe service instance"
    fi
}
# A PID is not service readiness: require an actual, bounded Binder dump from
# the same process instance, including all three stock algorithm status lines.
service_ready() {
    [ "$(getprop init.svc.$SERVICE)" = running ] || return 1
    [ "$(service_identity)" = "$1" ] || return 1
    dump=$(dumpsys -t 1 "$SERVICE" 2>/dev/null) || return 1
    for algorithm in ET FT IPD; do
        printf '%s\n' "$dump" | grep -Eq "${algorithm} algorithm is (not )?started" || return 1
    done
    [ "$(service_identity)" = "$1" ]
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
    [ "$(getprop sys.boot_completed)" = 1 ] || return 0
    [ -n "$RUN_TOKEN" ] || return 0
    mapped_exec "$RUN_PID" || return 0
    MAPPED=yes
    if [ "$(cat "$UNSAFE_INSTANCE" 2>/dev/null)" = "$RUN_TOKEN" ] ||
       [ "$(payload_unsafe_token)" = "$RUN_TOKEN" ]; then
        PHASE=unsafe
        return 0
    fi
    PHASE=unreported
    report=$("$BIN" --read-file "$RUNTIME_DIR/$RUN_PID.state" 2>/dev/null) || return 0
    if ! IFS=' ' read -r schema report_pid report_start report_phase report_mode report_gate report_eye_hz report_mouth_hz extra <<EOF
$report
EOF
    then
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
    [ "$PHASE" != ready ] || APPLIED_MODE=$report_mode
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
    "$BIN" --prepare-runtime || die "cannot prepare private runtime directory"
}
initialize_boot() {
    boot_id=$(cat /proc/sys/kernel/random/boot_id) || die "cannot read boot identity"
    [ -n "$boot_id" ] || die "empty boot identity"
    [ "$(cat "$STATE_DIR/boot.id" 2>/dev/null)" != "$boot_id" ] || return 0
    # PID/starttime can repeat across boots. Initialize once under the control
    # lock, including when a manual command wins the race with service.sh.
    "$BIN" --prepare-runtime --reset || die "cannot reset runtime reports"
    rm -f "$BLOCKED" "$RECOVERY_USED" "$UNSAFE_INSTANCE" "$UNSAFE_HANDLED" "$AUTO_ATTEMPTS" "$AUTO_ATTEMPTS.tmp" || die "cannot reset boot budget"
    boot_mode=off
    value=$("$BIN" --read-file "$MODE_FILE" 2>/dev/null)
    [ "$value" = off ] || boot_mode=dual
    "$BIN" --save-mode "$boot_mode" || die "cannot initialize mode"
    clear_publish
    echo "$boot_id" > "$STATE_DIR/boot.id" || die "cannot save boot identity"
    log "boot_completed=1; runtime reset; waiting for Binder readiness"
}
block_injection() {
    echo "$*" > "$BLOCKED" || die "cannot persist injection stop condition"
    log "automatic injection stopped: $*; select a mode explicitly to retry"
}

monotonic_seconds() { cut -d. -f1 /proc/uptime; }
save_automatic_budget() {
    printf '%s\n' "$1" > "$AUTO_ATTEMPTS.tmp" && mv -f "$AUTO_ATTEMPTS.tmp" "$AUTO_ATTEMPTS" ||
        die "cannot persist automatic injection budget"
}
load_automatic_budget() {
    auto_last= auto_count=0 auto_ready=0 auto_previous=
    [ -e "$AUTO_ATTEMPTS" ] || return 0
    [ -f "$AUTO_ATTEMPTS" ] || die "automatic injection budget is not a file"
    IFS=' ' read -r auto_last auto_count auto_ready extra < "$AUTO_ATTEMPTS" || die "cannot read automatic injection budget"
    [ -n "$auto_last" ] && [ -z "$extra" ] || die "invalid automatic injection budget"
    case "$auto_count" in ''|*[!0-9]*) die "invalid automatic attempt count" ;; esac
    case "$auto_ready" in ''|*[!0-9]*) die "invalid automatic ready time" ;; esac
    auto_previous="$auto_last $auto_count $auto_ready"
}
reserve_automatic_attempt() {
    load_automatic_budget
    auto_reserved=0
    [ "$auto_last" != "$1" ] || return 0
    if [ "$auto_count" -ge 3 ]; then
        block_injection "three injected instances failed to remain ready; leaving $1 unmodified"
        return 1
    fi
    # Reserve before the injector runs: even an async exit with no unsafe token
    # must not be able to start a new uncounted automatic attempt.
    save_automatic_budget "$1 $((auto_count + 1)) 0"
    auto_reserved=1
}
release_unused_attempt() {
    [ "$auto_reserved" = 1 ] || return 0
    if [ -n "$auto_previous" ]; then
        save_automatic_budget "$auto_previous"
    else
        rm -f "$AUTO_ATTEMPTS" || die "cannot release unused automatic attempt"
    fi
}
note_stable_instance() {
    load_automatic_budget
    [ "$auto_last" = "$RUN_TOKEN" ] && [ "$auto_count" -gt 0 ] || return 0
    if [ "$PHASE" != ready ]; then
        [ "$auto_ready" -eq 0 ] || save_automatic_budget "$auto_last $auto_count 0"
        return 0
    fi
    auto_now=$(monotonic_seconds) || die "cannot read monotonic time"
    if [ "$auto_ready" -eq 0 ]; then
        save_automatic_budget "$auto_last $auto_count $auto_now"
    elif [ "$((auto_now - auto_ready))" -ge 60 ]; then
        save_automatic_budget "$auto_last 0 $auto_ready"
    fi
}

# Shared by all watchdog invocations: at most one automatic recovery until the
# next boot or explicit user mode command. Interrupted injections cannot
# form a restart loop by resetting a per-command retry counter.
recover_service() {
    failed_token=$1
    clear_publish
    if [ -f "$RECOVERY_USED" ] &&
       { [ "$(cat "$RECOVERY_USED" 2>/dev/null)" != "$failed_token rc=$rc" ] ||
         [ "$(cat "$UNSAFE_HANDLED" 2>/dev/null)" = "$failed_token" ]; }; then
        block_injection "recovery budget exhausted after $failed_token (rc=$rc)"
        if [ "$rc" -eq 5 ]; then
            echo "$failed_token" > "$UNSAFE_HANDLED" || die "cannot acknowledge blocked unsafe instance"
        fi
        return 1
    fi
    echo "$failed_token rc=$rc" > "$RECOVERY_USED" || die "cannot persist recovery budget"
    # Quarantine is immediate, but an automatic failure is ACKed only after its
    # budget reservation. If interrupted between the two writes, resume that
    # same reservation instead of treating it as a second failure.
    if [ "$rc" -eq 5 ]; then
        echo "$failed_token" > "$UNSAFE_HANDLED" || die "cannot acknowledge unsafe service instance"
    fi
    log "recovering failed injection $failed_token (rc=$rc); one fresh-process retry"
    current=$(service_identity)
    if [ "$current" = "$failed_token" ]; then
        setprop ctl.restart "$SERVICE" || {
            block_injection "restart request failed after $failed_token"
            return 1
        }
    elif [ -z "$current" ]; then
        setprop ctl.start "$SERVICE" || {
            block_injection "start request failed after $failed_token"
            return 1
        }
    fi
    # Exit 5 normally caused init to start a replacement already. Do not restart
    # that new instance a second time; never retry in the failed instance.
    m=0
    while [ $m -lt 20 ]; do
        current=$(service_identity)
        if [ -n "$current" ] && [ "$current" != "$failed_token" ] && service_ready "$current"; then
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
    # A rollback may fail later in the payload's asynchronous discovery thread.
    # Its private token survives process exit, unlike the current PID snapshot.
    unsafe_token=$(payload_unsafe_token)
    if [ -n "$unsafe_token" ] && [ "$(cat "$UNSAFE_HANDLED" 2>/dev/null)" != "$unsafe_token" ]; then
        echo "$unsafe_token" > "$UNSAFE_INSTANCE" || die "cannot record unsafe service instance"
        rc=5
        recover_service "$unsafe_token" || return 1
        publish
    fi
    [ "$(saved_mode)" = off ] && return 0
    [ -f "$BLOCKED" ] && return 1
    if [ "$MAPPED" = yes ]; then
        note_stable_instance
        case "$PHASE" in ready|pending) return 0 ;; *) return 1 ;; esac
    fi
    [ -n "$RUN_TOKEN" ] || return 1
    n=0
    rc=1
    while [ $n -lt 20 ]; do
        attempt_token=$(service_identity)
        [ -n "$attempt_token" ] || return 1
        # No ptrace, payload checks or runtime writes until Binder responds.
        service_ready "$attempt_token" || return 1
        verify_artifacts
        prepare_runtime
        reserve_automatic_attempt "$attempt_token" || return 1
        pid=${attempt_token%%:*}
        clear_publish
        "$BIN" "$pid" "$LIB" >> "$STATE_DIR/inject.log" 2>&1
        rc=$?
        [ "$rc" -ne 3 ] || release_unused_attempt
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
        elif [ "$(service_identity)" != "$attempt_token" ]; then
            # A natural exit/replacement is not a persistent injection failure.
            # The outer watchdog will apply the normal readiness checks again.
            return 1
        elif [ $rc -eq 3 ]; then
            # No writes were made. Release the control lock and let the outer
            # watchdog cool down; a transient lack of executor is not a block.
            return 3
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
        token=$(service_identity)
        new=${token%%:*}
        [ -n "$new" ] && [ "$new" != "$old" ] && service_ready "$token" && break
        sleep 0.5
        i=$((i + 1))
    done
    [ $i -lt 20 ] || die "service did not become Binder-ready (old pid ${old:-none})"
    # The old poll thread can publish unsafe after the user's initial ACK.
    # It has now exited and no new payload has been injected; retire that late
    # event before applying the explicitly requested mode with a fresh budget.
    acknowledge_unsafe
    if [ "$(saved_mode)" = off ]; then
        log "switched off: service pid ${old:-none} -> $new, no injection"
    else
        inject_now
    fi
}

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }

MODE=$1
case "$MODE" in
    status|boot|ensure|off|dual|on|toggle) ;;
    *) echo "usage: $0 status | off|dual|on|toggle [--force] | ensure"; exit 2 ;;
esac
# `on` is the companion-property spelling; normalize it to the saved `dual` mode.
[ "$MODE" = on ] && MODE=dual
if [ "$MODE" != status ]; then
    [ "$(getprop sys.boot_completed)" = 1 ] || { echo "boot not complete; no changes made" >&2; exit 1; }
    umask 077
    mkdir -p "$STATE_DIR" || exit 1
    if [ "${PICOET_CONTROL_LOCKED:-}" != 1 ]; then
        [ -x "$BIN" ] || die "injector/command lock helper missing: $BIN"
        exec "$BIN" --run-locked "$STATE_DIR/control.lock" "$MODDIR/picoet.sh" "$@"
    fi
    # Covers every failure/early exit, including failed restart/artifact checks.
    trap 'publish' 0
    initialize_boot
fi
case "$MODE" in
    boot)
        exit 0 ;;
    status)
        runtime_snapshot
        echo "saved_mode=$(saved_mode) saved_gate=$(saved_gate) injected=$MAPPED service_pid=${RUN_PID:-none} phase=$PHASE applied_mode=$APPLIED_MODE applied_gate=$APPLIED_GATE effective_eye_hz=$EFFECTIVE_EYE_HZ effective_mouth_hz=$EFFECTIVE_MOUTH_HZ externalfunc=$(getprop ro.pxr.externalfunc)"
        [ -f "$BLOCKED" ] && echo "automatic_injection_blocked=$(cat "$BLOCKED")"
        if [ -f "$PLOG" ]; then
            echo "log_tail:"
            "$BIN" --read-log | tail -n 5
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
        if [ "$FORCE" = 0 ]; then
            token=$(service_identity)
            if [ -n "$token" ]; then
                service_ready "$token" || die "cannot verify tracking clients: Binder not ready"
                [ -z "$(printf '%s\n' "$dump" | grep -E 'There is [1-9][0-9]* clients')" ] ||
                    die "tracking clients connected, restart would drop them; rerun with --force"
            fi
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
        "$BIN" --save-mode "$MODE" || die "cannot save mode/gate"
        acknowledge_unsafe
        rm -f "$BLOCKED" "$RECOVERY_USED" "$AUTO_ATTEMPTS" "$AUTO_ATTEMPTS.tmp" || die "cannot reset recovery budget"
        restart_and_inject || die "mode application failed; see status/inject.log"
        publish
        log "mode=$MODE gate=$(saved_gate) service pid ${old:-none} -> ${new:-none} phase=$PHASE applied_mode=$APPLIED_MODE applied_gate=$APPLIED_GATE"
        exit 0 ;;
esac
