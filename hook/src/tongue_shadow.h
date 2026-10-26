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
// On-headset tongue model. Two root-owned opt-in switches:
//   /data/local/tmp/tongue-live.ip     LIVE: TNG2 packets over Wi-Fi UDP 19031 to the subscribing
//                                      UnifiedPicoModule (and abstract socket pico_tongue_live for ADB)
//   /data/local/tmp/tongue-shadow.arm  SHADOW: also keep raw frames for offline review
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

// TNG2 sender, same packet ABI as the old tongue_live (UnifiedPicoModule TongueReceiver).
static void send_packet(unsigned seq, bool active, float left, float right, float out) {
    static int sock = -1, client = -1;
    if (sock < 0) {
        sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (sock >= 0) { sockaddr_un local{}; local.sun_family = AF_UNIX;
            const char name[] = "pico_tongue_live"; memcpy(local.sun_path + 1, name, sizeof(name) - 1);
            if (bind(sock, (sockaddr*)&local, offsetof(sockaddr_un, sun_path) + sizeof(name)) < 0 || listen(sock, 1) < 0) { close(sock); sock = -1; } }
    }
    if (client < 0 && sock >= 0) client = accept4(sock, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    struct Packet { char magic[4]; unsigned seq; float l, r, o, c; } packet{{'T', 'N', 'G', '2'},
        (seq & 0x3fffffffU) | (active ? 0U : 0x80000000U), left, right, out, active ? 1.f : 0.f};
    static_assert(sizeof(Packet) == 24, "TNG2 packet ABI");
    if (client >= 0 && send(client, &packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL) != ssize_t(sizeof(packet))) { close(client); client = -1; }

    // Wi-Fi channel (no ADB): the PC module sends "TSUB" to UDP 19031 about once a second; the
    // headset replies with TNG2 datagrams to that sender for 3s after its last request. The PC
    // starts the exchange, so a stateful PC firewall admits the replies without any rule.
    static int udp = -1; static sockaddr_in subscriber{}; static uint64_t subscribed = 0;
    if (udp < 0) {
        udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_in any{}; any.sin_family = AF_INET; any.sin_port = htons(19031); any.sin_addr.s_addr = htonl(INADDR_ANY);
        if (udp >= 0 && bind(udp, (sockaddr*)&any, sizeof(any)) < 0) { close(udp); udp = -1; }
    }
    if (udp < 0) return;
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = uint64_t(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
    for (int i = 0; i < 8; i++) {
        char req[16]; sockaddr_in from{}; socklen_t len = sizeof(from);
        ssize_t n = recvfrom(udp, req, sizeof(req), MSG_DONTWAIT, (sockaddr*)&from, &len);
        if (n < 0) break;
        uint32_t a = ntohl(from.sin_addr.s_addr);   // local networks only
        bool lan = (a >> 24) == 10 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8;
        if (n >= 4 && !memcmp(req, "TSUB", 4) && from.sin_family == AF_INET && lan) { subscriber = from; subscribed = now; }
    }
    if (subscribed && now - subscribed < 3000000000ULL)
        sendto(udp, &packet, sizeof(packet), MSG_DONTWAIT, (sockaddr*)&subscriber, sizeof(subscriber));
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
    unsigned idle = 0;
    for (;;) {
        unsigned s = slot_seq.load(std::memory_order_acquire);
        // Poll 100/s while frames flow; back off to 2/s once disarmed (no new frame for ~2s).
        if (s == seen || (s & 1)) { usleep(++idle > 200 ? 500000 : 10000); continue; }
        idle = 0;
        unsigned long long ns = slot_ns.load(std::memory_order_relaxed);
        memcpy(frame, slot, sizeof(frame));
        if (slot_seq.load(std::memory_order_acquire) != s) continue;   // torn copy, retry
        seen = s;
        timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
        tongue_seg::infer(frame, logits);
        auto g = tongue_seg::geometry(frame, logits);
        clock_gettime(CLOCK_MONOTONIC, &b);
        // 20Hz output stage: on after 2 present frames (100ms), off after 2 absent, ~40ms EMA.
        if (g.present) { on++; off = 0; } else { off++; on = 0; }
        if (!active && on >= 2) active = true;
        if (active && off >= 2) active = false;
        const float alpha = .71f;
        if (active) { if (g.present) pos += (g.position - pos) * alpha; out += (1 - out) * alpha; }
        else { out += (0 - out) * alpha; pos += (0 - pos) * alpha; }
        float left = (pos < 0 ? -pos : 0) * out, right = (pos > 0 ? pos : 0) * out;
        double ms = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        if (live.load(std::memory_order_relaxed)) send_packet(s / 2, active, left, right, out);
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
