// libpicoet_hook.so -- P1 gate hook + P2 plugin capture / Hook A dry-run +
// P3 eye-mode patch (left/right) + P4 dual (per-eye) gaze.
//
// Runs inside pxreyetrackingservice, injected at runtime by picoet-inject.
// Design: HOOK-DUAL-DESIGN-2026-10-02 sections 4.4/5/7. This library contains
// only our code plus the vendored ShadowHook source; it refers to the PICO
// libraries by name/offset at runtime and ships no PICO bytes.
//
// Service compatibility/init failure blocks all plugin writes. Installation
// results are reported per process; a partially installed gate is not dual gaze.
//
// Build: hook/CMakeLists.txt (ShadowHook 2.0.1, OBJECT libs for ctor order).
// Note: this file is compiled with -fno-omit-frame-pointer because the P4
// Hook A proxy reads the caller frame ([[x29]+0x10] timestamp, x30 caller id).

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <link.h>
#include <pthread.h>
#include <stddef.h>
#include <math.h>
#include <sys/mman.h>
#include "shadowhook.h"

// ---- service library (gate) ----
#define SERVICE_LIB_PATH "/system/lib64/libpxreyetrackingservice.so"
#define SERVICE_LIB_SONAME "libpxreyetrackingservice.so"
#define SERVICE_FNV1A64 0xC51A2999AF634D51ULL /* PICO OS 5.13.7 stock */
#define GATE_SYM "_ZN3PXR7EyeUtil13isBusinessDevEv"

// ---- gaze plugin (Hook A) ----
#define PLUGIN_SONAME "libpxreyetracking.phoenix.so"
#define PLUGIN_OFF_INFER 0x49d0cULL /* infer_entry, function entry */
#define PLUGIN_OFF_HOOKA_CALLER 0x5cbd4ULL /* return address of the only caller */
#define PLUGIN_OFF_HOOKB 0x5c6ccULL
#define PLUGIN_REGION_FNV 0xDF826910BC21B004ULL /* [0x5c2d0, 0x5c6d0) */

// ---- eye-mode patch site (P3) ----
#define EYE_MODE_FILE "/data/local/tmp/picoet-mode" /* off|left|right|dual */
#define GATE_FILE "/data/local/tmp/picoet-gate" /* on|off; missing = on */
#define RUNTIME_DIR "/data/local/tmp/picoet-runtime" /* root-owned, prepared by picoet.sh */
#define PLUGIN_OFF_EYE_MODE 0x5cd68ULL /* ldr w8,[x8,#0x108] */
#define PLUGIN_EYE_MODE_WIN_OFF 0x5cd50ULL
#define PLUGIN_EYE_MODE_WIN_LEN 0x30U
#define PLUGIN_EYE_MODE_FNV 0x79576A0B00A351F2ULL /* [0x5cd50, 0x5cd80) */

// ---- dual (P4): Hook A real logic + Hook B leaf ----
#define PLUGIN_OFF_MAT_CTOR 0x18dd44ULL /* cv::Mat::Mat() */
#define PLUGIN_OFF_MAT_DTOR 0x18ed0cULL /* cv::Mat::~Mat() */
#define PLUGIN_OFF_OP_DELETE 0x34278cULL /* operator delete */
#define PLUGIN_OFF_HOOKB_RETURN 0x5c6d0ULL /* Hook B jumps back here */
#define PLUGIN_TAIL_OFF 0x494c58ULL /* LOAD0 zero tail (leaf home) */
#define PLUGIN_TAIL_END 0x495000ULL

__attribute__((visibility("default"))) unsigned long long picoet_hook_marker = 0;
__attribute__((visibility("default"))) unsigned long long picoet_gate_calls = 0;
__attribute__((visibility("default"))) unsigned long long picoet_gate_installed = 0;
__attribute__((visibility("default"))) unsigned long long picoet_plugin_base = 0;
__attribute__((visibility("default"))) unsigned long long picoet_hooka_calls = 0;
__attribute__((visibility("default"))) unsigned long long picoet_hooka_installed = 0;
__attribute__((visibility("default"))) unsigned long long picoet_eye_mode_patched = 0;
__attribute__((visibility("default"))) unsigned long long picoet_eye_mode = 0; /* 0 off/none, 1 left, 2 right, 3 dual */
__attribute__((visibility("default"))) unsigned long long picoet_dual_installed = 0;
__attribute__((visibility("default"))) unsigned long long picoet_dual_leaf_installed = 0;

// Diagnostic ABI: retain the old prefix offsets for local inspection tools.
// The old sample fields stay zero; frame data is now thread-local, not shared
// with the leaf. n_ok counts both-eye successes; n_fix counts eye writes.
struct picoet_dual_state {
    uint32_t valid;
    uint32_t pad0;
    uint64_t stamp;
    float s_l[2];
    float s_r[2];
    float f_raw[2];
    uint32_t pad1[2];
    uint32_t n_dispatch;
    uint32_t n_ok;
    uint32_t n_fix;
    uint32_t pad2;
    uint32_t n_left;
    uint32_t n_right;
    uint32_t n_invalid_left;
    uint32_t n_invalid_right;
    uint32_t n_stamp_miss;
};

__attribute__((visibility("default"))) struct picoet_dual_state picoet_g;

struct dual_frame {
    uint64_t stamp;
    float left[2];
    float right[2];
    unsigned valid; /* bit 0 left, bit 1 right */
};
static _Thread_local struct dual_frame g_dual_frame;

