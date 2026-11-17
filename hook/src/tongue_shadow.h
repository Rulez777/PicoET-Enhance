#pragma once
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstddef>
#include "root_file.h"
#include "tongue_seg.h"
#include "tongue_cpu.h"
#include "tongue_train.h"
#include <mutex>
// On-headset tongue model. Release builds write no diagnostics: the shadow log and frame files
// below exist only when built with -DPICOET_TONGUE_DIAG=ON (hook/CMakeLists.txt).
// On-headset tongue model. Two root-owned opt-in switches:
//   /data/local/tmp/tongue-live.ip     LIVE: TNG2 packets over Wi-Fi UDP 19031 to the subscribing
//                                      UnifiedPicoModule (and abstract socket pico_tongue_live for ADB)
//   /data/local/tmp/tongue-shadow.arm  SHADOW: also keep raw frames for offline review
//   /data/local/tmp/tongue-calibration per-user angles + mask threshold (calibration app, TCFG)
// The tracking thread copies a 2x2-averaged 200x200 frame at up to 30 Hz on demand (no inference, no I/O
// beyond the arm check every 500ms). A worker waits for frame and socket events and runs tongue_seg. Diagnostic output:
//   /data/local/tmp/tongue-shadow.log     one line per inference (capped)
//   /data/local/tmp/tongue-shadow.frames  raw 200x200 frames for offline review (capped)
namespace tongue_shadow_impl {
static unsigned char slot[200 * 200];
static std::mutex slot_lock;
static unsigned slot_seq = 0;
static unsigned long long slot_ns = 0;
static int frame_event = -1;
static std::atomic<bool> wants_frames{false};
constexpr uint64_t kLeaseNs = 3000000000ULL;
constexpr uint64_t kActiveIntervalNs = 33333334ULL;
constexpr uint64_t kIdleIntervalNs = 125000000ULL;
constexpr uint64_t kMotionHoldNs = 500000000ULL;

static uint64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

static void wake_worker() {
    const uint64_t one = 1;
    while (write(frame_event, &one, sizeof(one)) < 0 && errno == EINTR) {}
}
static std::atomic<bool> worker_started{false};
static std::atomic<bool> live{false}, keep_frames{false};

// Per-user calibration file: "left center right min_area", written by the calibration app through
// TCFG (below) or by hand. Missing or invalid -> the model's defaults.
static const char kCalibration[] = "/data/local/tmp/tongue-calibration";
static bool valid_calibration(float l, float c, float r, int area) {
    auto ok = [](float v) { return v > 0.f && v < 1.7f; };
    return ok(l) && ok(c) && ok(r) && std::fabs(l - c) > .08f && std::fabs(r - c) > .08f &&
           (l - c) * (r - c) < 0 && area >= 20 && area <= 600;
}
static void load_calibration() {
    tongue_seg::Calibration cal{tongue_seg_weights::angle_left, tongue_seg_weights::angle_center,
                                tongue_seg_weights::angle_right, 100};
    int f = picoet_open_root_file(kCalibration, O_RDONLY, 0);
    if (f >= 0) {
        char text[96] = {}; ssize_t n = read(f, text, sizeof(text) - 1); close(f);
        float l, c, r; int area;
        if (n > 0 && sscanf(text, "%f %f %f %d", &l, &c, &r, &area) == 4 && valid_calibration(l, c, r, area))
            cal = {l, c, r, area};
    }
    tongue_seg::calibration = cal;
}
static void save_calibration(float l, float c, float r, int area) {
    char tmp[] = "/data/local/tmp/tongue-calibration.new";
    unlink(tmp);
    int f = picoet_open_root_file(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (f < 0) return;
    dprintf(f, "%.4f %.4f %.4f %d\n", l, c, r, area); close(f);
    rename(tmp, kCalibration);
}

// Per-user suppression map (tongue_seg::suppress), 56*56 bytes of 0/1, written by the calibration
// app through TSUP. Missing or wrong-sized -> nothing suppressed. Calibration reset clears it.
static const char kSuppress[] = "/data/local/tmp/tongue-suppress.bin";
static int suppressed_cells = 0;
static void load_suppression() {
    unsigned char map[56 * 56] = {};
    int f = picoet_open_root_file(kSuppress, O_RDONLY, 0);
    if (f >= 0) { if (read(f, map, sizeof(map)) != ssize_t(sizeof(map))) memset(map, 0, sizeof(map)); close(f); }
    int n = 0;
    for (int i = 0; i < 56 * 56; i++) { map[i] = map[i] ? 1 : 0; n += map[i]; }
    if (n > 56 * 56 / 2) { memset(map, 0, sizeof(map)); n = 0; }   // implausible: ignore
    memcpy(tongue_seg::suppress, map, sizeof(map)); suppressed_cells = n;
}
static void save_suppression(const unsigned char* map) {
    char tmp[] = "/data/local/tmp/tongue-suppress.bin.new";
    unlink(tmp);
    int f = picoet_open_root_file(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (f < 0) return;
    bool ok = write(f, map, 56 * 56) == 56 * 56; close(f);
    if (ok) rename(tmp, kSuppress); else unlink(tmp);
}

// UDP 19031, one socket for every peer:
//   LAN  "TSUB"           PC module subscribes; TNG2 tongue packets for 3s (when live)
//   127.0.0.1 "TCAM"      calibration app subscribes; TFRM camera+mask+geometry for 3s
//   127.0.0.1 "TCFG" L C R A   save + apply a calibration     "TRST"  back to defaults
// The PC starts the exchange, so a stateful PC firewall admits the replies without any rule.
struct __attribute__((packed)) FrameHeader {
    char magic[4]; uint32_t seq, flags; int32_t area; float raw_angle, position, left, right, out,
    nose_x, nose_y; int32_t nose; float cal_left, cal_center, cal_right; int32_t cal_area;
    // TFR2: retraining status for the app
    uint32_t personal_model, train_state; float train_progress, old_score, new_score;
    uint32_t rec_label, rec_count, suppressed_cells;
};
static_assert(sizeof(FrameHeader) == 96, "TFR2 header ABI");

// Socket state is owned by the worker, including when no frames are requested.
static int sock = -1, client = -1, udp = -1;
static sockaddr_in pc{}, app{};
static uint64_t pc_seen = 0, app_seen = 0;

static void open_sockets() {
    if (sock < 0) {
        sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (sock >= 0) { sockaddr_un local{}; local.sun_family = AF_UNIX;
            const char name[] = "pico_tongue_live"; memcpy(local.sun_path + 1, name, sizeof(name) - 1);
            if (bind(sock, (sockaddr*)&local, offsetof(sockaddr_un, sun_path) + sizeof(name)) < 0 || listen(sock, 1) < 0) { close(sock); sock = -1; } }
    }
    if (udp < 0) {
        udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_in any{}; any.sin_family = AF_INET; any.sin_port = htons(19031); any.sin_addr.s_addr = htonl(INADDR_ANY);
        if (udp >= 0) { int size = 1 << 18; setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)); }
        if (udp >= 0 && bind(udp, (sockaddr*)&any, sizeof(any)) < 0) { close(udp); udp = -1; }
    }
}
static bool leased(uint64_t seen, uint64_t now) {
    return seen && now - seen < kLeaseNs;
}

static void receive_commands(uint64_t now) {
    if (udp < 0) return;
    for (int i = 0; i < 8; i++) {
        static char req[4 + 56 * 56 + 16]; sockaddr_in from{}; socklen_t len = sizeof(from);
        ssize_t n = recvfrom(udp, req, sizeof(req), MSG_DONTWAIT, (sockaddr*)&from, &len);
        if (n < 0) break;
        if (n < 4 || from.sin_family != AF_INET) continue;
        uint32_t a = ntohl(from.sin_addr.s_addr);
        bool loopback = (a >> 24) == 127;
        // Private, carrier-grade/VPN (100.64/10, e.g. Tailscale) and link-local networks only.
        bool lan = (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8 ||
                   (a >> 22) == (0x64400000u >> 22) || (a >> 16) == 0xa9fe;
        if (!memcmp(req, "TSUB", 4) && lan) { pc = from; pc_seen = now; }
        else if (!memcmp(req, "TCAM", 4) && loopback) { app = from; app_seen = now; }
        else if (!memcmp(req, "TCFG", 4) && loopback && n >= 20) {
            float l, c, r, area; memcpy(&l, req + 4, 4); memcpy(&c, req + 8, 4); memcpy(&r, req + 12, 4); memcpy(&area, req + 16, 4);
            if (valid_calibration(l, c, r, int(area))) { save_calibration(l, c, r, int(area)); load_calibration(); }
        }
        // TRST: back to the default calibration and no suppression (the app sends it before a new run).
        else if (!memcmp(req, "TRST", 4) && loopback) { unlink(kCalibration); unlink(kSuppress); load_calibration(); load_suppression(); }
        // TSUP <56*56 bytes>: per-user suppression map measured on this person's no-tongue mouth.
        else if (!memcmp(req, "TSUP", 4) && loopback && n == 4 + 56 * 56) {
            save_suppression(reinterpret_cast<unsigned char*>(req + 4)); load_suppression();
        }
        // TSUM <56*56 bytes>: add cells to the existing map (retraining keeps the calibration's map).
        else if (!memcmp(req, "TSUM", 4) && loopback && n == 4 + 56 * 56) {
            unsigned char merged[56 * 56];
            for (int i = 0; i < 56 * 56; i++) merged[i] = (tongue_seg::suppress[i] || req[4 + i]) ? 1 : 0;
            save_suppression(merged); load_suppression();
        }
        // Retraining (tongue_train.h): TREC <label> records the following frames under that label
        // (0 stops), TCLR drops the recording, TTRN starts training, TSTD restores the shipped model
        // and calibration.
        else if (!memcmp(req, "TREC", 4) && loopback && n >= 5 && uint8_t(req[4]) <= tongue_train::RIGHT) tongue_train::recording = uint8_t(req[4]);
        else if (!memcmp(req, "TCLR", 4) && loopback) tongue_train::clear_recording();
        else if (!memcmp(req, "TTRN", 4) && loopback) tongue_train::start();
        else if (!memcmp(req, "TSTD", 4) && loopback) {
            tongue_train::restore_standard(); unlink(kCalibration); unlink(kSuppress); load_calibration(); load_suppression();
        }
    }
}

static void publish(unsigned seq, const tongue_seg::Geometry& g, bool active, float left, float right,
                    float out, const unsigned char* frame, const float* logits, uint64_t now) {
    const bool is_live = live.load();
    struct Packet { char magic[4]; unsigned seq; float l, r, o, c; } packet{{'T', 'N', 'G', '2'},
        (seq & 0x3fffffffU) | (active ? 0U : 0x80000000U), left, right, out, active ? 1.f : 0.f};
    static_assert(sizeof(Packet) == 24, "TNG2 packet ABI");
    if (is_live && client >= 0 && send(client, &packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL) != ssize_t(sizeof(packet))) { close(client); client = -1; }

    if (udp < 0) return;
    if (is_live && leased(pc_seen, now))
        sendto(udp, &packet, sizeof(packet), MSG_DONTWAIT, (sockaddr*)&pc, sizeof(pc));
    if (leased(app_seen, now)) {
        static unsigned char buf[sizeof(FrameHeader) + 200 * 200 + 56 * 56];
        const auto& cal = tongue_seg::calibration;
        uint32_t rec_count;
        { std::lock_guard<std::mutex> lg(tongue_train::lock); rec_count = uint32_t(tongue_train::samples.size()); }
        FrameHeader h{{'T', 'F', 'R', '2'}, seq, uint32_t((g.present ? 1 : 0) | (g.raw ? 2 : 0) | (active ? 4 : 0)),
                      g.area, g.raw_angle, g.position, left, right, out, g.nose_x, g.nose_y, g.nose,
                      cal.left, cal.center, cal.right, cal.min_area,
                      tongue_train::personal.load() ? 1u : 0u, tongue_train::state.load(), tongue_train::progress.load(),
                      tongue_train::old_score.load(), tongue_train::new_score.load(),
                      tongue_train::recording.load(), rec_count, uint32_t(suppressed_cells)};
        memcpy(buf, &h, sizeof(h)); memcpy(buf + sizeof(h), frame, 200 * 200);
        unsigned char* m = buf + sizeof(h) + 200 * 200;   // what counts: suppressed cells shown as 0
        for (int i = 0; i < 56 * 56; i++)
            m[i] = tongue_seg::suppress[i] ? 0 : (unsigned char)(255.f / (1.f + std::exp(-logits[i])));
        sendto(udp, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr*)&app, sizeof(app));
    }
}

static void* worker(void*) {
    // Normal time-sharing priority (explicit, never inherited from the tracking thread).
    int tid = int(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, tid, 0);
    // The tracking service is confined to the little cores; move only this thread to the fastest
    // cores (found from cpufreq) so it never competes with eye/face tracking.
    int pinned = tongue_cpu::pin_fast();
#ifdef PICOET_TONGUE_DIAG
    {
        int f = picoet_open_root_file("/data/local/tmp/tongue-shadow.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (f >= 0) {
            // The log is appended across service restarts: start over once it passes 8 MiB.
            struct stat st;
            if (fstat(f, &st) == 0 && st.st_size > (8 << 20) && ftruncate(f, 0) != 0) { /* keep appending */ }
            dprintf(f, "# worker tid=%d fast_cores_pinned=%d\n", tid, pinned); close(f);
        }
    }
#else
    (void)pinned;
#endif
    unsigned seen = 0, written_frames = 0, lines = 0;
    unsigned char frame[200 * 200]; static float logits[56 * 56];
    int on = 0, off = 0; bool active = false; float pos = 0, out = 0;
    uint64_t calibration_checked = 0, last_infer = 0, motion_until = 0;
    unsigned char reference[100]{};
    bool have_reference = false;
    load_calibration(); load_suppression();
    { tongue_seg::Head h; if (tongue_train::load_head(h)) { tongue_seg::active_head = h; tongue_train::personal = true; } }
    for (;;) {
        open_sockets();
        const uint64_t now = monotonic_ns();
        receive_commands(now);
        const bool is_live = live.load();
        if (!is_live && client >= 0) { close(client); client = -1; }
        if (is_live && client < 0 && sock >= 0)
            client = accept4(sock, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);

        const bool app_active = leased(app_seen, now);
        if (!app_active && app_seen) {
            app_seen = 0;
            tongue_train::recording = tongue_train::NONE;
        }
        const bool full_rate = app_active || keep_frames.load() ||
            tongue_train::state.load() == tongue_train::RUNNING;
        const bool demand = full_rate || (is_live && (leased(pc_seen, now) || client >= 0));
        wants_frames.store(demand);

        unsigned s;
        unsigned long long ns;
        bool got_frame;
        {
            std::lock_guard<std::mutex> guard(slot_lock);
            s = slot_seq;
            ns = slot_ns;
            got_frame = demand && s != seen && now < ns + 250000000ULL;
            if (got_frame) memcpy(frame, slot, sizeof(frame));
            seen = s;
        }
        if (!demand || (last_infer && now >= last_infer + 350000000ULL)) {
            on = off = 0;
            active = false;
            pos = out = 0;
            have_reference = false;
            last_infer = motion_until = 0;
        }
        if (!got_frame) {
            int timeout = (udp < 0 || sock < 0) ? 1000 : -1;
            for (uint64_t lease : {pc_seen, app_seen}) {
                if (!leased(lease, now)) continue;
                const int ms = int((lease + kLeaseNs - now + 999999) / 1000000);
                if (timeout < 0 || ms < timeout) timeout = ms;
            }
            pollfd fds[] = {{frame_event, POLLIN, 0}, {udp, POLLIN, 0},
                {is_live && client < 0 ? sock : -1, POLLIN, 0}, {client, 0, 0}};
            if (poll(fds, 4, timeout) > 0) {
                if (fds[0].revents & POLLIN) {
                    uint64_t count;
                    (void)read(frame_event, &count, sizeof(count));
                }
                if (fds[3].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                    close(client);
                    client = -1;
                }
            }
            continue;
        }

        // Compare sparse mouth pixels to the last inferred frame, so gradual movement accumulates.
        unsigned char grid[100];
        unsigned delta = 0, changed = 0;
        for (unsigned i = 0; i < 100; ++i) {
            grid[i] = frame[(70 + (i / 10) * 12) * 200 + 10 + (i % 10) * 20];
            const unsigned diff = unsigned(std::abs(int(grid[i]) - int(reference[i])));
            delta += diff;
            changed += diff >= 16;
        }
        if (!have_reference || delta >= 400 || changed >= 5) motion_until = ns + kMotionHoldNs;
        if (!full_rate && !active && !on && ns >= motion_until &&
            last_infer && ns - last_infer < kIdleIntervalNs) continue;
        memcpy(reference, grid, sizeof(grid));
        have_reference = true;
        const float smoothing_dt = last_infer ? float(ns - last_infer) * 1e-9f : .033333334f;
        last_infer = ns;
        if (ns - calibration_checked > 2000000000ULL) { calibration_checked = ns; load_calibration(); load_suppression(); }
        {   // install a newly trained head + its calibration, or go back to the shipped model
            std::lock_guard<std::mutex> lg(tongue_train::lock);
            if (tongue_train::pending_head) {
                tongue_seg::active_head = *tongue_train::pending_head; tongue_train::pending_head.reset(); tongue_train::personal = true;
            }
            if (tongue_train::pending_calibration_valid) {
                const auto& c = tongue_train::pending_calibration;
                if (valid_calibration(c.left, c.center, c.right, c.min_area)) save_calibration(c.left, c.center, c.right, c.min_area);
                tongue_train::pending_calibration_valid = false; load_calibration();
            }
            if (tongue_train::pending_reset) {
                tongue_seg::active_head = tongue_seg::standard_head(); tongue_train::pending_reset = false; tongue_train::personal = false;
            }
        }
        timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        tongue_seg::infer(frame, logits);
        auto g = tongue_seg::geometry(frame, logits);
        // Hysteresis on the mask size: a tongue needs min_area cells to appear, but once shown it
        // stays while it keeps 60% of that. Far-right tongues shrink to ~90-130 cells on the
        // owner's head-mount and would otherwise flicker out; lip false masks never start a tongue.
        if (active && !g.present) {
            tongue_seg::Calibration keep = tongue_seg::calibration;
            keep.min_area = keep.min_area * 6 / 10;
            auto held_geometry = tongue_seg::geometry(frame, logits, keep);
            if (held_geometry.present) g = held_geometry;
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        tongue_train::record(frame, logits);
        // Active output at up to 30 Hz: two-frame hysteresis and a 40 ms EMA time constant.
        if (g.present) { on++; off = 0; } else { off++; on = 0; }
        if (!active && on >= 2) active = true;
        if (active && off >= 2) active = false;
        const float alpha = 1.f - std::exp(-smoothing_dt / .04f);
        if (active) { if (g.present) pos += (g.position - pos) * alpha; out += (1 - out) * alpha; }
        else { out += (0 - out) * alpha; pos += (0 - pos) * alpha; }
        float left = (pos < 0 ? -pos : 0) * out, right = (pos > 0 ? pos : 0) * out;
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        publish(s, g, active, left, right, out, frame, logits, monotonic_ns());
#ifdef PICOET_TONGUE_DIAG
        if (lines < 20000) {   // per service run (~17 min at 20 Hz)
            int f = picoet_open_root_file("/data/local/tmp/tongue-shadow.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (f >= 0) { dprintf(f, "%u %llu present=%d area=%d angle=%.3f position=%.3f active=%d left=%.3f right=%.3f out=%.3f ms=%.1f\n",
                                  s, ns / 1000000ULL, g.present, g.area, g.angle, g.position, active, left, right, out, ms);
                          close(f); lines++; }
        }
        if (keep_frames.load(std::memory_order_relaxed) && written_frames < 3000) {
            int f = picoet_open_root_file("/data/local/tmp/tongue-shadow.frames", O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (f >= 0) { if (write(f, frame, sizeof(frame)) == ssize_t(sizeof(frame))) written_frames++; close(f); }
        }
#else
        (void)ms; (void)lines; (void)written_frames;
#endif
    }
    return nullptr;
}
}

static void tongue_shadow(const void* mat) {
    using namespace tongue_shadow_impl;
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    static uint64_t last = 0, last_arm_check = 0;
    const uint64_t now = monotonic_ns();
    if (busy.test_and_set()) return;
    struct Unlock { std::atomic_flag& f; ~Unlock() { f.clear(); } } unlock{busy};
    if (now - last_arm_check >= 500000000ULL) {
        last_arm_check = now;
#ifdef PICOET_TONGUE_DIAG
        int arm = picoet_open_root_file("/data/local/tmp/tongue-shadow.arm", O_RDONLY, 0);
#else
        int arm = -1;   // release builds keep no frames and write no per-frame log
#endif
        int ip = picoet_open_root_file("/data/local/tmp/tongue-live.ip", O_RDONLY, 0);
        const bool shadow_changed = keep_frames.exchange(arm >= 0) != (arm >= 0);
        const bool live_changed = live.exchange(ip >= 0) != (ip >= 0);
        if (arm >= 0) close(arm);
        if (ip >= 0) close(ip);
        if ((shadow_changed || live_changed) && worker_started.load()) wake_worker();
    }
    if (!worker_started.load()) {
        frame_event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (frame_event < 0) return;
        pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 1 << 20);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        sched_param sp0{}; pthread_attr_setschedpolicy(&attr, SCHED_OTHER); pthread_attr_setschedparam(&attr, &sp0);
        if (pthread_create(&t, &attr, worker, nullptr) == 0) worker_started = true;
        else { close(frame_event); frame_event = -1; }
        pthread_attr_destroy(&attr);
    }
    if (!wants_frames.load() || now - last < kActiveIntervalNs) return;
    unsigned char h[96]; iovec dh{h, sizeof(h)}, sh{const_cast<void*>(mat), sizeof(h)};
    if (process_vm_readv(getpid(), &dh, 1, &sh, 1, 0) != sizeof(h)) return;
    int flags, rows, cols; unsigned char* data; size_t stride;
    memcpy(&flags, h, 4); memcpy(&rows, h + 8, 4); memcpy(&cols, h + 12, 4); memcpy(&data, h + 16, 8); memcpy(&stride, h + 80, 8);
    // Any single-channel 8-bit mouth image (PICO 4 Pro / Enterprise deliver 400x400) up to 1 MiB;
    // it is box-averaged to the model's 200x200 (for 400x400 exactly the trained 2x2 average).
    unsigned channels = ((unsigned(flags) >> 3) & 511) + 1;
    if ((unsigned(flags) & 0xffff0000U) != 0x42ff0000U || (flags & 7) != 0 || channels != 1 || !data ||
        rows < 200 || cols < 200 || stride < size_t(cols) || stride > (1u << 20) / size_t(rows)) return;
    static unsigned char pixels[1 << 20];
    size_t bytes = size_t(rows) * stride;
    iovec dp{pixels, bytes}, sp{data, bytes};
    if (process_vm_readv(getpid(), &dp, 1, &sp, 1, 0) != ssize_t(bytes)) return;
    last = now;
    std::unique_lock<std::mutex> guard(slot_lock, std::try_to_lock);
    if (!guard.owns_lock()) return;
    if (rows == 400 && cols == 400) {
        for (unsigned y = 0; y < 200; ++y) {
            const unsigned char* top = pixels + 2 * y * stride;
            const unsigned char* bottom = top + stride;
            for (unsigned x = 0; x < 200; ++x) {
                const unsigned i = 2 * x;
                slot[y * 200 + x] = (top[i] + top[i + 1] + bottom[i] + bottom[i + 1] + 2) / 4;
            }
        }
    } else {
        for (unsigned y = 0; y < 200; y++) {
            unsigned y0 = y * rows / 200, y1 = (y + 1) * rows / 200;
            for (unsigned x = 0; x < 200; x++) {
                unsigned x0 = x * cols / 200, x1 = (x + 1) * cols / 200, sum = 0, n = (y1 - y0) * (x1 - x0);
                for (unsigned yy = y0; yy < y1; yy++) for (unsigned xx = x0; xx < x1; xx++) sum += pixels[yy * stride + xx];
                slot[y * 200 + x] = (unsigned char)((sum + n / 2) / n);
            }
        }
    }
    slot_ns = now;
    ++slot_seq;
    guard.unlock();
    wake_worker();
}
