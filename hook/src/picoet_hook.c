// Runtime payload for pxreyetrackingservice: service-local P1 and independent
// eye/mouth expressions at startup-configured source-clock rates.
// PICO code is addressed only at runtime behind firmware guards, never shipped.
// C++17; merge_proxy requires -fno-omit-frame-pointer.
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
#include <sys/mman.h>
#include <atomic>
#include <cstring>
#include "shadowhook.h"
#include "expression/high_rate.h"
#include "expression/integration.h"
#include "root_file.h"

extern "C" {
__attribute__((visibility("default"))) unsigned long long picoet_hook_marker = 0;
__attribute__((visibility("default"))) unsigned long long picoet_gate_calls = 0;
__attribute__((visibility("default"))) unsigned long long picoet_gate_installed = 0;
__attribute__((visibility("default"))) unsigned long long picoet_plugin_base = 0;
__attribute__((visibility("default"))) unsigned long long picoet_dual_installed = 0;
}

namespace {
constexpr const char kServiceLibPath[] = "/system/lib64/libpxreyetrackingservice.so";
constexpr const char kServiceLibSoname[] = "libpxreyetrackingservice.so";
constexpr uint64_t kServiceFnv1a64 = 0xC51A2999AF634D51ULL;
constexpr const char kGateSym[] = "_ZN3PXR7EyeUtil13isBusinessDevEv";
constexpr const char kEyeModeFile[] = "/data/local/tmp/picoet-mode";
constexpr const char kGateFile[] = "/data/local/tmp/picoet-gate";
constexpr const char kEyeHzFile[] = "/data/local/tmp/picoet-eye-hz";
constexpr const char kMouthHzFile[] = "/data/local/tmp/picoet-mouth-hz";
constexpr const char kHookLog[] = "/data/local/tmp/picoet-hook.log";
constexpr const char kPluginSoname[] = "libpxreyetracking.phoenix.so";
constexpr uint64_t kPluginMinLoadBytes = 0x34279c;
constexpr size_t kOffInfer = 0x613e0, kOffTrackor = 0x625b4, kOffOwnerInit = 0x60998;
constexpr size_t kOffPipeline = 0x605ac, kOffNormalExit = 0x62c6c, kOffCatchExit = 0x62f70;
constexpr size_t kOffMerge = 0x62f90, kOffDestroy = 0x6016c, kOffExport = 0x60754;
constexpr const char kExpressionVersion[] = "expression-dual-rate-source-dt-effective-time-r3-profile0";
unsigned long long g_start_ticks;
int g_enhanced_mode = 0, g_gate_requested = 1;
expression_high_rate::RateConfig g_rates;
uintptr_t g_expr_plugin;
std::atomic<bool> g_expr_ready {false};

void append_log(const char* msg)
{
    int fd = picoet_open_root_file(kHookLog, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ssize_t written = write(fd, msg, strlen(msg));
    (void)written;
    written = write(fd, "\n", 1);
    (void)written;
    close(fd);
}
void log_fmt(const char* fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    append_log(buf);
}
template<class T> T load(const void* object, size_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
    return value;
}

// Atomic PID/starttime-bound state publication is shared with the Magisk controls.
unsigned long long process_start_ticks()
{
    char buf[2048];
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char* p = strrchr(buf, ')');
    if (!p || p[1] != ' ') return 0;
    p += 2;
    for (int field = 3; field < 22; field++) {
        p = strchr(p, ' ');
        if (!p) return 0;
        while (*p == ' ') p++;
    }
    char* end;
    errno = 0;
    unsigned long long ticks = strtoull(p, &end, 10);
    return errno || end == p || *end != ' ' ? 0 : ticks;
}
int publish_record(const char* name, const char* record, size_t len)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%ld.tmp", (long)getpid());
    int dir = picoet_open_runtime();
    if (dir < 0) return -1;
    int fd = picoet_root_file(openat(dir, tmp,
        O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (fd < 0) { close(dir); return -1; }
    if (ftruncate(fd, 0) != 0) { close(fd); close(dir); return -1; }
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, record + done, (size_t)len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    int rc = close(fd);
    if (done == len && rc == 0 && renameat(dir, tmp, dir, name) == 0) {
        close(dir);
        return 0;
    }
    unlinkat(dir, tmp, 0);
    close(dir);
    return -1;
}
int publish_runtime(const char* phase)
{
    char name[64], record[160];
    const char* mode = __atomic_load_n(&picoet_dual_installed, __ATOMIC_RELAXED) ? "dual" : "off";
    snprintf(name, sizeof(name), "%ld.state", (long)getpid());
    int len = snprintf(record, sizeof(record), "2 %ld %llu %s %s %s %u %u\n",
                       (long)getpid(), g_start_ticks, phase, mode,
                       picoet_gate_installed ? "on" : "off", g_rates.eye_hz, g_rates.mouth_hz);
    if (!g_start_ticks || len <= 0 || (size_t)len >= sizeof(record)) return -1;
    return publish_record(name, record, static_cast<size_t>(len));
}
void report_runtime(const char* phase)
{
    if (publish_runtime(phase) != 0)
        log_fmt("runtime state: cannot publish %s errno=%d", phase, errno);
}
int fnv1a64_file(const char* path, uint64_t* out)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    static unsigned char buf[65536];
    uint64_t h = 14695981039346656037ULL;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
    }
    close(fd);
    *out = h;
    return 0;
}
uint64_t fnv1a64_mem(const unsigned char* p, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// ---- unchanged service-local P1 gate ---------------------------------------
int read_gate_enabled()
{
    int fd = picoet_open_root_file(kGateFile, O_RDONLY, 0);
    if (fd < 0) return 1;
    char buf[8];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 1;
    buf[n] = '\0';
    return strncmp(buf, "off", 3) != 0;
}
int my_isBusinessDev()
{
    unsigned long long n = __atomic_add_fetch(&picoet_gate_calls, 1, __ATOMIC_RELAXED);
    if (n <= 3 || (n % 1000) == 0) log_fmt("P1 gate: isBusinessDev call #%llu -> 1", n);
    return 1;
}
int install_gate_hook()
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
    void* orig = nullptr;
    void* stub = shadowhook_hook_sym_name(kServiceLibSoname, kGateSym,
                                          (void*)my_isBusinessDev, &orig);
    if (!stub || !orig) {
        log_fmt("P1: gate hook failed errno=%d", shadowhook_get_errno());
        return -1;
    }
    __atomic_store_n(&picoet_gate_installed, 1, __ATOMIC_RELEASE);
    append_log("P1 gate hook installed (isBusinessDev -> 1)");
    return 0;
}
int phoenix_cb(struct dl_phdr_info* info, size_t, void* opaque)
{
    void** base = static_cast<void**>(opaque);
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    const char* bn = strrchr(info->dlpi_name, '/');
    bn = bn ? bn + 1 : info->dlpi_name;
    if (strcmp(bn, kPluginSoname) != 0) return 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const auto& p = info->dlpi_phdr[i];
        if (p.p_type == PT_LOAD && p.p_flags == (PF_R | PF_X) && p.p_vaddr == 0 &&
            p.p_memsz >= kPluginMinLoadBytes) *base = (void*)info->dlpi_addr;
    }
    return 0;
}
void* find_phoenix()
{
    void* base = nullptr;
    dl_iterate_phdr(phoenix_cb, &base);
    return base;
}