_Static_assert(offsetof(struct picoet_dual_state, valid) == 0x00, "dual layout");
_Static_assert(offsetof(struct picoet_dual_state, stamp) == 0x08, "dual layout");
_Static_assert(offsetof(struct picoet_dual_state, s_l) == 0x10, "dual layout");
_Static_assert(offsetof(struct picoet_dual_state, s_r) == 0x18, "dual layout");
_Static_assert(offsetof(struct picoet_dual_state, f_raw) == 0x20, "dual layout");
_Static_assert(offsetof(struct picoet_dual_state, n_dispatch) == 0x30, "dual layout");
_Static_assert(offsetof(struct picoet_dual_state, n_ok) == 0x34, "dual layout");
_Static_assert(offsetof(struct picoet_dual_state, n_fix) == 0x38, "dual layout");

// Hook B leaf template (src/picoet_leaf.S); copied to the plugin tail page.
extern const unsigned char picoet_leaf_start[] __attribute__((visibility("hidden")));
extern const unsigned char picoet_leaf_end[] __attribute__((visibility("hidden")));
extern const unsigned char picoet_leaf_apply_slot[] __attribute__((visibility("hidden")));
extern const unsigned char picoet_leaf_ret_pc[] __attribute__((visibility("hidden")));

struct eye_vec3 {
    void *begin;
    void *end;
    void *cap;
};
_Static_assert(sizeof(struct eye_vec3) == 24, "sret ABI");

typedef struct eye_vec3 (*infer_fn_t)(void *, void *, void *, void *, void *, void *, int);
typedef void (*void_fn1_t)(void *);

static infer_fn_t g_orig_infer = NULL;
static void_fn1_t g_mat_ctor = NULL;
static void_fn1_t g_mat_dtor = NULL;
static void_fn1_t g_op_delete = NULL;

static void *g_phoenix_base = NULL;
static void *g_hooka_stub = NULL;
static int g_plugin_guard_ok = 0;
static int g_eye_mode = 0;
static int g_gate_requested = 1;
static unsigned long long g_start_ticks;

static int find_region(unsigned long addr, unsigned long *start, unsigned long *end, int *prot);

