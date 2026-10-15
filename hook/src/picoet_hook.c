// libpicoet_hook.so -- runtime payload for the eye-tracking service.
//
// Two features, both installed only inside pxreyetrackingservice:
//   P1 gate: PXR::EyeUtil::isBusinessDev() returns 1, so the eye-tracking
//            service writes real pupil diameter data to shared memory.
//   Expression independence (r4): the FaceTrackor expression model no longer
//            mixes the two eyes. EyeLook, Blink, Wide and the brow channels
//            are computed and filtered per eye, each with its own quality
//            window, reference image and history, and the filtered right inner
//            brow plus the per-eye quality states are exported as the EPI1
//            extension on the facial prefix.
//
// Runs inside the service, injected at runtime by picoet-inject. This library
// contains only our code plus the vendored ShadowHook source; it refers to the
// PICO libraries by name/offset at runtime and ships no PICO bytes.
//
// Installation is all-or-nothing: every compatibility guard, hook and patch is
// checked, and any failure rolls back what was already installed. The runtime
// state file reports only what was actually installed.
//
// Build: hook/CMakeLists.txt (ShadowHook 2.0.1, OBJECT libs for ctor order).
// This file is compiled as C++17; -fno-omit-frame-pointer is required because
// merge_proxy reads its caller via __builtin_return_address.

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
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "shadowhook.h"

// ---- exported sentinels and counters (read with dlsym) ----------------------
extern "C" {
__attribute__((visibility("default"))) unsigned long long picoet_hook_marker = 0;
__attribute__((visibility("default"))) unsigned long long picoet_gate_calls = 0;
__attribute__((visibility("default"))) unsigned long long picoet_gate_installed = 0;
__attribute__((visibility("default"))) unsigned long long picoet_plugin_base = 0;
__attribute__((visibility("default"))) unsigned long long picoet_dual_installed = 0;
}