// ---- guarded FaceTrackor initialization: first builder profile 6 -> 0 -------
// No global SNPE setter hook. Runtime, execution priority, retries and native ET
// are unchanged. The scope exists only for an uninitialized FaceTrackor owner.
struct SnpeBuild {
    bool entered = false, returned = false;
    void *slot = nullptr, *result = nullptr;
    unsigned profile_calls = 0;
};
struct SnpeEngine {
    void *engine = nullptr, *builders[2] {}, *setter_return = nullptr;
    int runtime[2] {-1, -1}, priority = -1, original_profile = -1, forwarded_profile = -1;
    unsigned setter_calls = 0, overrides = 0;
    bool setter_returned = false;
    SnpeBuild builds[2];
};
struct SnpeInit {
    void* owner = nullptr;
    bool returned = false, boundary_unknown = false;
    int role = -1;
    SnpeEngine engines[2];
};
thread_local SnpeInit* snpe_initializing;
std::atomic<bool> snpe_hooks_ready {false};
class SnpeInitializationScope {
    SnpeInit trace;
    SnpeInit* previous;
    bool enabled;
public:
    explicit SnpeInitializationScope(void* owner) : previous(snpe_initializing),
        enabled(snpe_hooks_ready.load(std::memory_order_acquire) && !load<unsigned char>(owner, 0)) {
        snpe_initializing = nullptr;
        if (enabled) { trace.owner = owner; snpe_initializing = &trace; }
    }
    void returned() { trace.returned = true; }
    ~SnpeInitializationScope() {
        snpe_initializing = previous;
        if (!enabled) return;
        for (unsigned role = 0; role < 2; ++role) {
            const auto& e = trace.engines[role];
            log_fmt("snpe-init owner=%p role=%s init_returned=%u owner_flag=%u boundary_unknown=%u "
                    "runtime_first=%d runtime_retry=%d priority_first=%d profile_original=%d "
                    "profile_forwarded=%d overrides=%u first_nonnull=%u retry_entered=%u retry_nonnull=%u",
                    trace.owner, role ? "mouth" : "eye", unsigned(trace.returned),
                    unsigned(load<unsigned char>(trace.owner, 0)), unsigned(trace.boundary_unknown),
                    e.runtime[0], e.runtime[1], e.priority, e.original_profile, e.forwarded_profile,
                    e.overrides, unsigned(e.builds[0].returned && e.builds[0].result),
                    unsigned(e.builds[1].entered), unsigned(e.builds[1].returned && e.builds[1].result));
        }
    }
};
template<size_t site> void snpe_instruction(shadowhook_cpu_context_t* cpu, void*)
{
    auto* trace = snpe_initializing;
    if (!trace) return;
    if constexpr (site == 0x60bc8 || site == 0x60c5c) {
        if (cpu->regs[19] == reinterpret_cast<uintptr_t>(trace->owner) && trace->role == -1)
            trace->role = site == 0x60bc8 ? 0 : 1;
        else { trace->boundary_unknown = true; trace->role = -1; }
        return;
    }
    if constexpr (site == 0x60bcc || site == 0x60c60) {
        if (cpu->regs[19] != reinterpret_cast<uintptr_t>(trace->owner)) trace->boundary_unknown = true;
        trace->role = -1;
        return;
    }
    if (trace->role < 0) return;
    auto& e = trace->engines[trace->role];
    if constexpr (site == 0x610dc) {
        if (!e.engine) e.engine = reinterpret_cast<void*>(cpu->regs[0]);
        else trace->boundary_unknown = true;
        return;
    }
    if (!e.engine || cpu->regs[19] != reinterpret_cast<uintptr_t>(e.engine)) {
        trace->boundary_unknown = true;
        return;
    }
    if constexpr (site == 0x410d0 || site == 0x4116c) {
        constexpr unsigned attempt = site == 0x410d0 ? 0 : 1;
        e.runtime[attempt] = int(uint32_t(cpu->regs[1]));
        e.builders[attempt] = reinterpret_cast<void*>(cpu->regs[0]);
    } else if constexpr (site == 0x410fc) {
        ++e.setter_calls;
        e.original_profile = int(uint32_t(cpu->regs[1]));
        if (e.setter_calls == 1 && !e.builds[0].entered && !trace->boundary_unknown &&
            e.runtime[0] == 5 && e.original_profile == 6 &&
            cpu->regs[0] == reinterpret_cast<uintptr_t>(e.builders[0])) {
            cpu->regs[1] = 0;
            ++e.overrides;
        }
        e.forwarded_profile = int(uint32_t(cpu->regs[1]));
    } else if constexpr (site == 0x41100) {
        e.setter_returned = true;
        e.setter_return = reinterpret_cast<void*>(cpu->regs[0]);
    } else if constexpr (site == 0x41108) {
        e.priority = int(uint32_t(cpu->regs[1]));
    } else if constexpr (site == 0x41114 || site == 0x41198) {
        constexpr unsigned attempt = site == 0x41114 ? 0 : 1;
        constexpr size_t slot_offset = attempt ? 0x10 : 0x28;
        auto& build = e.builds[attempt];
        build.entered = true;
        build.profile_calls = attempt ? e.setter_calls - e.builds[0].profile_calls : e.setter_calls;
        if (cpu->regs[8] == cpu->sp + slot_offset &&
            cpu->regs[0] == reinterpret_cast<uintptr_t>(e.builders[attempt]))
            build.slot = reinterpret_cast<void*>(cpu->regs[8]);
        else trace->boundary_unknown = true;
    } else if constexpr (site == 0x41118 || site == 0x4119c) {
        constexpr unsigned attempt = site == 0x41118 ? 0 : 1;
        constexpr size_t slot_offset = attempt ? 0x10 : 0x28;
        auto& build = e.builds[attempt];
        if (build.entered && build.slot == reinterpret_cast<void*>(cpu->sp + slot_offset)) {
            build.returned = true;
            build.result = load<void*>(build.slot, 0);
        } else trace->boundary_unknown = true;
    }
}