static void append_log(const char *msg)
{
    int fd = open("/data/local/tmp/picoet-hook.log",
                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    size_t n = strlen(msg);
    ssize_t w = write(fd, msg, n);
    (void)w;
    w = write(fd, "\n", 1);
    (void)w;
    close(fd);
}

static void log_fmt(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    append_log(buf);
}

static unsigned long long process_start_ticks(void)
{
    char buf[2048];
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    char *p = strrchr(buf, ')'); /* comm can contain spaces and ')' */
    if (!p || p[1] != ' ')
        return 0;
    p += 2;
    for (int field = 3; field < 22; field++) {
        p = strchr(p, ' ');
        if (!p)
            return 0;
        while (*p == ' ')
            p++;
    }
    char *end;
    errno = 0;
    unsigned long long ticks = strtoull(p, &end, 10);
    return errno || end == p || *end != ' ' ? 0 : ticks;
}

// One writer: constructor, then (only if needed) the plugin poll thread.
// Atomic rename prevents readers seeing half a record. PID + /proc starttime
// prevent a previous service instance, including a reused PID, from advertising.
static int publish_runtime(const char *phase)
{
    char path[128], tmp[128], record[160];
    const char *mode = "off";
    if (picoet_dual_installed)
        mode = "dual";
    else if (picoet_eye_mode_patched)
        mode = g_eye_mode == 1 ? "left" : "right";
    snprintf(path, sizeof(path), RUNTIME_DIR "/%ld.state", (long)getpid());
    snprintf(tmp, sizeof(tmp), RUNTIME_DIR "/%ld.tmp", (long)getpid());
    int len = snprintf(record, sizeof(record), "1 %ld %llu %s %s %s\n",
                       (long)getpid(), g_start_ticks, phase, mode,
                       picoet_gate_installed ? "on" : "off");
    if (!g_start_ticks || len <= 0 || (size_t)len >= sizeof(record))
        return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < (size_t)len) {
        ssize_t n = write(fd, record + done, (size_t)len - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    int rc = close(fd);
    if (done == (size_t)len && rc == 0 && rename(tmp, path) == 0)
        return 0;
    unlink(tmp);
    return -1;
}

static void report_runtime(const char *phase)
{
    if (publish_runtime(phase) != 0)
        log_fmt("runtime state: cannot publish %s errno=%d", phase, errno);
}

static int plugin_range_readable(const unsigned char *base, size_t off, size_t len)
{
    unsigned long addr = (unsigned long)base + off, start, end;
    int prot;
    return find_region(addr, &start, &end, &prot) == 0 &&
           (prot & (PROT_READ | PROT_EXEC)) == (PROT_READ | PROT_EXEC) &&
           len <= end - addr;
}

static int fnv1a64_file(const char *path, uint64_t *out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    static unsigned char buf[65536];
    uint64_t h = 14695981039346656037ULL;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    close(fd);
    *out = h;
    return 0;
}

static uint64_t fnv1a64_mem(const unsigned char *p, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ---- gate hook (P1) ---------------------------------------------------------

/* The gate layer can be switched off with /data/local/tmp/picoet-gate (on|off).
 * A missing file means on, so ad-hoc injections keep the P1 behaviour. */
static int read_gate_enabled(void)
{
    int fd = open(GATE_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 1;
    char buf[8];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 1;
    buf[n] = '\0';
    return strncmp(buf, "off", 3) != 0;
}

static int my_isBusinessDev(void)
{
    unsigned long long n = __atomic_add_fetch(&picoet_gate_calls, 1, __ATOMIC_RELAXED);
    if (n <= 3 || (n % 1000) == 0)
        log_fmt("P1 gate: isBusinessDev call #%llu -> 1", n);
    return 1;
}

static int install_gate_hook(void)
{
    append_log("P1 init: libpicoet_hook.so constructed");

    uint64_t h = 0;
    if (fnv1a64_file(SERVICE_LIB_PATH, &h) != 0) {
        append_log("P1 guard: cannot read service lib; skip");
        return -1;
    }
    if (h != SERVICE_FNV1A64) {
        log_fmt("P1 guard: service lib hash 0x%016llX != known; skip", (unsigned long long)h);
        return -1;
    }
    append_log("P1 guard: service lib hash matches known firmware");

    int err = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    if (err != SHADOWHOOK_ERRNO_OK && err != SHADOWHOOK_ERRNO_DUP) {
        log_fmt("P1: shadowhook_init failed errno=%d", err);
        return -1;
    }

    if (!g_gate_requested) {
        append_log("P1 gate: disabled by gate file; skipping gate hook");
        return 0;
    }

    void *orig = NULL;
    void *stub = shadowhook_hook_sym_name(SERVICE_LIB_SONAME, GATE_SYM,
                                          (void *)my_isBusinessDev, &orig);
    if (!stub || !orig) {
        log_fmt("P1: gate hook failed errno=%d", shadowhook_get_errno());
        return -1;
    }
    picoet_gate_installed = 1;
    append_log("P1 gate hook installed (isBusinessDev -> 1)");
    return 0;
}

// ---- plugin discovery (P2) --------------------------------------------------

static int phoenix_cb(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    (void)data;
    if (!info->dlpi_name || !info->dlpi_name[0])
        return 0;
    const char *bn = strrchr(info->dlpi_name, '/');
    bn = bn ? bn + 1 : info->dlpi_name;
    if (strcmp(bn, PLUGIN_SONAME) == 0) {
        g_phoenix_base = (void *)info->dlpi_addr;
        return 1; /* stop */
    }
    return 0;
}

static void *find_phoenix(void)
{
    g_phoenix_base = NULL;
    dl_iterate_phdr(phoenix_cb, NULL);
    return g_phoenix_base;
}

// ---- Hook A dry-run (P2): observe only, never change behavior ---------------

static void hooka_pre(shadowhook_cpu_context_t *ctx, void *data)
{
    (void)data;
    unsigned long long n = __atomic_add_fetch(&picoet_hooka_calls, 1, __ATOMIC_RELAXED);
    if (n <= 5 || (n % 1000) == 0) {
        unsigned long long x30 = ctx->regs[30];
        unsigned long long x3 = ctx->regs[3];
        unsigned long long x4 = ctx->regs[4];
        unsigned long long want = (unsigned long long)g_phoenix_base + PLUGIN_OFF_HOOKA_CALLER;
        log_fmt("P2 HookA #%llu x30=0x%llx x3=%s x4=%s caller_is_0x5cbd4=%s",
                n, x30, x3 ? "set" : "null", x4 ? "set" : "null",
                x30 == want ? "yes" : "no");
    }
}

static int plugin_guards_ok(const unsigned char *b)
{
    static const unsigned char exp49[16] = {
        0xfc, 0x6f, 0xba, 0xa9, 0xfa, 0x67, 0x01, 0xa9,
        0xf8, 0x5f, 0x02, 0xa9, 0xf6, 0x57, 0x03, 0xa9,
    };
    static const unsigned char exp6cc[4] = {0xea, 0x27, 0x40, 0xb9};
    static const unsigned char expbd0[4] = {0x4f, 0xb4, 0xff, 0x97};

    if (!plugin_range_readable(b, PLUGIN_OFF_INFER, 16) ||
        !plugin_range_readable(b, 0x5c2d0, 0x400) ||
        !plugin_range_readable(b, PLUGIN_OFF_HOOKA_CALLER - 4, 4)) {
        append_log("P2 guard: plugin anchors outside readable executable mappings; skip");
        return -1;
    }
    if (memcmp(b + PLUGIN_OFF_INFER, exp49, 16) != 0) {
        append_log("P2 guard: plugin infer_entry prologue mismatch; skip");
        return -1;
    }
    if (memcmp(b + PLUGIN_OFF_HOOKB, exp6cc, 4) != 0 ||
        memcmp(b + PLUGIN_OFF_HOOKA_CALLER - 4, expbd0, 4) != 0) {
        append_log("P2 guard: plugin hook anchors mismatch; skip");
        return -1;
    }
    if (fnv1a64_mem(b + 0x5c2d0, 0x400) != PLUGIN_REGION_FNV) {
        append_log("P2 guard: plugin region hash mismatch; skip");
        return -1;
    }
    g_plugin_guard_ok = 1;
    return 0;
}

static int install_hooka_dryrun(void *base)
{
    const unsigned char *b = (const unsigned char *)base;
    if (plugin_guards_ok(b) != 0)
        return -1;

    g_hooka_stub = shadowhook_intercept_func_addr((void *)(b + PLUGIN_OFF_INFER),
                                                  hooka_pre, NULL, SHADOWHOOK_INTERCEPT_DEFAULT);
    if (!g_hooka_stub) {
        log_fmt("P2: Hook A intercept failed errno=%d", shadowhook_get_errno());
        return -1;
    }
    picoet_hooka_installed = 1;
    picoet_plugin_base = (unsigned long long)base;
    log_fmt("P2 HookA installed (intercept dry-run) base=0x%llx", (unsigned long long)base);
    return 0;
}

// ---- P4 dual: Hook A real proxy + Hook B leaf --------------------------------
//
// Hook A (plugin 0x49d0c, the fusion inference; sole caller 0x5cbd0): replaced
// by infer_proxy via shadowhook_hook_func_addr. The proxy runs the left and
// right single-eye inferences into thread-local state, then the original inference
// with the original arguments, whose result is returned unchanged. Single-eye
// calls go through the saved orig trampoline, never through the hook.
//
// The proxy relies on the caller frame: at entry x29 is the frame of the
// function that called 0x49d0c and x30 is its return address, so with
// -fno-omit-frame-pointer we read fp[1] (x30) to identify the caller and
// [[fp[0]]+0x10] for the frame timestamp (same as cave-dual.S).
//
// Hook B (plugin 0x5c6cc, ldr w10,[sp,#0x24] in the 0x5c2d0 merge point):
// replaced by a plain b to the trampoline in the plugin zero tail page.
// It preserves registers and calls apply_dual with the original stack frame.
// Only per-eye directions/validity are changed; combined output stays stock.

static int vec_point(const struct eye_vec3 *v, float out[2])
{
    if (!v->begin || !v->end)
        return 0;
    if ((uintptr_t)v->end < (uintptr_t)v->begin ||
        (uintptr_t)v->end - (uintptr_t)v->begin < 8)
        return 0;
    float x, y;
    memcpy(&x, v->begin, 4);
    memcpy(&y, (const char *)v->begin + 4, 4);
    if (!isfinite(x) || !isfinite(y))
        return 0;
    if (x == 0.0f && y == 0.0f) /* calibration not ready */
        return 0;
    out[0] = x;
    out[1] = y;
    return 1;
}

static struct eye_vec3 infer_proxy(void *a0, void *a1, void *a2, void *a3, void *a4,
                                   void *a5, int mode)
{
    if (!g_orig_infer) {
        struct eye_vec3 empty = {NULL, NULL, NULL};
        return empty;
    }

    uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
    uint64_t lr = fp[1];
    uint64_t want = (uint64_t)g_phoenix_base + PLUGIN_OFF_HOOKA_CALLER;

    if (!g_phoenix_base || lr != want) {
        /* not the fusion call site: pass through untouched */
        return g_orig_infer(a0, a1, a2, a3, a4, a5, mode);
    }

    /* [[x29]+0x10] with x29 = caller frame at entry (cave-dual.S) */
    uint64_t caller_fp = fp[0];
    uint64_t stamp = *(uint64_t *)(uintptr_t)(*(uint64_t *)(uintptr_t)caller_fp + 0x10);

    struct dual_frame *frame = &g_dual_frame;
    frame->valid = 0;
    frame->stamp = stamp;
    uint32_t n = __atomic_add_fetch(&picoet_g.n_dispatch, 1, __ATOMIC_RELAXED);

    float sl[2] = {0}, sr[2] = {0}, ff[2] = {0};
    int ok_l = 0, ok_r = 0, ok_f = 0;
    unsigned char mat[0x60] __attribute__((aligned(16)));

    if (a3 && a4) {
        struct eye_vec3 r;

        g_mat_ctor(mat);
        r = g_orig_infer(a0, a1, a2, a3, NULL, mat, mode);
        ok_l = vec_point(&r, sl);
        if (r.begin)
            g_op_delete(r.begin);
        g_mat_dtor(mat);

        g_mat_ctor(mat);
        r = g_orig_infer(a0, a1, a2, NULL, a4, mat, mode);
        ok_r = vec_point(&r, sr);
        if (r.begin)
            g_op_delete(r.begin);
        g_mat_dtor(mat);
    }

    struct eye_vec3 fused = g_orig_infer(a0, a1, a2, a3, a4, a5, mode);
    ok_f = vec_point(&fused, ff);

    // With exactly one usable input the original call already ran that eye's
    // calibrated single-eye branch. Reuse it without an extra engine call.
    if (a3 && !a4) {
        ok_l = ok_f;
        memcpy(sl, ff, sizeof sl);
    } else if (!a3 && a4) {
        ok_r = ok_f;
        memcpy(sr, ff, sizeof sr);
    }
    if (ok_l) {
        memcpy(frame->left, sl, sizeof sl);
        frame->valid |= 1;
        __atomic_add_fetch(&picoet_g.n_left, 1, __ATOMIC_RELAXED);
    }
    if (ok_r) {
        memcpy(frame->right, sr, sizeof sr);
        frame->valid |= 2;
        __atomic_add_fetch(&picoet_g.n_right, 1, __ATOMIC_RELAXED);
    }
    if (ok_l && ok_r)
        __atomic_add_fetch(&picoet_g.n_ok, 1, __ATOMIC_RELAXED);
    if (n <= 5 || (n % 1000) == 0)
        log_fmt("P4 independent: #%u mask=%u F=%d L=(%f,%f) R=(%f,%f)",
                n, frame->valid, ok_f, (double)sl[0], (double)sl[1],
                (double)sr[0], (double)sr[1]);

    return fused;
}

// Called by the register-preserving Hook B trampoline on the inference thread.
// The guarded stock assembly puts eye origins in metres and uses a 1.2 m
// reference plane: offset_x = -origin_x / 1.2. Reading the origin avoids any
// dependence on fused values (including non-finite values and filter history).
static void apply_dual(unsigned char *sp)
{
    if (!__atomic_load_n(&picoet_dual_installed, __ATOMIC_ACQUIRE))
        return; /* installing/failed Hook A: preserve stock behaviour */

    struct dual_frame *frame = &g_dual_frame;
    uint64_t stamp;
    memcpy(&stamp, sp + 0x28, sizeof stamp);
    unsigned valid = frame->stamp == stamp ? frame->valid : 0;
    if (frame->stamp != stamp)
        __atomic_add_fetch(&picoet_g.n_stamp_miss, 1, __ATOMIC_RELAXED);
    frame->valid = 0; /* consume at most once, even when both eyes are invalid */

    unsigned applied = 0;
    for (unsigned eye = 0; eye < 2; ++eye) {
        size_t base = eye ? 0xc0 : 0x50;
        const float *single = eye ? frame->right : frame->left;
        int usable = (valid & (1u << eye)) && sp[base] && sp[base + 0x10];
        if (usable) {
            float origin_x, xy[2];
            memcpy(&origin_x, sp + base + 4, sizeof origin_x);
            xy[0] = single[0] - origin_x / 1.2f;
            xy[1] = single[1];
            usable = isfinite(xy[0]) && isfinite(xy[1]);
            if (usable) {
                memcpy(sp + base + 0x14, xy, sizeof xy);
                applied |= 1u << eye;
                __atomic_add_fetch(&picoet_g.n_fix, 1, __ATOMIC_RELAXED);
            }
        }
        if (!usable) {
            sp[base + 0x10] = 0; /* only gaze direction; openness/origin untouched */
            uint32_t *counter = eye ? &picoet_g.n_invalid_right : &picoet_g.n_invalid_left;
            __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
        }
    }
    // One example of each validity combination per tracking thread. This shows
    // actual writeback, rather than inferring it from Hook A installation.
    static _Thread_local unsigned reported;
    if (!(reported & (1u << applied))) {
        reported |= 1u << applied;
        float left[2], right[2], combined[2];
        memcpy(left, sp + 0x64, sizeof left);
        memcpy(right, sp + 0xd4, sizeof right);
        memcpy(combined, sp + 0x20, sizeof combined);
        log_fmt("P4 apply: stamp=%llu source=%u output=%u L=(%f,%f) R=(%f,%f) C=(%f,%f)",
                (unsigned long long)stamp, valid, applied,
                (double)left[0], (double)left[1], (double)right[0], (double)right[1],
                (double)combined[0], (double)combined[1]);
    }
}

static int install_hookb(void *base)
{
    const unsigned char *b = (const unsigned char *)base;
    static const unsigned char exp6cc[4] = {0xea, 0x27, 0x40, 0xb9};

    unsigned long tail = (unsigned long)base + PLUGIN_TAIL_OFF;
    unsigned long tail_end = (unsigned long)base + PLUGIN_TAIL_END;
    unsigned long site = (unsigned long)base + PLUGIN_OFF_HOOKB;

    unsigned long rstart = 0, rend = 0;
    int prot = 0;
    if (find_region(tail, &rstart, &rend, &prot) != 0 ||
        (prot & (PROT_READ | PROT_EXEC)) != (PROT_READ | PROT_EXEC)) {
        append_log("P4: plugin tail not in a readable executable mapping; skip");
        return -1;
    }
    if (tail_end > rend) {
        append_log("P4: plugin tail page extends past the mapping; skip");
        return -1;
    }

    unsigned long srstart = 0, srend = 0;
    int sprot = 0;
    if (find_region(site, &srstart, &srend, &sprot) != 0 ||
        (sprot & (PROT_READ | PROT_EXEC)) != (PROT_READ | PROT_EXEC)) {
        append_log("P4: hook B site not in a readable executable mapping; skip");
        return -1;
    }

    if (memcmp(b + PLUGIN_OFF_HOOKB, exp6cc, 4) != 0) {
        append_log("P4 guard: hook B site mismatch; skip");
        return -1;
    }

    int64_t bdiff = (int64_t)tail - (int64_t)site;
    if ((bdiff & 3) != 0 || bdiff < -(1 << 25) || bdiff >= (1 << 25)) {
        append_log("P4: hook B branch out of range; skip");
        return -1;
    }

    size_t leaf_len = (size_t)(picoet_leaf_end - picoet_leaf_start);
    if (leaf_len == 0 || leaf_len > 512 || tail + leaf_len > tail_end) {
        append_log("P4: leaf does not fit the plugin tail; skip");
        return -1;
    }
    for (size_t i = 0; i < leaf_len; i++) {
        if (b[PLUGIN_TAIL_OFF + i] != 0) {
            append_log("P4: plugin tail is not zero; skip");
            return -1;
        }
    }

    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0)
        pagesz = 4096;
    unsigned long tail_page = tail & ~((unsigned long)pagesz - 1);

    unsigned char leaf[512];
    memcpy(leaf, picoet_leaf_start, leaf_len);
    size_t g_off = (size_t)(picoet_leaf_apply_slot - picoet_leaf_start);
    size_t ret_off = (size_t)(picoet_leaf_ret_pc - picoet_leaf_start);
    if (g_off + 8 > leaf_len || ret_off + 4 > leaf_len) {
        append_log("P4: leaf template slots out of range; skip");
        return -1;
    }
    uint64_t g_addr = (uint64_t)(uintptr_t)&apply_dual;
    memcpy(leaf + g_off, &g_addr, 8);

    unsigned long ret_pc = tail + ret_off;
    unsigned long ret_target = (unsigned long)base + PLUGIN_OFF_HOOKB_RETURN;
    int64_t diff = (int64_t)ret_target - (int64_t)ret_pc;
    if ((diff & 3) != 0 || diff < -(1 << 25) || diff >= (1 << 25)) {
        append_log("P4: hook B return branch out of range; skip");
        return -1;
    }
    uint32_t br = 0x14000000u | ((uint32_t)(diff >> 2) & 0x03FFFFFFu);
    memcpy(leaf + ret_off, &br, 4);

    int changed_tail = 0;
    if ((prot & PROT_WRITE) == 0) {
        if (mprotect((void *)tail_page, (size_t)pagesz, prot | PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            log_fmt("P4: tail mprotect RWX failed errno=%d; skip", errno);
            return -1;
        }
        changed_tail = 1;
    }
    memcpy((void *)tail, leaf, leaf_len);
    __builtin___clear_cache((char *)tail, (char *)tail + leaf_len);
    if (memcmp((const void *)tail, leaf, leaf_len) != 0) {
        if (changed_tail)
            mprotect((void *)tail_page, (size_t)pagesz, prot);
        append_log("P4: leaf readback mismatch; skip");
        return -1;
    }
    if (changed_tail && mprotect((void *)tail_page, (size_t)pagesz, prot) != 0)
        log_fmt("P4: WARNING tail prot restore failed errno=%d", errno);

    unsigned long site_page = site & ~((unsigned long)pagesz - 1);
    int changed_site = 0;
    if ((sprot & PROT_WRITE) == 0) {
        if (mprotect((void *)site_page, (size_t)pagesz, sprot | PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            log_fmt("P4: site mprotect RWX failed errno=%d; skip", errno);
            return -1;
        }
        changed_site = 1;
    }
    uint32_t b_inst = 0x14000000u | ((uint32_t)(bdiff >> 2) & 0x03FFFFFFu);
    __atomic_store_n((uint32_t *)(void *)site, b_inst, __ATOMIC_RELEASE);
    __builtin___clear_cache((char *)site, (char *)site + 4);
    if (__atomic_load_n((const uint32_t *)(const void *)site, __ATOMIC_ACQUIRE) != b_inst) {
        __atomic_store_n((uint32_t *)(void *)site, 0xb94027eau /* ea 27 40 b9 */, __ATOMIC_RELEASE);
        __builtin___clear_cache((char *)site, (char *)site + 4);
        if (changed_site)
            mprotect((void *)site_page, (size_t)pagesz, sprot);
        append_log("P4: hook B site readback mismatch; original restored; skip");
        return -1;
    }
    if (changed_site && mprotect((void *)site_page, (size_t)pagesz, sprot) != 0)
        log_fmt("P4: WARNING site prot restore failed errno=%d", errno);

    picoet_dual_leaf_installed = 1;
    log_fmt("P4 dual: Hook B leaf installed at 0x%lx (site 0x%lx -> b)", tail, site);
    return 0;
}

/* Mat()/ ~Mat()/operator delete entries used by the proxy (design 5.4). */
static int dual_internals_ok(const unsigned char *b)
{
    static const unsigned char exp_ctor[4] = {0xe8, 0x5f, 0xa8, 0x52}; /* mov w8,#0x42ff0000 */
    static const unsigned char exp_dtor[4] = {0xf4, 0x4f, 0xbe, 0xa9}; /* stp x20,x19,[sp,#-0x20]! */
    static const unsigned char exp_del[4] = {0x65, 0xcf, 0xf3, 0x17}; /* b <thunk to operator delete> */

    if (!plugin_range_readable(b, PLUGIN_OFF_MAT_CTOR, 4) ||
        !plugin_range_readable(b, PLUGIN_OFF_MAT_DTOR, 4) ||
        !plugin_range_readable(b, PLUGIN_OFF_OP_DELETE, 4) ||
        memcmp(b + PLUGIN_OFF_MAT_CTOR, exp_ctor, 4) != 0 ||
        memcmp(b + PLUGIN_OFF_MAT_DTOR, exp_dtor, 4) != 0 ||
        memcmp(b + PLUGIN_OFF_OP_DELETE, exp_del, 4) != 0) {
        append_log("P4 guard: Mat/operator-delete prologue mismatch; skip");
        return -1;
    }
    return 0;
}

static int install_dual(void *base)
{
    const unsigned char *b = (const unsigned char *)base;
    if (plugin_guards_ok(b) != 0 || dual_internals_ok(b) != 0)
        return -1;

    /* Leaf first: if the Hook A proxy is never installed the leaf is inert. */
    if (install_hookb(base) != 0)
        return -1;

    g_mat_ctor = (void_fn1_t)(void *)((uintptr_t)base + PLUGIN_OFF_MAT_CTOR);
    g_mat_dtor = (void_fn1_t)(void *)((uintptr_t)base + PLUGIN_OFF_MAT_DTOR);
    g_op_delete = (void_fn1_t)(void *)((uintptr_t)base + PLUGIN_OFF_OP_DELETE);

    /* ShadowHook stores *orig_addr before patching the site (sh_inst.c), so handing
     * it &g_orig_infer directly closes the "site patched but orig not set" window;
     * the NULL guard in infer_proxy stays as a second line of defence. */
    g_orig_infer = NULL;
    void *stub = shadowhook_hook_func_addr((void *)(b + PLUGIN_OFF_INFER),
                                           (void *)infer_proxy, (void **)&g_orig_infer);
    if (!stub || !g_orig_infer) {
        log_fmt("P4: Hook A real hook failed errno=%d (Hook B installed but inert)",
                shadowhook_get_errno());
        return -1;
    }
    __atomic_store_n(&picoet_dual_installed, 1, __ATOMIC_RELEASE);
    picoet_plugin_base = (unsigned long long)base;
    log_fmt("P4 dual: Hook A real installed base=0x%llx orig=0x%llx",
            (unsigned long long)base, (unsigned long long)g_orig_infer);
    return 0;
}

// ---- P3 eye-mode patch (left/right) -----------------------------------------
//
// Replaces one instruction at plugin 0x5cd68:
//   orig : ldr  w8, [x8, #0x108]  (08 09 41 b9)
//   left : movz w8, #1            (28 00 80 52)
//   right: movz w8, #0            (08 00 80 52)
// The mode comes from EYE_MODE_FILE ("off"|"left"|"right"|"dual"; off/dual do
// not patch). The page is made writable, the guard bytes are checked, the new
// instruction is written and verified by readback, the instruction cache is
// flushed, and the original page protection is restored. Any failure only
// logs; the host process is never crashed and unknown firmware is skipped.

static const unsigned char eye_orig4[4] = {0x08, 0x09, 0x41, 0xb9};
static const unsigned char eye_left4[4] = {0x28, 0x00, 0x80, 0x52};
static const unsigned char eye_right4[4] = {0x08, 0x00, 0x80, 0x52};

/* little-endian arm64 instruction word (single 4-byte aligned store) */
static uint32_t inst_word(const unsigned char b[4])
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static int read_eye_mode(void)
{
    int fd = open(EYE_MODE_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log_fmt("P3 mode: open %s failed errno=%d; treat as off", EYE_MODE_FILE, errno);
        return 0;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int read_errno = errno;
    close(fd);
    if (n <= 0) {
        log_fmt("P3 mode: empty/unreadable file (n=%ld errno=%d); treat as off", (long)n, read_errno);
        return 0;
    }
    buf[n] = '\0';
    for (ssize_t i = n - 1; i >= 0; i--) {
        char c = buf[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            buf[i] = '\0';
        else
            break;
    }
    if (strcmp(buf, "left") == 0)
        return 1;
    if (strcmp(buf, "right") == 0)
        return 2;
    if (strcmp(buf, "dual") == 0)
        return 3;
    if (strcmp(buf, "off") != 0)
        log_fmt("P3 mode: unrecognized value '%s'; treat as off", buf);
    return 0;
}

static int maps_line_scope(const char *line, unsigned long *start, unsigned long *end, int *prot)
{
    const char *p = line;
    unsigned long a = 0;
    int digits = 0;
    while (*p && *p != '-') {
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return -1;
        a = (a << 4) | (unsigned long)v;
        p++;
        digits++;
    }
    if (*p != '-' || digits == 0)
        return -1;
    p++;
    unsigned long b = 0;
    digits = 0;
    while (*p) {
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else break;
        b = (b << 4) | (unsigned long)v;
        p++;
        digits++;
    }
    if (digits == 0 || *p != ' ')
        return -1;
    p++;
    int pr = 0;
    if (p[0] == 'r') pr |= PROT_READ; else if (p[0] != '-') return -1;
    if (p[1] == 'w') pr |= PROT_WRITE; else if (p[1] != '-') return -1;
    if (p[2] == 'x') pr |= PROT_EXEC; else if (p[2] != '-') return -1;
    *start = a;
    *end = b;
    *prot = pr;
    return 0;
}

static int find_region(unsigned long addr, unsigned long *start, unsigned long *end, int *prot)
{
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    static char line[512];
    size_t ll = 0;
    char buf[4096];
    int rc = -1;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c != '\n') {
                if (ll + 1 < sizeof(line))
                    line[ll++] = c;
                continue;
            }
            line[ll] = '\0';
            ll = 0;
            unsigned long s = 0, e = 0;
            int pr = 0;
            if (maps_line_scope(line, &s, &e, &pr) == 0 && addr >= s && addr < e) {
                *start = s;
                *end = e;
                *prot = pr;
                rc = 0;
                goto out;
            }
        }
    }
out:
    close(fd);
    return rc;
}

static int apply_eye_mode(void *base, int mode)
{
    const char *label = mode == 1 ? "left" : mode == 2 ? "right" : mode == 3 ? "dual" : "off";

    if (mode != 1 && mode != 2) {
        log_fmt("P3 mode: '%s'; no eye-mode patch", label);
        return 0;
    }

    if (!g_plugin_guard_ok ||
        !plugin_range_readable(base, PLUGIN_EYE_MODE_WIN_OFF, PLUGIN_EYE_MODE_WIN_LEN)) {
        append_log("P3: required plugin/window guard unavailable; skip");
        return -1;
    }

    unsigned long site = (unsigned long)base + PLUGIN_OFF_EYE_MODE;
    unsigned long rstart = 0, rend = 0;
    int prot = 0;
    if (find_region(site, &rstart, &rend, &prot) != 0 ||
        (prot & (PROT_READ | PROT_EXEC)) != (PROT_READ | PROT_EXEC)) {
        append_log("P3: eye-mode site not in a readable executable mapping; skip");
        return -1;
    }

    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0)
        pagesz = 4096;
    unsigned long page = site & ~((unsigned long)pagesz - 1);

    int changed_prot = 0;
    if ((prot & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE)) {
        if (mprotect((void *)page, (size_t)pagesz, prot | PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            log_fmt("P3: mprotect RWX failed errno=%d; skip", errno);
            return -1;
        }
        changed_prot = 1;
    }

    const unsigned char *sitep = (const unsigned char *)site;
    const unsigned char *want = mode == 1 ? eye_left4 : eye_right4;

    if (fnv1a64_mem((const unsigned char *)base + PLUGIN_EYE_MODE_WIN_OFF,
                    PLUGIN_EYE_MODE_WIN_LEN) != PLUGIN_EYE_MODE_FNV) {
        if (changed_prot)
            mprotect((void *)page, (size_t)pagesz, prot);
        append_log("P3 guard: eye-mode window hash mismatch; skip");
        return -1;
    }
    if (memcmp(sitep, eye_orig4, 4) != 0) {
        if (changed_prot)
            mprotect((void *)page, (size_t)pagesz, prot);
        append_log("P3 guard: eye-mode site bytes mismatch; skip");
        return -1;
    }

    uint32_t want_w = inst_word(want);
    __atomic_store_n((uint32_t *)(void *)sitep, want_w, __ATOMIC_RELEASE);
    __builtin___clear_cache((char *)sitep, (char *)sitep + 4);

    if (__atomic_load_n((const uint32_t *)(const void *)sitep, __ATOMIC_ACQUIRE) != want_w) {
        __atomic_store_n((uint32_t *)(void *)sitep, inst_word(eye_orig4), __ATOMIC_RELEASE);
        __builtin___clear_cache((char *)sitep, (char *)sitep + 4);
        if (changed_prot)
            mprotect((void *)page, (size_t)pagesz, prot);
        append_log("P3: readback mismatch after write; original restored; skip");
        return -1;
    }

    if (changed_prot) {
        if (mprotect((void *)page, (size_t)pagesz, prot) != 0) {
            log_fmt("P3: WARNING restore prot failed errno=%d; retrying", errno);
            if (mprotect((void *)page, (size_t)pagesz, prot) != 0)
                log_fmt("P3: WARNING page left RWX; restore failed errno=%d", errno);
        }
    }

    picoet_eye_mode_patched = 1;
    log_fmt("P3 eye patch: mode=%s site=0x%lx orig=080941b9 -> %02x%02x%02x%02x; readback ok; prot=0x%x; p2_guard=%s",
            label, site, want[0], want[1], want[2], want[3], prot,
            g_plugin_guard_ok ? "ok" : "fail");
    return 0;
}

static void on_plugin_found(void *b)
{
    int rc;
    if (g_eye_mode == 3) {
        rc = install_dual(b);
        if (rc != 0)
            append_log("P4: dual install failed; no dual gaze advertised");
    } else {
        rc = install_hooka_dryrun(b);
        if (rc == 0)
            rc = apply_eye_mode(b, g_eye_mode);
    }
    report_runtime(rc == 0 ? "ready" : "failed");
}

static void *plugin_poll_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 300; i++) { /* up to 60 s */
        void *b = find_phoenix();
        if (b) {
            on_plugin_found(b);
            return NULL;
        }
        usleep(200 * 1000);
    }
    append_log("P2 plugin poll: not found after 60s");
    report_runtime("failed");
    return NULL;
}

__attribute__((constructor)) static void picoet_init(void)
{
    static volatile int once = 0;
    if (once)
        return;
    once = 1;

    picoet_hook_marker = 0x5049434F45543150ULL; /* "PICOET1P" */
    g_start_ticks = process_start_ticks();
    g_eye_mode = read_eye_mode();
    g_gate_requested = read_gate_enabled();
    picoet_eye_mode = (unsigned long long)g_eye_mode;
    if (publish_runtime("initializing") != 0) {
        append_log("runtime state unavailable; refusing all hooks (prepare picoet-runtime directory)");
        return;
    }

    // P1: gate hook (stable: ShadowHook init must run after sh_errno_ctor;
    // guaranteed by the object-link order in CMakeLists.txt).
    if (install_gate_hook() != 0) {
        append_log("service guard/gate initialization failed; no plugin writes permitted");
        report_runtime("failed");
        return;
    }
    if (g_eye_mode == 0) {
        report_runtime("ready");
        return;
    }
    report_runtime("pending");

    // P2/P3/P4: find the plugin, then either install the dual hooks (mode
    // "dual") or the Hook A dry-run plus the left/right eye-mode patch.
    void *b = find_phoenix();
    if (b) {
        on_plugin_found(b);
    } else {
        append_log("P2: plugin not loaded yet; starting poll thread");
        pthread_t t;
        if (pthread_create(&t, NULL, plugin_poll_thread, NULL) == 0)
            pthread_detach(t);
        else {
            append_log("P2: poll thread creation failed");
            report_runtime("failed");
        }
    }
}