namespace {

// ---- constants --------------------------------------------------------------

// Service library (gate).
constexpr const char kServiceLibPath[] = "/system/lib64/libpxreyetrackingservice.so";
constexpr const char kServiceLibSoname[] = "libpxreyetrackingservice.so";
constexpr uint64_t kServiceFnv1a64 = 0xC51A2999AF634D51ULL; /* PICO OS 5.13.7 stock */
constexpr const char kGateSym[] = "_ZN3PXR7EyeUtil13isBusinessDevEv";

// Control files and log.
constexpr const char kEyeModeFile[] = "/data/local/tmp/picoet-mode"; /* off|dual */
constexpr const char kGateFile[] = "/data/local/tmp/picoet-gate";    /* on|off; missing = on */
constexpr const char kRuntimeDir[] = "/data/local/tmp/picoet-runtime";
constexpr const char kHookLog[] = "/data/local/tmp/picoet-hook.log";

// Gaze plugin and the expression-independent hook sites.
constexpr const char kPluginSoname[] = "libpxreyetracking.phoenix.so";
constexpr uint64_t kPluginMinLoadBytes = 0x34279c; /* first PT_LOAD R|X span */
constexpr size_t kOffInfer = 0x613e0;
constexpr size_t kOffTrackor = 0x625b4;
constexpr size_t kOffOwnerInit = 0x60998;
constexpr size_t kOffNormalExit = 0x62c6c;
constexpr size_t kOffCatchExit = 0x62f70;
constexpr size_t kOffMerge = 0x62f90;
constexpr size_t kOffDestroy = 0x6016c;
constexpr size_t kOffExport = 0x60754;
constexpr size_t kOffBlinkCopyPatch = 0x62a8c;
constexpr size_t kOffSharedRefPatch = 0x62b70;
constexpr uint32_t kWordMerge = 0xd10503ff;
constexpr uint32_t kWordExport = 0xd63f02c0;
constexpr uint32_t kWordBlinkCopy = 0x540000c1;
constexpr uint32_t kWordSharedRef = 0x2a1c0368;
constexpr uint32_t kWordDestroy = 0xf81e0ff3;
constexpr uint32_t kPatchBlinkCopy = 0x14000010;  /* b +0x40: skip good-eye blink copy */
constexpr uint32_t kPatchSharedRef = 0x14000023;  /* b +0x8c: skip shared reference tail */
constexpr const char kExpressionVersion[] = "expression-independent-r4-20261014";

unsigned long long g_start_ticks;
int g_enhanced_mode = 0;
int g_gate_requested = 1;

// ---- logging ----------------------------------------------------------------

void append_log(const char *msg)
{
    int fd = open(kHookLog, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    size_t n = strlen(msg);
    ssize_t w = write(fd, msg, n);
    (void)w;
    w = write(fd, "\n", 1);
    (void)w;
    close(fd);
}

void log_fmt(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    append_log(buf);
}

// ---- process identity and runtime state -------------------------------------

unsigned long long process_start_ticks(void)
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
// mode=dual only when the gate hook and the expression hooks are both installed.
int publish_runtime(const char *phase)
{
    char path[128], tmp[128], record[160];
    const char *mode = __atomic_load_n(&picoet_dual_installed, __ATOMIC_RELAXED) ? "dual" : "off";
    snprintf(path, sizeof(path), "%s/%ld.state", kRuntimeDir, (long)getpid());
    snprintf(tmp, sizeof(tmp), "%s/%ld.tmp", kRuntimeDir, (long)getpid());
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

void report_runtime(const char *phase)
{
    if (publish_runtime(phase) != 0)
        log_fmt("runtime state: cannot publish %s errno=%d", phase, errno);
}

// ---- generic helpers --------------------------------------------------------

int fnv1a64_file(const char *path, uint64_t *out)
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

uint64_t fnv1a64_mem(const unsigned char *p, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

template<class T> T load(const void *object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const unsigned char *>(object) + offset, sizeof(value));
    return value;
}

template<class T> void store(void *object, size_t offset, T value)
{
    std::memcpy(static_cast<unsigned char *>(object) + offset, &value, sizeof(value));
}

// ---- gate hook (P1) ---------------------------------------------------------

/* The gate layer can be switched off with /data/local/tmp/picoet-gate (on|off).
 * A missing file means on, so ad-hoc injections keep the P1 behaviour. */
int read_gate_enabled(void)
{
    int fd = open(kGateFile, O_RDONLY | O_CLOEXEC);
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

int my_isBusinessDev(void)
{
    unsigned long long n = __atomic_add_fetch(&picoet_gate_calls, 1, __ATOMIC_RELAXED);
    if (n <= 3 || (n % 1000) == 0)
        log_fmt("P1 gate: isBusinessDev call #%llu -> 1", n);
    return 1;
}

int install_gate_hook(void)
{
    append_log("P1 init: libpicoet_hook.so constructed");

    uint64_t h = 0;
    if (fnv1a64_file(kServiceLibPath, &h) != 0) {
        append_log("P1 guard: cannot read service lib; skip");
        return -1;
    }
    if (h != kServiceFnv1a64) {
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
    void *stub = shadowhook_hook_sym_name(kServiceLibSoname, kGateSym,
                                          (void *)my_isBusinessDev, &orig);
    if (!stub || !orig) {
        log_fmt("P1: gate hook failed errno=%d", shadowhook_get_errno());
        return -1;
    }
    __atomic_store_n(&picoet_gate_installed, 1, __ATOMIC_RELEASE);
    append_log("P1 gate hook installed (isBusinessDev -> 1)");
    return 0;
}

// ---- plugin discovery -------------------------------------------------------

int phoenix_cb(struct dl_phdr_info *info, size_t, void *opaque)
{
    void **base = static_cast<void **>(opaque);
    if (!info->dlpi_name || !info->dlpi_name[0])
        return 0;
    const char *bn = strrchr(info->dlpi_name, '/');
    bn = bn ? bn + 1 : info->dlpi_name;
    if (strcmp(bn, kPluginSoname) != 0)
        return 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto &p = info->dlpi_phdr[i];
        if (p.p_type == PT_LOAD && p.p_flags == (PF_R | PF_X) && p.p_vaddr == 0 &&
            p.p_memsz >= kPluginMinLoadBytes)
            *base = (void *)info->dlpi_addr;
    }
    return 0;
}

void *find_phoenix(void)
{
    void *base = NULL;
    dl_iterate_phdr(phoenix_cb, &base);
    return base;
}

// ---- expression-independent machinery (verified r4) -------------------------
//
// The stock expression model mixes the two eyes before output. The independent
// path keeps each eye's own raw inference result, quality window, reference
// image, numerical history and stock Kalman filter, and writes the filtered
// result back over the 19 standard eye slots. The stock mouth processing, the
// combined eye validity and the owner lifecycle keep running untouched.

// Native ABI types used by the hooked functions. No PICO bytes are embedded.
struct expr_vec {
    void *begin, *end, *capacity;
};
struct expr_mat {
    alignas(8) unsigned char bytes[0x60];
};
struct expr_output {
    float values[54];
};
static_assert(sizeof(expr_vec) == 24, "stock AAPCS64 vector return ABI");
static_assert(sizeof(expr_mat) == 0x60, "stock Mat storage");
static_assert(sizeof(expr_output) == 0xd8, "stock AAPCS64 POD return ABI");

using infer_fn_t = expr_vec (*)(void *, void *, const expr_vec *);
using trackor_fn_t = expr_output (*)(void *, const expr_mat *, const expr_mat *, const expr_mat *);
using init_fn_t = void (*)(void *);

// Stock helper call layout for the native adapter, offsets from plugin base.
struct StockLayout {
    static constexpr uintptr_t kalman_construct = 0x159b90;
    static constexpr uintptr_t kalman_correct = 0x15a528;
    static constexpr uintptr_t kalman_predict = 0x15a260;
    static constexpr uintptr_t kalman_bytes = 0x600;
    static constexpr uintptr_t kalman_transition = 0xc0;
    static constexpr uintptr_t kalman_measurement = 0x180;
    static constexpr uintptr_t kalman_process_noise = 0x1e0;
    static constexpr uintptr_t kalman_measurement_noise = 0x240;
    static constexpr uintptr_t mat_bytes = 0x60;
    static constexpr uintptr_t mat_construct = 0x18dd44;
    static constexpr uintptr_t mat_external = 0x18e710;
    static constexpr uintptr_t mat_destroy = 0x18ed0c;
    static constexpr uintptr_t set_identity = 0x1a1678;
};

template<class T> T entry(uintptr_t base, uintptr_t offset)
{
    return reinterpret_cast<T>(base + offset);
}

// ---- numerical reconstruction of the stock eye post-processing --------------

constexpr unsigned kEyeCount = 2;
constexpr unsigned kChannelsPerEye = 10;
constexpr unsigned kEyeChannels = 20;

enum EyeChannel : unsigned {
    BrowDown = 0,
    BrowInnerUp = 1,
    BrowOuterUp = 2,
    Blink = 3,
    LookDown = 4,
    LookIn = 5,
    LookOut = 6,
    LookUp = 7,
    Squint = 8,
    Wide = 9,
};

float clamp01(float value)
{
    // Stock 0x61c88/0x61f74 use comparisons, not fmin/fmax NaN replacement.
    if (value > 1.0f) value = 1.0f;
    if (value < 0.0f) value = 0.0f;
    return value;
}

float blink_endpoint(float value)
{
    // 0x62568..0x62594 compares float converted to DOUBLE constants.
    // Strict thresholds matter: float(0.04) is below double(0.04), while
    // float(0.8) is above double(0.8).
    if (static_cast<double>(value) < 0.04) value = 0.0f;
    if (static_cast<double>(value) > 0.8) value = 1.0f;
    return value;
}

void postprocess_one(const float input[kChannelsPerEye], float output[kChannelsPerEye])
{
    const float down = input[BrowDown];
    const float up = input[BrowInnerUp];
    float down4 = down * down;
    float up4 = up * up;
    down4 = down * down4;
    up4 = up * up4;
    down4 = down * down4;
    up4 = up * up4;

    // 0x6242c adds the fourth powers as float BEFORE converting to double.
    const double denominator = static_cast<double>(down4 + up4) + 1e-10;
    const double down_weight = static_cast<double>(down4) / denominator;
    const double up_weight = static_cast<double>(up4) / denominator;
    const float processed_down = static_cast<float>(down_weight *
                                                   static_cast<double>(down + up));
    // 0x62458 uses the ALREADY transformed down value; not raw down+up again.
    const float processed_up = static_cast<float>(up_weight *
                                                 static_cast<double>(up + processed_down));
    float linked_up = processed_up;
    if (processed_up < 0.2f) linked_up = input[Wide] * 0.5f + processed_up;
    float linked_down = processed_down;
    if (processed_down < 0.8f) linked_down = input[Blink] * 0.5f + processed_down;

    memcpy(output, input, sizeof(float) * kChannelsPerEye);
    output[BrowDown] = linked_down;
    // Original overwrites outer with the transformed inner, even on one eye.
    output[BrowInnerUp] = linked_up;
    output[BrowOuterUp] = linked_up;
    output[Blink] = blink_endpoint(input[Blink]);
}

float adaptive(float previous, float current)
{
    const float weight = expf(-9.0f * fabsf(previous - current));
    const float complement = 1.0f - weight;
    const float old_part = previous * weight;
    const float new_part = current * complement;
    return old_part + new_part;
}

float motion_weight(const float raw[kChannelsPerEye],
                    const float earlier[kChannelsPerEye])
{
    // 0x62360..0x623c8: replace the mean of two Blink differences by THIS
    // eye's difference, and apply the stock left-direction grouping per eye.
    const float blink = fabsf(raw[Blink] - earlier[Blink]);
    const float down_up = fabsf(raw[LookDown] - earlier[LookDown]) +
                          fabsf(raw[LookUp] - earlier[LookUp]);
    const float in_out = fabsf(raw[LookIn] - earlier[LookIn]) +
                         fabsf(raw[LookOut] - earlier[LookOut]);
    const float directions = (down_up + in_out) * 0.5f;
    return fminf(fmaxf(blink + directions, 0.0f), 1.0f);
}

float composite(float previous, float current, float motion)
{
    // Exact stock constants: current 0x35e450=0x3f19999a, previous
    // 0x360980=0x3ecccccc (NOT rounded C++ literal 0.4f=0x3ecccccd).
    constexpr float current_weight = 0x1.333334p-1f;
    constexpr float previous_weight = 0x1.999998p-2f;
    const float current_part = current * current_weight;
    const float previous_part = previous * previous_weight;
    const float intermediate = current_part + previous_part;
    const float complement = 1.0f - motion;
    const float scaled = intermediate * complement;
    const float old_weight = 1.0f - complement * current_weight;
    const float new_part = scaled * current_weight;
    const float old_part = previous * old_weight;
    return new_part + old_part;
}

struct EyeHistory {
    uint32_t successful_frames = 0; // Saturates at two: stock needs two prior outputs.
    float previous[kChannelsPerEye] {};
    float earlier[kChannelsPerEye] {};
};

struct FilterState {
    EyeHistory eye[kEyeCount];
};

// Each context must own a DIFFERENT stock Kalman object, initialized as stock:
// DP=2, MP=2, CP=0, CV_32F; transition/measurement identity, Q=R=0.1 identity.
// The callback calls stock correct(measurement), THEN predict(empty control),
// and copies the two predicted floats to prediction.
using BrowPredict = void (*)(void *context, const float measurement[2],
                             float prediction[2]);

void filter_one(EyeHistory &history, const float raw[kChannelsPerEye],
                const float postprocessed[kChannelsPerEye],
                float output[kChannelsPerEye], BrowPredict predict, void *context)
{
    float current[kChannelsPerEye];
    for (unsigned i = 0; i < kChannelsPerEye; ++i) current[i] = clamp01(postprocessed[i]);

    if (history.successful_frames < 2) {
        memcpy(output, current, sizeof(current));
    } else {
        const float motion = motion_weight(raw, history.earlier);
        for (unsigned i = BrowDown; i <= BrowOuterUp; ++i)
            output[i] = composite(history.previous[i], current[i], motion);
        // Blink, all four directions, Squint and Wide are stock k=9 slots.
        for (unsigned i = Blink; i < kChannelsPerEye; ++i)
            output[i] = adaptive(history.previous[i], current[i]);
        const float measurement[2] = {output[BrowInnerUp], output[BrowOuterUp]};
        float prediction[2];
        predict(context, measurement, prediction);
        output[BrowInnerUp] = prediction[0];
        output[BrowOuterUp] = prediction[1];
    }

    for (unsigned i = 0; i < kChannelsPerEye; ++i) output[i] = clamp01(output[i]);
    memcpy(history.earlier, history.previous, sizeof(history.earlier));
    memcpy(history.previous, output, sizeof(history.previous));
    if (history.successful_frames < 2) ++history.successful_frames;
}

void postprocess_eye20(const float input[kEyeChannels], float output[kEyeChannels])
{
    float snapshot[kEyeChannels];
    memcpy(snapshot, input, sizeof(snapshot));
    for (unsigned eye = 0; eye < kEyeCount; ++eye) {
        const unsigned offset = eye * kChannelsPerEye;
        postprocess_one(snapshot + offset, output + offset);
    }
}

void filter_eye20(FilterState &state, const float raw[kEyeChannels],
                  const float postprocessed[kEyeChannels],
                  float filtered[kEyeChannels], BrowPredict predict,
                  void *const contexts[kEyeCount])
{
    float raw_snapshot[kEyeChannels], post_snapshot[kEyeChannels];
    memcpy(raw_snapshot, raw, sizeof(raw_snapshot));
    memcpy(post_snapshot, postprocessed, sizeof(post_snapshot));
    for (unsigned eye = 0; eye < kEyeCount; ++eye) {
        const unsigned offset = eye * kChannelsPerEye;
        filter_one(state.eye[eye], raw_snapshot + offset, post_snapshot + offset,
                   filtered + offset, predict, contexts[eye]);
    }
}

float writeback_expression52(const float filtered[kEyeChannels], float expression52[52])
{
    // 0x360b90 reorder of mouth33 followed by eye20. eye[11] is omitted.
    constexpr int slots[kEyeChannels] = {
        16, 3, 30, 28, 0, 2, 44, 31, 26, 47,
        4, -1, 36, 38, 12, 11, 45, 35, 41, 46,
    };
    float snapshot[kEyeChannels];
    memcpy(snapshot, filtered, sizeof(snapshot));
    for (unsigned i = 0; i < kEyeChannels; ++i) {
        if (slots[i] >= 0) expression52[slots[i]] = snapshot[i];
    }
    return snapshot[kChannelsPerEye + BrowInnerUp];
}

// ---- native adapter for the per-eye Kalman filter ---------------------------

using ObjectCall = void (*)(void *);
using KalmanConstruct = void (*)(void *, int, int, int, int);
using KalmanStep = const void *(*)(void *, const void *);
using MatExternal = void (*)(void *, int, int, int, void *, size_t);
struct NativeArray {
    uint32_t flags, padding;
    void *object;
    uint64_t size;
};
using SetIdentity = void (*)(const NativeArray *, const double *);
static_assert(sizeof(NativeArray) == 24, "stock AAPCS64 InputOutputArray layout");

struct NativeMat {
    uintptr_t base;
    alignas(8) unsigned char bytes[StockLayout::mat_bytes];
    explicit NativeMat(uintptr_t plugin_base) : base(plugin_base)
    {
        entry<ObjectCall>(base, StockLayout::mat_construct)(bytes);
    }
    NativeMat(uintptr_t plugin_base, const float *measurement) : base(plugin_base)
    {
        entry<MatExternal>(base, StockLayout::mat_external)(
            bytes, 2, 1, 5, const_cast<float *>(measurement), sizeof(float));
    }
    ~NativeMat() { entry<ObjectCall>(base, StockLayout::mat_destroy)(bytes); }
    NativeMat(const NativeMat &) = delete;
    NativeMat &operator=(const NativeMat &) = delete;
};

class NativeBrowFilter {
public:
    explicit NativeBrowFilter(uintptr_t plugin_base);
    ~NativeBrowFilter();
    NativeBrowFilter(const NativeBrowFilter &) = delete;
    NativeBrowFilter &operator=(const NativeBrowFilter &) = delete;
    static void predict(void *context, const float measurement[2], float prediction[2]);

private:
    uintptr_t base_;
    alignas(8) unsigned char kalman_[StockLayout::kalman_bytes];
    void destroy();
};

NativeBrowFilter::NativeBrowFilter(uintptr_t plugin_base) : base_(plugin_base)
{
    // Stock 0x5ff80..0x5ff94 constructs sixteen 0x60-byte Mats in Kalman.
    // Its constructor handles failures before completion; after it completes,
    // our ownership mirrors the stock reverse-order Mat destruction.
    entry<KalmanConstruct>(base_, StockLayout::kalman_construct)(kalman_, 2, 2, 0, 5);
    try {
        const uintptr_t matrices[] = {
            StockLayout::kalman_transition, StockLayout::kalman_measurement,
            StockLayout::kalman_process_noise, StockLayout::kalman_measurement_noise,
        };
        for (unsigned i = 0; i < 4; ++i) {
            const NativeArray target = {0x3010000, 0, kalman_ + matrices[i], 0};
            const double scalar[4] = {i < 2 ? 1.0 : 0.1, 0.0, 0.0, 0.0};
            entry<SetIdentity>(base_, StockLayout::set_identity)(&target, scalar);
        }
    } catch (...) {
        destroy();
        throw;
    }
}

void NativeBrowFilter::destroy()
{
    for (uintptr_t offset = StockLayout::kalman_bytes; offset != 0;) {
        offset -= StockLayout::mat_bytes;
        entry<ObjectCall>(base_, StockLayout::mat_destroy)(kalman_ + offset);
    }
}

NativeBrowFilter::~NativeBrowFilter()
{
    destroy();
}

void NativeBrowFilter::predict(void *context, const float measurement[2], float prediction[2])
{
    auto &filter = *static_cast<NativeBrowFilter *>(context);
    NativeMat input(filter.base_, measurement);
    NativeMat empty_control(filter.base_);
    entry<KalmanStep>(filter.base_, StockLayout::kalman_correct)(filter.kalman_, input.bytes);
    const void *result = entry<KalmanStep>(filter.base_, StockLayout::kalman_predict)(
        filter.kalman_, empty_control.bytes);
    const float *data;
    memcpy(&data, static_cast<const unsigned char *>(result) + 0x10, sizeof(data));
    memcpy(prediction, data, 2 * sizeof(float));
}

// ---- per-eye quality, reference images and owner state -----------------------

// Each State belongs to one freshly constructed stock FaceTrackor. The caller
// serializes that owner's calls and removes the State when the owner is destroyed.
struct EyeState {
    int32_t classification = -1;
    uint64_t window_samples = 0;
    int32_t window_good = 0;
    bool reference_present = false;
    uint64_t reference_generation = 0;
    uint64_t reference_update = 0;
    uint64_t successful_updates = 0;
};

// Replays only the stock shared-reference event that resets mouth quality.
// Neither independent eye's quality, reference, or numerical output reads it.
struct MouthQualityCompatibility {
    std::deque<uint8_t> eye_samples[2];
    int32_t eye_good[2] {};
    bool shared_reference_present = false;
};

struct State {
    EyeState eyes[2];
    const void *frame_current[2] {};
    MouthQualityCompatibility mouth_compatibility;
};

struct Validity {
    bool eye[2];
    float combined;
};

constexpr size_t kReferenceOffsets[2] = {0x8, 0x68};
constexpr size_t kQueueOffsets[2] = {0x120, 0x150};
constexpr size_t kCountOffsets[2] = {0x110, 0x114};

// Reproduces the _OutputArray built at 0x62b90..0x62ba8 / 0x62bac..0x62bc0.
// It refers to a stock-constructed Mat; it is not a memcpy of the Mat header.
struct MatOutput {
    int32_t flags;
    int32_t padding;
    void *object;
    uint64_t size;
};
static_assert(sizeof(MatOutput) == 0x18, "stock AArch64 OutputArray ABI");
static_assert(offsetof(MatOutput, object) == 8, "stock OutputArray object offset");

void copy_reference(uintptr_t base, void *destination, const void *current)
{
    MatOutput output {0x02010000, 0, destination, 0};
    using CopyTo = void (*)(const void *, const MatOutput *);
    reinterpret_cast<CopyTo>(base + 0x12953c)(current, &output);
}

int32_t advance_legacy_quality(std::deque<uint8_t> &samples, int32_t &good,
                               uint64_t window, int32_t threshold, float probability)
{
    // Stock 0x62810..0x62904 removes exactly ONE oldest sample if size >= window.
    // The count subtraction is skipped when the current good count is zero.
    if (samples.size() >= window) {
        if (good > 0) good -= samples.front();
        samples.pop_front();
    }
    // 0x62974 loads float bits 0x3ecccccd; cset gt is a strict comparison.
    const uint8_t sample = probability > 0.4f;
    samples.push_back(sample);
    good += sample;
    if (good > threshold) return 1;
    // Match the unsigned false-count comparison at 0x62a28/0x62a4c.
    const uint64_t bad = samples.size() - static_cast<uint64_t>(int64_t(good));
    return bad > static_cast<uint64_t>(int64_t(threshold)) ? 0 : -1;
}

void begin_reference_frame(State &state)
{
    state.frame_current[0] = nullptr;
    state.frame_current[1] = nullptr;
}

// Call after a normally completed trackor call. Reads the stock per-eye quality
// queues/counters; does not advance or clear them.
Validity observe_quality(State &state, const void *owner)
{
    const int32_t threshold = load<int32_t>(owner, 0x104);
    Validity validity {};
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto &value = state.eyes[eye];
        value.window_samples = load<uint64_t>(owner, kQueueOffsets[eye] + 0x28);
        value.window_good = load<int32_t>(owner, kCountOffsets[eye]);
        const int64_t bad = static_cast<int64_t>(value.window_samples) - value.window_good;
        value.classification = value.window_good > threshold ? 1 : bad > threshold ? 0 : -1;
        ++value.successful_updates;
        validity.eye[eye] = value.classification != 0;
    }
    // The stock 0xd8 return has only one eye-group validity float. Preserve its
    // OR policy while exposing both local decisions separately to the caller.
    validity.combined = (validity.eye[0] || validity.eye[1]) ? 1.0f : 0.0f;
    return validity;
}

// For the 0x62f90 hook's second Mat argument. eye is ABI input order, not a
// separately verified physical left/right identity. current is its first Mat.
const void *select_reference(State &state, void *owner, unsigned eye,
                             const void *current)
{
    state.frame_current[eye] = current;
    return state.eyes[eye].reference_present
        ? static_cast<unsigned char *>(owner) + kReferenceOffsets[eye]
        : current;
}

uint32_t finish_references(State &state, uintptr_t base, void *owner,
                           const void *eye0, const void *eye1)
{
    const void *current[2] = {eye0, eye1};
    uint32_t captured = 0;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto &value = state.eyes[eye];
        if (value.classification == 0) {
            value.reference_present = false;
        } else if (value.classification == 1 && !value.reference_present) {
            copy_reference(base, static_cast<unsigned char *>(owner) + kReferenceOffsets[eye],
                           current[eye]);
            using ClearQueue = void (*)(void *);
            reinterpret_cast<ClearQueue>(base + 0x5e7a0)(
                static_cast<unsigned char *>(owner) + kQueueOffsets[eye]);
            store<int32_t>(owner, kCountOffsets[eye], 0);
            value.reference_present = true;
            ++value.reference_generation;
            value.reference_update = value.successful_updates;
            captured |= uint32_t(1) << eye;
        }
    }
    // Force stock's fallback path; select_reference supplies the independent
    // second channel. A single stock byte cannot encode both reference states.
    store<uint8_t>(owner, 1, 0);
    return captured;
}

// Once per normally completed frame, selects each eye's reference from this
// frame's own current Mat.
uint32_t finish_references(State &state, uintptr_t base, void *owner)
{
    return finish_references(state, base, owner, state.frame_current[0], state.frame_current[1]);
}

// Once per normally completed frame, AFTER observe_quality/finish_references
// and before the next stock trackor call. raw24 is the captured infer result,
// including quality probabilities at 20/22. Failed frames must not call this.
bool preserve_mouth_quality_reset(State &state, uintptr_t base, void *owner,
                                  const float raw24[24])
{
    auto &legacy = state.mouth_compatibility;
    const uint64_t window = static_cast<uint64_t>(int64_t(load<int32_t>(owner, 0x100)));
    const int32_t threshold = load<int32_t>(owner, 0x104);
    int32_t classification[2];
    for (unsigned eye = 0; eye < 2; ++eye) {
        classification[eye] = advance_legacy_quality(legacy.eye_samples[eye], legacy.eye_good[eye],
                                                     window, threshold, raw24[20 + 2 * eye]);
    }
    // Stock 0x62b70..0x62b8c: both 0 clears the legacy shared flag; otherwise
    // only an explicit 1 with no existing reference triggers queue resets.
    if (classification[0] == 0 && classification[1] == 0) {
        legacy.shared_reference_present = false;
        return false;
    }
    if ((classification[0] == 1 || classification[1] == 1) && !legacy.shared_reference_present) {
        // 0x62bcc zeroes the mouth count, then 0x62be0 clears its stock deque.
        store<int32_t>(owner, 0x118, 0);
        using ClearQueue = void (*)(void *);
        reinterpret_cast<ClearQueue>(base + 0x5e7a0)(static_cast<unsigned char *>(owner) + 0x180);
        for (unsigned eye = 0; eye < 2; ++eye) {
            legacy.eye_samples[eye].clear();
            legacy.eye_good[eye] = 0;
        }
        legacy.shared_reference_present = true;
        return true;
    }
    return false;
}

// ---- frame context, registry and the expression hooks ------------------------

uintptr_t g_expr_plugin = 0;
std::atomic<bool> g_expr_ready {false};

struct OwnerState {
    State reference;
    FilterState filter {};
    NativeBrowFilter left, right;
    explicit OwnerState(uintptr_t base) : left(base), right(base) {}
};

// Function-local statics on purpose: installed hooks can run on another service
// thread before this translation unit's dynamic initializers, so first use must
// construct the registry and its lock.
std::mutex &registry_mutex()
{
    static std::mutex value;
    return value;
}

std::unordered_map<void *, std::unique_ptr<OwnerState>> &owner_registry()
{
    static std::unordered_map<void *, std::unique_ptr<OwnerState>> value;
    return value;
}

struct Frame {
    void *owner = nullptr;
    OwnerState *state = nullptr;
    unsigned raw_calls = 0, merge_calls[2] {};
    size_t raw_length = 0;
    bool completion_seen = false, completed = false, pending_export = false;
    int32_t export_state[2] {};
    float raw[24] {}, post[20] {}, filtered[20] {}, final[52] {};
    float right_inner = 0;
};
thread_local Frame g_frame;

using MergeFn = void (*)(const void *, const void *, void *);
using DestroyFn = void (*)(void *);
MergeFn g_original_merge = nullptr;
DestroyFn g_original_destroy = nullptr;

void reset_owner(void *owner);

void forget_owner(void *owner)
{
    if (g_frame.owner == owner) g_frame = Frame {};
    std::lock_guard<std::mutex> lock(registry_mutex());
    owner_registry().erase(owner);
}

void reset_owner(void *owner)
{
    forget_owner(owner);
}

void begin_frame(void *owner)
{
    g_frame = Frame {};
    if (!g_expr_ready.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(registry_mutex());
    auto &state = owner_registry()[owner];
    if (!state) state = std::make_unique<OwnerState>(g_expr_plugin);
    g_frame = Frame {};
    g_frame.owner = owner;
    g_frame.state = state.get();
    begin_reference_frame(state->reference);
}

void observe_infer(void *engine, const float *values, size_t count)
{
    if (!g_frame.state || engine != load<void *>(g_frame.owner, 0x880)) return;
    ++g_frame.raw_calls;
    g_frame.raw_length = count;
    if (count == 24) memcpy(g_frame.raw, values, sizeof(g_frame.raw));
}

void observe_completion(bool completed)
{
    if (!g_frame.state) return;
    g_frame.completion_seen = true;
    g_frame.completed = completed;
}

void end_frame(float *output)
{
    if (!g_frame.state) return;
    if (!g_frame.completion_seen || !g_frame.completed || g_frame.raw_calls != 1 ||
        g_frame.raw_length != 24 || g_frame.merge_calls[0] != 1 || g_frame.merge_calls[1] != 1) {
        output[52] = 0;
        // Keep the explicit expression capability on a failed live frame so the
        // receiver holds each eye's own last valid value instead of falling back
        // to the fused source.
        memcpy(g_frame.final, output, sizeof(g_frame.final));
        g_frame.pending_export = true;
        return;
    }
    postprocess_eye20(g_frame.raw, g_frame.post);
    void *contexts[2] = {&g_frame.state->left, &g_frame.state->right};
    filter_eye20(g_frame.state->filter, g_frame.raw, g_frame.post, g_frame.filtered,
                 NativeBrowFilter::predict, contexts);
    g_frame.right_inner = writeback_expression52(g_frame.filtered, output);
    const Validity validity = observe_quality(g_frame.state->reference, g_frame.owner);
    output[52] = validity.combined;
    (void)finish_references(g_frame.state->reference, g_expr_plugin, g_frame.owner);
    (void)preserve_mouth_quality_reset(g_frame.state->reference, g_expr_plugin, g_frame.owner,
                                       g_frame.raw);
    memcpy(g_frame.final, output, sizeof(g_frame.final));
    for (unsigned i = 0; i < 2; ++i)
        g_frame.export_state[i] = g_frame.state->reference.eyes[i].classification;
    g_frame.pending_export = true;
}

__attribute__((noinline)) void merge_proxy(const void *current, const void *reference,
                                           void *result)
{
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - g_expr_plugin;
    if (g_frame.state) {
        int eye = (caller == 0x626dc || caller == 0x62cb0) ? 0 :
                  (caller == 0x626ec || caller == 0x62cc0) ? 1 : -1;
        if (eye >= 0) {
            ++g_frame.merge_calls[eye];
            reference = select_reference(g_frame.state->reference, g_frame.owner, unsigned(eye), current);
        }
    }
    g_original_merge(current, reference, result);
}

void export_prefix(shadowhook_cpu_context_t *cpu, void *)
{
    if (!g_frame.pending_export) return;
    void *prefix = reinterpret_cast<void *>(cpu->regs[0]);
    // Match the same call's standard output before attaching sidecar fields.
    if (memcmp(static_cast<unsigned char *>(prefix) + 8, g_frame.final, sizeof(g_frame.final))) return;
    store<float>(prefix, 216, g_frame.right_inner);
    store<int32_t>(prefix, 220, g_frame.export_state[0]);
    store<int32_t>(prefix, 224, g_frame.export_state[1]);
    store<uint32_t>(prefix, 228, 0x31495045); // EPI1
    store<uint32_t>(prefix, 232, 1);
    g_frame.pending_export = false; // This pending callback identity is consumed once.
}

void destroy_proxy(void *owner)
{
    forget_owner(owner);
    g_original_destroy(owner);
}

// ---- outer proxies on the tracked function entry points ----------------------

infer_fn_t g_original_infer = nullptr;
trackor_fn_t g_original_trackor = nullptr;
init_fn_t g_original_init = nullptr;

__attribute__((noinline)) expr_vec infer_proxy(void *a0, void *engine, const expr_vec *images)
{
    expr_vec result = g_original_infer(a0, engine, images);
    size_t bytes = uintptr_t(result.end) - uintptr_t(result.begin);
    observe_infer(engine, static_cast<const float *>(result.begin), bytes / sizeof(float));
    return result; // The stock caller still owns and frees exactly the original vector.
}

__attribute__((noinline)) expr_output trackor_proxy(void *owner, const expr_mat *eye0,
                                                    const expr_mat *eye1, const expr_mat *mouth)
{
    begin_frame(owner);
    expr_output result = g_original_trackor(owner, eye0, eye1, mouth);
    end_frame(result.values);
    return result;
}

void initialization_proxy(void *owner)
{
    if (!load<unsigned char>(owner, 0)) reset_owner(owner);
    g_original_init(owner);
}

void normal_exit(shadowhook_cpu_context_t *, void *)
{
    observe_completion(true);
}

void exception_exit(shadowhook_cpu_context_t *, void *)
{
    observe_completion(false);
}

// ---- installation: guards, hooks, patches and rollback -----------------------

bool guard_word(uintptr_t base, size_t offset, uint32_t expected)
{
    const uint32_t got = load<uint32_t>(reinterpret_cast<const void *>(base + offset), 0);
    log_fmt("guard %s word site=0x%zx got=0x%08x expected=0x%08x",
            got == expected ? "ok" : "failed", offset, got, expected);
    return got == expected;
}

// FNV-1a compatibility anchors for the supported firmware, not PICO code.
bool guard_windows(uintptr_t base)
{
    static const struct { size_t offset, length; uint64_t expected; } windows[] = {
        {0x605ac, 0x1f0, 0x31995bf748f7c422ULL},
        {0x613e0, 0x2a0, 0xb67cb8dc0066e98dULL},
        {0x625b4, 0x9dc, 0x0379770f754e251fULL},
    };
    for (const auto &w : windows) {
        const uint64_t got =
            fnv1a64_mem(reinterpret_cast<const unsigned char *>(base + w.offset), w.length);
        log_fmt("guard %s window site=0x%zx length=0x%zx got=0x%016llX expected=0x%016llX",
                got == w.expected ? "ok" : "failed", w.offset, w.length,
                (unsigned long long)got, (unsigned long long)w.expected);
        if (got != w.expected) return false;
    }
    return true;
}

bool guard_init_head(uintptr_t base)
{
    static const uint32_t head[] = {0xd10403ff, 0xf90063f7, 0xa90d57f6, 0xa90e4ff4};
    for (unsigned i = 0; i < 4; ++i) {
        if (!guard_word(base, 0x60998 + 4 * size_t(i), head[i])) return false;
    }
    return true;
}

void *install_function(uintptr_t base, size_t offset, const char *stage, void *proxy, void **original)
{
    void *hook = shadowhook_hook_func_addr(reinterpret_cast<void *>(base + offset), proxy, original);
    int error = shadowhook_get_errno();
    log_fmt("hook stage=%s site=0x%zx address=%p handle=%p trampoline=%p errno=%d message=%s",
            stage, offset, reinterpret_cast<void *>(base + offset), hook, *original, error,
            shadowhook_to_errmsg(error));
    return hook;
}

// Intercept one instruction. ShadowHook must rewrite exactly four bytes; if the
// 20 bytes after the site changed, the intercept is rejected and rolled back.
void *intercept_one_instruction(uintptr_t base, size_t offset, const char *stage,
                                shadowhook_interceptor_t callback)
{
    auto *site = reinterpret_cast<unsigned char *>(base + offset);
    unsigned char following[20];
    uint32_t original_instruction = load<uint32_t>(site, 0);
    memcpy(following, site + 4, sizeof(following));
    void *hook = shadowhook_intercept_instr_addr(site, callback, nullptr,
                                                SHADOWHOOK_INTERCEPT_WITH_FPSIMD_READ_WRITE);
    int error = shadowhook_get_errno();
    if (hook && memcmp(following, site + 4, sizeof(following))) {
        char expected[41], got[41];
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < 20; ++i) {
            expected[2 * i] = hex[following[i] >> 4];
            expected[2 * i + 1] = hex[following[i] & 15];
            got[2 * i] = hex[site[4 + i] >> 4];
            got[2 * i + 1] = hex[site[4 + i] & 15];
        }
        expected[40] = got[40] = 0;
        log_fmt("intercept rejected stage=%s site=0x%zx address=%p required=4-byte "
                "original_instruction=0x%08x patched_instruction=0x%08x following_expected=%s following_got=%s",
                stage, offset, site, original_instruction, load<uint32_t>(site, 0), expected, got);
        int rc = shadowhook_unintercept(hook);
        int cleanup_error = shadowhook_get_errno();
        log_fmt("rollback rejected-intercept site=0x%zx rc=%d errno=%d message=%s",
                offset, rc, cleanup_error, shadowhook_to_errmsg(cleanup_error));
        return nullptr;
    }
    if (hook)
        log_fmt("intercept accepted stage=%s site=0x%zx rewrite=4-byte original_instruction=0x%08x",
                stage, offset, original_instruction);
    else
        log_fmt("intercept failed stage=%s site=0x%zx address=%p errno=%d message=%s",
                stage, offset, site, error, shadowhook_to_errmsg(error));
    return hook;
}

bool patch_word(uintptr_t base, size_t offset, uint32_t word)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0)
        pagesz = 4096;
    const size_t size = static_cast<size_t>(pagesz);
    void *page = reinterpret_cast<void *>((base + offset) & ~(static_cast<uintptr_t>(size) - 1));
    if (mprotect(page, size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        log_fmt("patch: mprotect RWX failed site=0x%zx errno=%d", offset, errno);
        return false;
    }
    store<uint32_t>(reinterpret_cast<void *>(base + offset), 0, word);
    __builtin___clear_cache(reinterpret_cast<char *>(base + offset),
                            reinterpret_cast<char *>(base + offset + 4));
    if (mprotect(page, size, PROT_READ | PROT_EXEC)) {
        log_fmt("patch: restore prot failed site=0x%zx errno=%d", offset, errno);
        return false;
    }
    return true;
}

void rollback_hook(void *hook, const char *stage, size_t offset, bool instruction)
{
    if (!hook) return;
    int rc = instruction ? shadowhook_unintercept(hook) : shadowhook_unhook(hook);
    int error = shadowhook_get_errno();
    log_fmt("rollback stage=%s site=0x%zx rc=%d errno=%d message=%s", stage, offset, rc, error,
            shadowhook_to_errmsg(error));
}

// The r4 hook set: two single-instruction bypasses, the per-eye reference merge,
// the owner destroy hook and the EPI1 export callback. Everything rolls back on
// failure; g_expr_ready flips only after the whole set is in place.
int install_expression(uintptr_t base)
{
    static const struct { size_t offset; uint32_t word; } sites[] = {
        {kOffMerge, kWordMerge}, {kOffExport, kWordExport}, {kOffBlinkCopyPatch, kWordBlinkCopy},
        {kOffSharedRefPatch, kWordSharedRef}, {kOffDestroy, kWordDestroy},
    };
    for (const auto &site : sites) {
        if (!guard_word(base, site.offset, site.word)) return -1;
    }
    g_expr_plugin = base;

    g_original_merge = nullptr;
    void *merge_hook = install_function(base, kOffMerge, "merge", reinterpret_cast<void *>(merge_proxy),
                                        reinterpret_cast<void **>(&g_original_merge));
    if (!merge_hook || !g_original_merge) return -1;

    g_original_destroy = nullptr;
    void *destroy_hook = install_function(base, kOffDestroy, "destroy", reinterpret_cast<void *>(destroy_proxy),
                                          reinterpret_cast<void **>(&g_original_destroy));
    if (!destroy_hook || !g_original_destroy) {
        shadowhook_unhook(merge_hook);
        return -1;
    }

    void *export_hook = intercept_one_instruction(base, kOffExport, "export", export_prefix);
    if (!export_hook) {
        shadowhook_unhook(destroy_hook);
        shadowhook_unhook(merge_hook);
        return -1;
    }

    if (!patch_word(base, kOffBlinkCopyPatch, kPatchBlinkCopy) ||
        !patch_word(base, kOffSharedRefPatch, kPatchSharedRef)) {
        patch_word(base, kOffBlinkCopyPatch, kWordBlinkCopy);
        patch_word(base, kOffSharedRefPatch, kWordSharedRef);
        shadowhook_unintercept(export_hook);
        shadowhook_unhook(destroy_hook);
        shadowhook_unhook(merge_hook);
        return -1;
    }
    g_expr_ready.store(true, std::memory_order_release);
    append_log("expression independent: reference merge, owner lifecycle and EPI1 export installed; "
               "blink-copy and shared-reference bypasses patched");
    return 0;
}

// Full plugin installation: guards first, then the outer entry hooks, then the
// expression hook set. Any failure rolls back everything installed so far.
int install_plugin(uintptr_t base)
{
    log_fmt("install begin pid=%ld base=%p version=%s", (long)getpid(),
            reinterpret_cast<void *>(base), kExpressionVersion);
    if (!guard_windows(base) || !guard_init_head(base)) {
        append_log("plugin guard mismatch; no hooks installed");
        return -1;
    }

    g_original_infer = nullptr;
    g_original_trackor = nullptr;
    g_original_init = nullptr;
    void *infer_hook = install_function(base, kOffInfer, "infer", reinterpret_cast<void *>(infer_proxy),
                                        reinterpret_cast<void **>(&g_original_infer));
    void *trackor_hook = nullptr, *init_hook = nullptr, *normal_hook = nullptr, *exception_hook = nullptr;
    if (infer_hook)
        trackor_hook = install_function(base, kOffTrackor, "trackor", reinterpret_cast<void *>(trackor_proxy),
                                        reinterpret_cast<void **>(&g_original_trackor));
    if (trackor_hook)
        init_hook = install_function(base, kOffOwnerInit, "owner-init", reinterpret_cast<void *>(initialization_proxy),
                                     reinterpret_cast<void **>(&g_original_init));
    if (init_hook) normal_hook = intercept_one_instruction(base, kOffNormalExit, "normal-exit", normal_exit);
    if (normal_hook) exception_hook = intercept_one_instruction(base, kOffCatchExit, "catch-exit", exception_exit);
    if (exception_hook && install_expression(base) == 0) {
        append_log("install complete; expression independent hooks ready");
        return 0;
    }

    append_log("plugin install failed; rolling back installed hooks and patches; service restart required");
    rollback_hook(exception_hook, "catch-exit", kOffCatchExit, true);
    rollback_hook(normal_hook, "normal-exit", kOffNormalExit, true);
    rollback_hook(init_hook, "owner-init", kOffOwnerInit, false);
    rollback_hook(trackor_hook, "trackor", kOffTrackor, false);
    rollback_hook(infer_hook, "infer", kOffInfer, false);
    return -1;
}

// ---- mode, plugin hand-off and bootstrap -------------------------------------

/* /data/local/tmp/picoet-mode: "dual" installs the expression hook set,
 * "off" or a missing/unreadable file keeps the gate hook only. */
int read_enhanced_mode(void)
{
    int fd = open(kEyeModeFile, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        log_fmt("mode: open %s failed errno=%d; gate only", kEyeModeFile, errno);
        return 0;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int read_errno = errno;
    close(fd);
    if (n <= 0) {
        log_fmt("mode: empty/unreadable file (n=%ld errno=%d); gate only", (long)n, read_errno);
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
    if (strcmp(buf, "dual") == 0)
        return 1;
    if (strcmp(buf, "off") != 0)
        log_fmt("mode: unrecognized value '%s'; gate only", buf);
    return 0;
}

void on_plugin_found(void *base)
{
    picoet_plugin_base = (unsigned long long)base;
    if (install_plugin(reinterpret_cast<uintptr_t>(base)) != 0) {
        report_runtime("failed");
        return;
    }
    // dual means the gate and the expression hooks are both in place.
    if (__atomic_load_n(&picoet_gate_installed, __ATOMIC_ACQUIRE))
        __atomic_store_n(&picoet_dual_installed, 1, __ATOMIC_RELEASE);
    report_runtime("ready");
}

void *plugin_poll_thread(void *)
{
    for (int i = 0; i < 300; i++) { /* up to 60 s */
        void *b = find_phoenix();
        if (b) {
            on_plugin_found(b);
            return nullptr;
        }
        usleep(200 * 1000);
    }
    append_log("plugin poll: not found after 60s");
    report_runtime("failed");
    return nullptr;
}

__attribute__((constructor)) void picoet_init(void)
{
    static volatile int once = 0;
    if (once)
        return;
    once = 1;

    picoet_hook_marker = 0x5049434F45543150ULL; /* "PICOET1P" */
    g_start_ticks = process_start_ticks();
    g_enhanced_mode = read_enhanced_mode();
    g_gate_requested = read_gate_enabled();
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
    if (!g_enhanced_mode) {
        report_runtime("ready");
        return;
    }
    report_runtime("pending");

    // Expression independence: find the plugin, then install the full hook set.
    void *b = find_phoenix();
    if (b) {
        on_plugin_found(b);
    } else {
        append_log("plugin not loaded yet; starting poll thread");
        pthread_t t;
        if (pthread_create(&t, NULL, plugin_poll_thread, NULL) == 0)
            pthread_detach(t);
        else {
            append_log("plugin poll thread creation failed");
            report_runtime("failed");
        }
    }
}

} // namespace