// ---- one set of entry hooks; source timestamps copied before slot release ----
using expr_vec = expression_high_rate::Vector;
using expr_mat = expression_high_rate::Mat;
using expr_output = expression_high_rate::Output;
using infer_fn_t = expression_high_rate::Infer;
using trackor_fn_t = expression_high_rate::Trackor;
using init_fn_t = void (*)(void*);
using pipeline_fn_t = void (*)(void*, const void*);
using destroy_fn_t = void (*)(void*);
infer_fn_t g_original_infer;
trackor_fn_t g_original_trackor;
init_fn_t g_original_init;
pipeline_fn_t g_original_pipeline;
expression_independent::Merge g_original_merge;
destroy_fn_t g_original_destroy;
struct PipelineFrame { void* owner = nullptr; uint64_t sources[3] {}; };
thread_local PipelineFrame pipeline_frame;
struct PipelineScope {
    PipelineFrame previous = pipeline_frame;
    ~PipelineScope() { pipeline_frame = previous; }
};
__attribute__((noinline)) void pipeline_proxy(void* tracking, const void* input)
{
    // Capture sources even during installation: ready may change before this
    // stock call reaches trackor, and the input slot is released inside it.
    PipelineScope scope;
    pipeline_frame = {};
    void* owner = load<void*>(tracking, 0xa8);
    if (owner && load<uint32_t>(input, 0x180) != 1) {
        pipeline_frame.owner = owner;
        pipeline_frame.sources[0] = load<uint64_t>(input, 8);
        pipeline_frame.sources[1] = load<uint64_t>(input, 0x88);
        pipeline_frame.sources[2] = load<uint64_t>(input, 0x108);
    }
    g_original_pipeline(tracking, input); // Stock releases the input slot inside this call.
}
__attribute__((noinline)) expr_vec infer_proxy(void* a0, void* engine, const expr_vec* images)
{
    if (!g_expr_ready.load(std::memory_order_acquire)) return g_original_infer(a0, engine, images);
    expr_vec result;
    bool taken = expression_high_rate::take_mouth(engine, &result);
    if (!taken) {
        expression_high_rate::submit_before_eye(engine);
        result = g_original_infer(a0, engine, images);
    }
    size_t count = (uintptr_t(result.end) - uintptr_t(result.begin)) / sizeof(float);
    expression_high_rate::observe_infer(engine, static_cast<const float*>(result.begin), count);
    expression_independent::observe_infer(engine, static_cast<const float*>(result.begin), count);
    return result; // Stock retains exactly one ownership of the returned vector.
}
__attribute__((noinline)) expr_output trackor_proxy(void* owner, const expr_mat* eye0,
                                                  const expr_mat* eye1, const expr_mat* mouth)
{
    if (!g_expr_ready.load(std::memory_order_acquire)) return g_original_trackor(owner, eye0, eye1, mouth);
    if (pipeline_frame.owner != owner) {
        append_log("trackor: missing pipeline source identity; refusing unsourced inference");
        return {};
    }
    return expression_high_rate::dispatch(owner, eye0, eye1, mouth, pipeline_frame.sources);
}
void initialization_proxy(void* owner)
{
    if (g_expr_ready.load(std::memory_order_acquire) && !load<unsigned char>(owner, 0))
        expression_independent::reset_owner(owner);
    SnpeInitializationScope scope(owner);
    g_original_init(owner);
    scope.returned();
}
__attribute__((noinline, disable_tail_calls)) void merge_proxy(const void* current, const void* reference, void* result)
{
    if (!g_expr_ready.load(std::memory_order_acquire)) {
        g_original_merge(current, reference, result);
        return;
    }
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - g_expr_plugin;
    expression_independent::merge_stock(current, reference, result, caller);
}
void destroy_proxy(void* owner)
{
    if (g_expr_ready.load(std::memory_order_acquire)) expression_independent::forget_owner(owner);
    g_original_destroy(owner);
}
void normal_exit(shadowhook_cpu_context_t*, void*)
{
    expression_high_rate::observe_stock_completion(true);
    expression_independent::observe_completion(true);
}
void exception_exit(shadowhook_cpu_context_t*, void*)
{
    expression_high_rate::observe_stock_completion(false);
    expression_independent::observe_completion(false);
}
void export_prefix(shadowhook_cpu_context_t* cpu, void*)
{
    if (g_expr_ready.load(std::memory_order_acquire))
        expression_independent::export_prefix(reinterpret_cast<void*>(cpu->regs[0]));
}

