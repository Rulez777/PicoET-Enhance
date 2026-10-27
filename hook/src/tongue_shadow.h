#pragma once
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
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
#include "tongue_train.h"
#include <mutex>
// On-headset tongue model. Two root-owned opt-in switches:
//   /data/local/tmp/tongue-live.ip     LIVE: TNG2 packets over Wi-Fi UDP 19031 to the subscribing
//                                      UnifiedPicoModule (and abstract socket pico_tongue_live for ADB)
//   /data/local/tmp/tongue-shadow.arm  SHADOW: also keep raw frames for offline review
//   /data/local/tmp/tongue-calibration per-user angles + mask threshold (calibration app, TCFG)
// The tracking thread only copies a 2x2-averaged 200x200 frame at <=20Hz (no inference, no I/O
// beyond the arm check every 500ms). A lazily created nice-19 worker runs tongue_seg and LOGS:
//   /data/local/tmp/tongue-shadow.log     one line per inference (capped)
//   /data/local/tmp/tongue-shadow.frames  raw 200x200 frames for offline review (capped)
namespace tongue_shadow_impl {
static unsigned char slot[200 * 200];
static std::atomic<unsigned> slot_seq{0};
static std::atomic<unsigned long long> slot_ns{0};
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
    uint32_t rec_label, rec_count, reserved;
};
static_assert(sizeof(FrameHeader) == 96, "TFR2 header ABI");