// ---- compatibility guards and all-or-nothing plugin installation ------------
bool guard_word(uintptr_t base, size_t offset, uint32_t expected)
{
    const uint32_t got = load<uint32_t>(reinterpret_cast<const void*>(base + offset), 0);
    log_fmt("guard %s word site=0x%zx got=0x%08x expected=0x%08x",
            got == expected ? "ok" : "failed", offset, got, expected);
    return got == expected;
}
bool guard_windows(uintptr_t base)
{
    static const struct { size_t offset, length; uint64_t expected; } windows[] = {
        {0x605ac, 0x1f0, 0x31995bf748f7c422ULL},
        {0x613e0, 0x2a0, 0xb67cb8dc0066e98dULL},
        {0x625b4, 0x9dc, 0x0379770f754e251fULL},
        {0x250388, 0x10, 0x74f73c5bc154bf91ULL},
        {0x60998, 0x628, 0xa8fb98f3588bd4d1ULL},
        {0x60fc0, 0x25c, 0xff2ec440547cf57aULL},
        {0x40fc0, 0x528, 0x477dbc16f67ce89bULL},
        {0x36460, 0x10, 0xb88a47de34d4bfecULL},
        {0x366c0, 0x10, 0x4ecd7a0e8b6699f3ULL},
        {0x36cf0, 0x10, 0x23ad451078bcb155ULL},
        {0x36c40, 0x10, 0x8328d3d18f691462ULL},
        {0x66170, 0x2a4, 0xe3ec8bb09c7a4263ULL},
        {0x18dd44, 0x10, 0x5702cbec961b4e94ULL},
        {0x18e610, 0x10, 0x35562a3fdcdd52d7ULL},
        {0x18ed0c, 0x10, 0x990dcb8d8d413cb7ULL},
        {0x3426f8, 0x10, 0x4a3b22356dd72f38ULL},
        {0x34278c, 0x10, 0xc323d1543aea75a9ULL},
        {0x56860, 0x10, 0xfd6ece98e9cfd4f9ULL},
    };
    for (const auto& w : windows) {
        const uint64_t got = fnv1a64_mem(reinterpret_cast<const unsigned char*>(base + w.offset), w.length);
        log_fmt("guard %s window site=0x%zx length=0x%zx got=0x%016llX expected=0x%016llX",
                got == w.expected ? "ok" : "failed", w.offset, w.length,
                (unsigned long long)got, (unsigned long long)w.expected);
        if (got != w.expected) return false;
    }
    static const uint32_t head[] = {0xd10403ff, 0xf90063f7, 0xa90d57f6, 0xa90e4ff4};
    for (unsigned i = 0; i < 4; ++i)
        if (!guard_word(base, kOffOwnerInit + 4 * size_t(i), head[i])) return false;
    return true;
}
struct SnpeMapping { uintptr_t base = 0; bool readable = false; };
int discover_snpe(dl_phdr_info* info, size_t, void* opaque)
{
    if (strcmp(info->dlpi_name, "/system/lib64/libSNPE.so")) return 0;
    auto* mapping = static_cast<SnpeMapping*>(opaque);
    mapping->base = info->dlpi_addr;
    mapping->readable = true;
    static const struct { size_t offset, length; } regions[] = {
        {0x110f60, 0x184}, {0x1131dc, 12}, {0x1131f4, 12},
        {0x55faf1, 2}, {0x4d0933, 3}, {0x55f407, 2}
    };
    for (const auto& region : regions) {
        bool found = false;
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
            const auto& p = info->dlpi_phdr[i];
            if (p.p_type == PT_LOAD && (p.p_flags & PF_R) && region.offset >= p.p_vaddr &&
                region.offset + region.length <= p.p_vaddr + p.p_memsz) found = true;
        }
        if (!found) mapping->readable = false;
    }
    return 1;
}
bool snpe_symbols_available(uintptr_t plugin)
{
    SnpeMapping mapping;
    dl_iterate_phdr(discover_snpe, &mapping);
    if (!mapping.base || !mapping.readable) {
        append_log("SNPE expected /system/lib64/libSNPE.so mapping unavailable");
        return false;
    }
    static const struct { size_t offset, length; uint64_t expected; } windows[] = {
        {0x110f60, 0x184, 0x15e6fbca2dda835eULL},
        {0x1131dc, 12, 0xc141ea008b437883ULL},
        {0x1131f4, 12, 0x5e4e8480ebee28cbULL}
    };
    for (const auto& w : windows) {
        uint64_t got = fnv1a64_mem(reinterpret_cast<const unsigned char*>(mapping.base + w.offset), w.length);
        log_fmt("snpe-guard site=0x%zx got=0x%016llX expected=0x%016llX", w.offset,
                (unsigned long long)got, (unsigned long long)w.expected);
        if (got != w.expected) return false;
    }
    if (memcmp(reinterpret_cast<void*>(mapping.base + 0x55faf1), "1", 2) ||
        memcmp(reinterpret_cast<void*>(mapping.base + 0x4d0933), "61", 3) ||
        memcmp(reinterpret_cast<void*>(mapping.base + 0x55f407), "0", 2)) return false;
    static const struct { const char* name; size_t offset, got; } symbols[] = {
        {"_ZN3zdl4SNPE11SNPEBuilder19setRuntimeProcessorENS_8DlSystem9Runtime_tE", 0x1131d0, 0x4a8e50},
        {"_ZN3zdl4SNPE11SNPEBuilder21setPerformanceProfileENS_8DlSystem20PerformanceProfile_tE", 0x1131dc, 0x4a8f80},
        {"_ZN3zdl4SNPE11SNPEBuilder24setExecutionPriorityHintENS_8DlSystem23ExecutionPriorityHint_tE", 0x1131f4, 0x4a9298},
        {"_ZN3zdl4SNPE11SNPEBuilder5buildEv", 0x1134e4, 0x4a9240}
    };
    void* library = shadowhook_dlopen("/system/lib64/libSNPE.so"); // Existing mapping only.
    bool available = library != nullptr;
    for (const auto& symbol : symbols) {
        void* address = library ? shadowhook_dlsym_dynsym(library, symbol.name) : nullptr;
        void* expected = reinterpret_cast<void*>(mapping.base + symbol.offset);
        void* bound = load<void*>(reinterpret_cast<void*>(plugin), symbol.got);
        log_fmt("snpe-import symbol=%s address=%p expected=%p bound=%p", symbol.name, address, expected, bound);
        if (address != expected || bound != address) available = false;
    }
    if (library) shadowhook_dlclose(library);
    if (available) append_log("snpe-version=1.61.0.3616 static function/literals and imports guarded; getter not called");
    return available;
}
void* install_function(uintptr_t base, size_t offset, const char* stage, void* proxy, void** original)
{
    void* hook = shadowhook_hook_func_addr(reinterpret_cast<void*>(base + offset), proxy, original);
    int error = shadowhook_get_errno();
    log_fmt("hook stage=%s site=0x%zx address=%p handle=%p trampoline=%p errno=%d message=%s",
            stage, offset, reinterpret_cast<void*>(base + offset), hook, *original, error,
            shadowhook_to_errmsg(error));
    return hook;
}
bool intercept_one_instruction(uintptr_t base, size_t offset, const char* stage,
                                shadowhook_interceptor_t callback, void** hook)
{
    auto* site = reinterpret_cast<unsigned char*>(base + offset);
    unsigned char following[20];
    uint32_t original_instruction = load<uint32_t>(site, 0);
    memcpy(following, site + 4, sizeof(following));
    *hook = shadowhook_intercept_instr_addr(site, callback, nullptr,
                                                SHADOWHOOK_INTERCEPT_WITH_FPSIMD_READ_WRITE);
    int error = shadowhook_get_errno();
    if (*hook && memcmp(following, site + 4, sizeof(following))) {
        char expected[41], got[41];
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < 20; ++i) {
            expected[2 * i] = hex[following[i] >> 4];
            expected[2 * i + 1] = hex[following[i] & 15];
            got[2 * i] = hex[site[4 + i] >> 4];
            got[2 * i + 1] = hex[site[4 + i] & 15];
        }
        expected[40] = got[40] = 0;
        log_fmt("intercept rejected stage=%s site=0x%zx required=4-byte original_instruction=0x%08x "
                "patched_instruction=0x%08x following_expected=%s following_got=%s",
                stage, offset, original_instruction, load<uint32_t>(site, 0), expected, got);
        // Keep the handle in the transaction, including when later unhook fails.
        return false;
    }
    if (*hook) log_fmt("intercept accepted stage=%s site=0x%zx rewrite=4-byte original_instruction=0x%08x",
                      stage, offset, original_instruction);
    else log_fmt("intercept failed stage=%s site=0x%zx errno=%d message=%s",
                  stage, offset, error, shadowhook_to_errmsg(error));
    return *hook != nullptr;
}
bool patch_word(uintptr_t base, size_t offset, uint32_t word)
{
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    const size_t size = static_cast<size_t>(pagesz);
    void* page = reinterpret_cast<void*>((base + offset) & ~(static_cast<uintptr_t>(size) - 1));
    if (mprotect(page, size, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        log_fmt("patch: mprotect RWX failed site=0x%zx errno=%d", offset, errno);
        return false;
    }
    auto* site = reinterpret_cast<uint32_t*>(base + offset);
    __atomic_store_n(site, word, __ATOMIC_RELEASE);
    __builtin___clear_cache(reinterpret_cast<char*>(site), reinterpret_cast<char*>(site + 1));
    if (mprotect(page, size, PROT_READ | PROT_EXEC)) {
        log_fmt("patch: restore prot failed site=0x%zx errno=%d", offset, errno);
        return false;
    }
    return true;
}
bool rollback_hook(void* hook, const char* stage, size_t offset, bool instruction)
{
    if (!hook) return true;
    int rc = instruction ? shadowhook_unintercept(hook) : shadowhook_unhook(hook);
    int error = shadowhook_get_errno();
    log_fmt("rollback stage=%s site=0x%zx rc=%d errno=%d message=%s", stage, offset, rc, error,
             shadowhook_to_errmsg(error));
    return rc == 0;
}
int install_plugin(uintptr_t base)
{
    log_fmt("install begin pid=%ld base=%p version=%s", (long)getpid(),
            reinterpret_cast<void*>(base), kExpressionVersion);
    if (!guard_windows(base) || !snpe_symbols_available(base)) {
        append_log("plugin/SNPE guard mismatch; no plugin hooks installed");
        return -1;
    }
    struct Patch { size_t offset; uint32_t original, replacement; };
    const Patch patches[] = {
        {0x62a8c, 0x540000c1, 0x14000010}, // Skip cross-eye Blink copy.
        {0x62b70, 0x2a1c0368, 0x14000023}, // Independent references replace shared tail.
        {0x661d4, 0xb9808a69, 0x52800009}, // Remove submission interval, not image/pairing gates.
    };
    for (const auto& p : patches) if (!guard_word(base, p.offset, p.original)) return -1;
    if (!guard_word(base, kOffMerge, 0xd10503ff) || !guard_word(base, kOffDestroy, 0xf81e0ff3) ||
        !guard_word(base, kOffExport, 0xd63f02c0)) return -1;
    g_expr_plugin = base;
    struct FunctionHook { size_t offset; const char* stage; void* proxy; void** original; void* hook = nullptr; };
    FunctionHook functions[] = {
        {kOffInfer, "infer", (void*)infer_proxy, (void**)&g_original_infer},
        {kOffTrackor, "trackor", (void*)trackor_proxy, (void**)&g_original_trackor},
        {kOffOwnerInit, "owner-init", (void*)initialization_proxy, (void**)&g_original_init},
        {kOffPipeline, "pipeline", (void*)pipeline_proxy, (void**)&g_original_pipeline},
        {kOffMerge, "merge", (void*)merge_proxy, (void**)&g_original_merge},
        {kOffDestroy, "destroy", (void*)destroy_proxy, (void**)&g_original_destroy},
    };
    struct InstructionHook { size_t offset; const char* stage; shadowhook_interceptor_t callback; void* hook = nullptr; };
    InstructionHook instructions[] = {
        {kOffNormalExit, "normal-exit", normal_exit}, {kOffCatchExit, "catch-exit", exception_exit},
        {kOffExport, "export", export_prefix},
        {0x60bc8, "snpe-init", snpe_instruction<0x60bc8>}, {0x60bcc, "snpe-init", snpe_instruction<0x60bcc>},
        {0x60c5c, "snpe-init", snpe_instruction<0x60c5c>}, {0x60c60, "snpe-init", snpe_instruction<0x60c60>},
        {0x610dc, "snpe-init", snpe_instruction<0x610dc>},
        {0x410d0, "snpe-init", snpe_instruction<0x410d0>}, {0x4116c, "snpe-init", snpe_instruction<0x4116c>},
        {0x410fc, "snpe-init", snpe_instruction<0x410fc>}, {0x41100, "snpe-init", snpe_instruction<0x41100>},
        {0x41108, "snpe-init", snpe_instruction<0x41108>},
        {0x41114, "snpe-init", snpe_instruction<0x41114>}, {0x41118, "snpe-init", snpe_instruction<0x41118>},
        {0x41198, "snpe-init", snpe_instruction<0x41198>}, {0x4119c, "snpe-init", snpe_instruction<0x4119c>},
    };
    bool installed = true;
    for (auto& f : functions) {
        *f.original = nullptr;
        f.hook = install_function(base, f.offset, f.stage, f.proxy, f.original);
        if (!f.hook || !*f.original) { installed = false; break; }
    }
    if (installed) for (auto& i : instructions) {
        if (!intercept_one_instruction(base, i.offset, i.stage, i.callback, &i.hook)) {
            installed = false;
            break;
        }
    }
    size_t patched = 0;
    if (installed) {
        expression_independent::configure(base, g_original_merge);
        expression_high_rate::configure(base, g_original_trackor, infer_proxy, g_original_infer,
            reinterpret_cast<expression_high_rate::Resize>(base + 0x250388));
        for (const auto& p : patches) {
            ++patched; // Also restore a write whose protection-restoration step fails.
            if (!patch_word(base, p.offset, p.replacement)) { installed = false; break; }
        }
    }
    if (installed) {
        snpe_hooks_ready.store(true, std::memory_order_release);
        g_expr_ready.store(true, std::memory_order_release);
        log_fmt("install complete; independent expressions eye_hz=%u mouth_hz=%u; source-dt; "
                "effective observation time; EPI1 v2; scoped FaceTrackor profile0",
                g_rates.eye_hz, g_rates.mouth_hz);
        return 0;
    }
    append_log("plugin install failed; rolling back plugin hooks and patches; service restart required");
    bool restored = true;
    while (patched) {
        const auto& p = patches[--patched];
        bool ok = patch_word(base, p.offset, p.original);
        log_fmt("rollback patch site=0x%zx restored=%d", p.offset, int(ok));
        if (!ok) restored = false;
    }
    for (size_t n = sizeof(instructions) / sizeof(instructions[0]); n; --n) {
        const auto& i = instructions[n - 1];
        if (!rollback_hook(i.hook, i.stage, i.offset, true)) restored = false;
    }
    for (size_t n = sizeof(functions) / sizeof(functions[0]); n; --n) {
        const auto& f = functions[n - 1];
        if (!rollback_hook(f.hook, f.stage, f.offset, false)) restored = false;
    }
    return restored ? -1 : -2;
}

// ---- original mode/bootstrap and asynchronous plugin discovery --------------
uint32_t read_frequency(const char* path, uint32_t fallback)
{
    int fd = picoet_open_root_file(path, O_RDONLY, 0);
    if (fd < 0) {
        if (errno != ENOENT) log_fmt("frequency: open %s failed errno=%d; default=%u", path, errno, fallback);
        return fallback;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0 && (size_t)n < sizeof(buf) - 1 && buf[0] >= '0' && buf[0] <= '9') {
        buf[n] = '\0';
        char* end;
        errno = 0;
        unsigned long value = strtoul(buf, &end, 10);
        const bool valid_number = !errno && value > 0 && value <= UINT32_MAX;
        while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
        if (valid_number && !*end && end == buf + n) return static_cast<uint32_t>(value);
    }
    log_fmt("frequency: invalid/unreadable %s; default=%u", path, fallback);
    return fallback;
}
int read_enhanced_mode()
{
    int fd = picoet_open_root_file(kEyeModeFile, O_RDONLY, 0);
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
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') buf[i] = '\0';
        else break;
    }
    if (strcmp(buf, "dual") == 0) return 1;
    if (strcmp(buf, "off") != 0) log_fmt("mode: unrecognized value '%s'; gate only", buf);
    return 0;
}
void on_plugin_found(void* base)
{
    picoet_plugin_base = (unsigned long long)base;
    int rc = install_plugin(reinterpret_cast<uintptr_t>(base));
    if (rc != 0) {
        if (rc == -2) {
            // Do not let a service with residual branches/hooks continue running.
            // This private token also covers failures in the async poll thread,
            // after the injector has already returned. Schema 2 stays unchanged.
            char token[64];
            int len = snprintf(token, sizeof(token), "%ld:%llu\n", (long)getpid(), g_start_ticks);
            if (!g_start_ticks || len <= 0 || static_cast<size_t>(len) >= sizeof(token) ||
                publish_record("unsafe", token, static_cast<size_t>(len)) != 0)
                append_log("unsafe rollback: cannot persist recovery token");
            append_log("unsafe rollback: terminating service");
            _exit(5);
        }
        report_runtime("failed");
        return;
    }
    if (__atomic_load_n(&picoet_gate_installed, __ATOMIC_ACQUIRE))
        __atomic_store_n(&picoet_dual_installed, 1, __ATOMIC_RELEASE);
    report_runtime("ready");
}
void* plugin_poll_thread(void*)
{
    for (int i = 0; i < 300; i++) {
        void* b = find_phoenix();
        if (b) { on_plugin_found(b); return nullptr; }
        usleep(200 * 1000);
    }
    append_log("plugin poll: not found after 60s");
    report_runtime("failed");
    return nullptr;
}
__attribute__((constructor)) void picoet_init()
{
    static volatile int once = 0;
    if (once) return;
    once = 1;
    picoet_hook_marker = 0x5049434F45543150ULL;
    g_start_ticks = process_start_ticks();
    g_enhanced_mode = read_enhanced_mode();
    g_gate_requested = read_gate_enabled();
    const uint32_t eye_hz = read_frequency(kEyeHzFile, 90);
    const uint32_t mouth_hz = read_frequency(kMouthHzFile, 90);
    g_rates = expression_high_rate::configure_rates(eye_hz, mouth_hz);
    log_fmt("frequency startup: requested_eye_hz=%u requested_mouth_hz=%u "
            "effective_eye_hz=%u effective_mouth_hz=%u mouth_clamped=%u",
            g_rates.requested_eye_hz, g_rates.requested_mouth_hz,
            g_rates.eye_hz, g_rates.mouth_hz, unsigned(g_rates.mouth_hz != mouth_hz));
    if (publish_runtime("initializing") != 0) {
        append_log("runtime state unavailable; refusing all hooks (prepare picoet-runtime directory)");
        return;
    }
    if (install_gate_hook() != 0) {
        append_log("service guard/gate initialization failed; no plugin writes permitted");
        report_runtime("failed");
        return;
    }
    if (!g_enhanced_mode) { report_runtime("ready"); return; }
    report_runtime("pending");
    void* b = find_phoenix();
    if (b) on_plugin_found(b);
    else {
        append_log("plugin not loaded yet; starting poll thread");
        pthread_t t;
        if (pthread_create(&t, nullptr, plugin_poll_thread, nullptr) == 0) pthread_detach(t);
        else { append_log("plugin poll thread creation failed"); report_runtime("failed"); }
    }
}
}