static void publish(unsigned seq, const tongue_seg::Geometry& g, bool active, float left, float right,
                    float out, const unsigned char* frame, const float* logits) {
    static int sock = -1, client = -1;   // abstract unix socket kept for ADB debugging
    if (sock < 0) {
        sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (sock >= 0) { sockaddr_un local{}; local.sun_family = AF_UNIX;
            const char name[] = "pico_tongue_live"; memcpy(local.sun_path + 1, name, sizeof(name) - 1);
            if (bind(sock, (sockaddr*)&local, offsetof(sockaddr_un, sun_path) + sizeof(name)) < 0 || listen(sock, 1) < 0) { close(sock); sock = -1; } }
    }
    bool is_live = live.load(std::memory_order_relaxed);
    if (is_live && client < 0 && sock >= 0) client = accept4(sock, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    struct Packet { char magic[4]; unsigned seq; float l, r, o, c; } packet{{'T', 'N', 'G', '2'},
        (seq & 0x3fffffffU) | (active ? 0U : 0x80000000U), left, right, out, active ? 1.f : 0.f};
    static_assert(sizeof(Packet) == 24, "TNG2 packet ABI");
    if (is_live && client >= 0 && send(client, &packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL) != ssize_t(sizeof(packet))) { close(client); client = -1; }

    static int udp = -1; static sockaddr_in pc{}, app{}; static uint64_t pc_seen = 0, app_seen = 0;
    if (udp < 0) {
        udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_in any{}; any.sin_family = AF_INET; any.sin_port = htons(19031); any.sin_addr.s_addr = htonl(INADDR_ANY);
        if (udp >= 0) { int size = 1 << 18; setsockopt(udp, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)); }
        if (udp >= 0 && bind(udp, (sockaddr*)&any, sizeof(any)) < 0) { close(udp); udp = -1; }
    }
    if (udp < 0) return;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = uint64_t(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
    for (int i = 0; i < 8; i++) {
        char req[32]; sockaddr_in from{}; socklen_t len = sizeof(from);
        ssize_t n = recvfrom(udp, req, sizeof(req), MSG_DONTWAIT, (sockaddr*)&from, &len);
        if (n < 0) break;
        if (n < 4 || from.sin_family != AF_INET) continue;
        uint32_t a = ntohl(from.sin_addr.s_addr);
        bool loopback = (a >> 24) == 127;
        bool lan = (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8;   // local networks only
        if (!memcmp(req, "TSUB", 4) && lan) { pc = from; pc_seen = now; }
        else if (!memcmp(req, "TCAM", 4) && loopback) { app = from; app_seen = now; }
        else if (!memcmp(req, "TCFG", 4) && loopback && n >= 20) {
            float l, c, r, area; memcpy(&l, req + 4, 4); memcpy(&c, req + 8, 4); memcpy(&r, req + 12, 4); memcpy(&area, req + 16, 4);
            if (valid_calibration(l, c, r, int(area))) { save_calibration(l, c, r, int(area)); load_calibration(); }
        }
        else if (!memcmp(req, "TRST", 4) && loopback) { unlink(kCalibration); load_calibration(); }
        // Retraining (tongue_train.h): TREC <label> records the following frames under that label
        // (0 stops), TCLR drops the recording, TTRN starts training, TSTD restores the shipped model
        // and calibration.
        else if (!memcmp(req, "TREC", 4) && loopback && n >= 5 && uint8_t(req[4]) <= tongue_train::RIGHT) tongue_train::recording = uint8_t(req[4]);
        else if (!memcmp(req, "TCLR", 4) && loopback) tongue_train::clear_recording();
        else if (!memcmp(req, "TTRN", 4) && loopback) tongue_train::start();
        else if (!memcmp(req, "TSTD", 4) && loopback) { tongue_train::restore_standard(); unlink(kCalibration); load_calibration(); }
    }
    if (is_live && pc_seen && now - pc_seen < 3000000000ULL)
        sendto(udp, &packet, sizeof(packet), MSG_DONTWAIT, (sockaddr*)&pc, sizeof(pc));
    if (app_seen && now - app_seen < 3000000000ULL) {
        static unsigned char buf[sizeof(FrameHeader) + 200 * 200 + 56 * 56];
        const auto& cal = tongue_seg::calibration;
        uint32_t rec_count;
        { std::lock_guard<std::mutex> lg(tongue_train::lock); rec_count = uint32_t(tongue_train::samples.size()); }
        FrameHeader h{{'T', 'F', 'R', '2'}, seq, uint32_t((g.present ? 1 : 0) | (g.raw ? 2 : 0) | (active ? 4 : 0)),
                      g.area, g.raw_angle, g.position, left, right, out, g.nose_x, g.nose_y, g.nose,
                      cal.left, cal.center, cal.right, cal.min_area,
                      tongue_train::personal.load() ? 1u : 0u, tongue_train::state.load(), tongue_train::progress.load(),
                      tongue_train::old_score.load(), tongue_train::new_score.load(),
                      tongue_train::recording.load(), rec_count, 0};
        memcpy(buf, &h, sizeof(h)); memcpy(buf + sizeof(h), frame, 200 * 200);
        unsigned char* m = buf + sizeof(h) + 200 * 200;
        for (int i = 0; i < 56 * 56; i++) m[i] = (unsigned char)(255.f / (1.f + std::exp(-logits[i])));
        sendto(udp, buf, sizeof(buf), MSG_DONTWAIT, (sockaddr*)&app, sizeof(app));
    }
}

static void* worker(void*) {
    // Normal time-sharing priority (explicit, never inherited from the tracking thread).
    int tid = int(syscall(SYS_gettid));
    setpriority(PRIO_PROCESS, tid, 0);
    // The tracking service is confined to cpuset /foreground (little cores 0-3). Move only this
    // thread to the root cpuset and pin it to big cores 4-7 so it never competes with tracking.
    int moved = -1, pinned = -1;
    int cs = open("/dev/cpuset/tasks", O_WRONLY | O_CLOEXEC);
    if (cs >= 0) { moved = dprintf(cs, "%d\n", tid) > 0 ? 0 : -1; close(cs); }
    cpu_set_t set; CPU_ZERO(&set); for (int c = 4; c < 8; c++) CPU_SET(c, &set);
    pinned = sched_setaffinity(tid, sizeof(set), &set);
    {
        int f = picoet_open_root_file("/data/local/tmp/tongue-shadow.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (f >= 0) { dprintf(f, "# worker tid=%d cpuset_moved=%d big_cores_pinned=%d\n", tid, moved, pinned); close(f); }
    }
    unsigned seen = 0, written_frames = 0, lines = 0;
    unsigned char frame[200 * 200]; static float logits[56 * 56];
    int on = 0, off = 0; bool active = false; float pos = 0, out = 0;
    unsigned idle = 0; uint64_t calibration_checked = 0;
    load_calibration();
    { tongue_seg::Head h; if (tongue_train::load_head(h)) { tongue_seg::active_head = h; tongue_train::personal = true; } }
    for (;;) {
        unsigned s = slot_seq.load(std::memory_order_acquire);
        // Poll 100/s while frames flow; back off to 2/s once disarmed (no new frame for ~2s).
        if (s == seen || (s & 1)) { usleep(++idle > 200 ? 500000 : 10000); continue; }
        idle = 0;
        unsigned long long ns = slot_ns.load(std::memory_order_relaxed);
        memcpy(frame, slot, sizeof(frame));
        if (slot_seq.load(std::memory_order_acquire) != s) continue;   // torn copy, retry
        seen = s;
        if (ns - calibration_checked > 2000000000ULL) { calibration_checked = ns; load_calibration(); }
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
        // 20Hz output stage: on after 2 present frames (100ms), off after 2 absent, ~40ms EMA.
        if (g.present) { on++; off = 0; } else { off++; on = 0; }
        if (!active && on >= 2) active = true;
        if (active && off >= 2) active = false;
        const float alpha = .71f;
        if (active) { if (g.present) pos += (g.position - pos) * alpha; out += (1 - out) * alpha; }
        else { out += (0 - out) * alpha; pos += (0 - pos) * alpha; }
        float left = (pos < 0 ? -pos : 0) * out, right = (pos > 0 ? pos : 0) * out;
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        publish(s / 2, g, active, left, right, out, frame, logits);
        if (lines < 200000) {
            int f = picoet_open_root_file("/data/local/tmp/tongue-shadow.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (f >= 0) { dprintf(f, "%u %llu present=%d area=%d angle=%.3f position=%.3f active=%d left=%.3f right=%.3f out=%.3f ms=%.1f\n",
                                  s / 2, ns / 1000000ULL, g.present, g.area, g.angle, g.position, active, left, right, out, ms);
                          close(f); lines++; }
        }
        if (keep_frames.load(std::memory_order_relaxed) && written_frames < 3000) {
            int f = picoet_open_root_file("/data/local/tmp/tongue-shadow.frames", O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (f >= 0) { if (write(f, frame, sizeof(frame)) == ssize_t(sizeof(frame))) written_frames++; close(f); }
        }
    }
    return nullptr;
}
}

static void tongue_shadow(const void* mat) {
    using namespace tongue_shadow_impl;
    static std::atomic_flag busy = ATOMIC_FLAG_INIT;
    static uint64_t last = 0, last_arm_check = 0; static bool armed = false;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = uint64_t(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
    if (busy.test_and_set()) return;
    struct Unlock { std::atomic_flag& f; ~Unlock() { f.clear(); } } unlock{busy};
    if (now - last_arm_check >= 500000000ULL) {
        last_arm_check = now;
        int arm = picoet_open_root_file("/data/local/tmp/tongue-shadow.arm", O_RDONLY, 0);
        int ip = picoet_open_root_file("/data/local/tmp/tongue-live.ip", O_RDONLY, 0);
        keep_frames = arm >= 0; live = ip >= 0;
        armed = arm >= 0 || ip >= 0; if (arm >= 0) close(arm); if (ip >= 0) close(ip);
    }
    if (!armed || now - last < 50000000ULL) return;
    unsigned char h[96]; iovec dh{h, sizeof(h)}, sh{const_cast<void*>(mat), sizeof(h)};
    if (process_vm_readv(getpid(), &dh, 1, &sh, 1, 0) != sizeof(h)) return;
    int flags, rows, cols; unsigned char* data; size_t stride;
    memcpy(&flags, h, 4); memcpy(&rows, h + 8, 4); memcpy(&cols, h + 12, 4); memcpy(&data, h + 16, 8); memcpy(&stride, h + 80, 8);
    if ((unsigned(flags) & 0xffff0fffU) != 0x42ff0000U || rows != 400 || cols != 400 || stride != 400 || !data) return;
    static unsigned char pixels[160000]; iovec dp{pixels, sizeof(pixels)}, sp{data, sizeof(pixels)};
    if (process_vm_readv(getpid(), &dp, 1, &sp, 1, 0) != sizeof(pixels)) return;
    last = now;
    if (!worker_started.exchange(true)) {
        pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, 1 << 20);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        sched_param sp0{}; pthread_attr_setschedpolicy(&attr, SCHED_OTHER); pthread_attr_setschedparam(&attr, &sp0);
        if (pthread_create(&t, &attr, worker, nullptr) != 0) worker_started = false;
        pthread_attr_destroy(&attr);
    }
    // seqlock: odd while writing
    unsigned s = slot_seq.load(std::memory_order_relaxed) + 1;
    slot_seq.store(s, std::memory_order_release);
    for (unsigned y = 0; y < 200; y++) for (unsigned x = 0; x < 200; x++) {
        const unsigned char* p = pixels + (2 * y) * 400 + 2 * x;
        slot[y * 200 + x] = (unsigned char)((p[0] + p[1] + p[400] + p[401] + 2) >> 2);
    }
    slot_ns.store(now, std::memory_order_relaxed);
    slot_seq.store(s + 1, std::memory_order_release);
}
