#pragma once
// On-device personal retraining of the tongue mask head; the encoder stays frozen. Driven by the
// calibration app over loopback UDP (tongue_shadow.h): the app labels incoming frames (closed
// mouth, open mouth without tongue, tongue centre/left/right), then starts training.
//  - Masks are built automatically: the change against the open-mouth-without-tongue reference
//    (brighter, not glare), joined with the current model's own mask, largest blob only.
//  - Training always starts from the shipped head. Every 4th frame of each pose is held out.
//  - The new head is kept only if it is clearly better than the current head on the held-out
//    frames (more tongue frames seen, no more false tongues, left/right still separate).
//    Otherwise nothing changes. "Restore standard" deletes the personal head and calibration.
#include <algorithm>
#include <atomic>
#include <memory>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "root_file.h"
#include "tongue_seg.h"

namespace tongue_train {
enum Label : uint8_t { NONE = 0, CLOSED = 1, OPEN = 2, CENTER = 3, LEFT = 4, RIGHT = 5 };
enum State : uint32_t { IDLE = 0, RUNNING = 1, ACCEPTED = 2, REJECTED = 3, FAILED = 4 };

struct Sample { std::vector<uint8_t> frame; std::vector<uint8_t> model_mask; uint8_t label; };

#ifndef TONGUE_HEAD_FILE
#define TONGUE_HEAD_FILE "/data/local/tmp/tongue-head.bin"
#endif
static const char kHeadFile[] = TONGUE_HEAD_FILE;
static const size_t kMaxSamples = 360;            // ~15 MB of frames at most

static std::mutex lock;                            // guards samples, pending head, report
static std::vector<Sample> samples;
static std::atomic<uint32_t> recording{NONE};
static std::atomic<uint32_t> state{IDLE};
static std::atomic<float> progress{0}, old_score{0}, new_score{0};
static std::atomic<bool> personal{false}, cancel{false};
static std::unique_ptr<tongue_seg::Head> pending_head;  // worker installs it at the next frame
static bool pending_reset = false;
static tongue_seg::Calibration pending_calibration{};
static bool pending_calibration_valid = false;

// ---------- persistence ----------
static uint32_t checksum(const float* f, size_t n) {
    uint32_t h = 2166136261u; const uint8_t* p = reinterpret_cast<const uint8_t*>(f);
    for (size_t i = 0; i < n * 4; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}
static std::vector<float> flatten(const tongue_seg::Head& h) {
    std::vector<float> v;
    for (auto* part : {&h.w1, &h.b1, &h.w2, &h.b2, &h.w3, &h.b3}) v.insert(v.end(), part->begin(), part->end());
    return v;
}
static bool unflatten(const std::vector<float>& v, tongue_seg::Head& h) {
    if (int(v.size()) != tongue_seg::kHeadFloats) return false;
    size_t o = 0;
    for (auto* part : {&h.w1, &h.b1, &h.w2, &h.b2, &h.w3, &h.b3}) {
        size_t n = part == &h.w1 ? 16 * 89 : part == &h.w2 ? 16 * 16 * 9 : part == &h.b3 ? 1 : 16;
        part->assign(v.begin() + o, v.begin() + o + n); o += n;
    }
    for (float x : v) if (!std::isfinite(x)) return false;
    return true;
}
static bool save_head(const tongue_seg::Head& h) {
    std::vector<float> v = flatten(h);
    char tmp[sizeof(kHeadFile) + 4]; snprintf(tmp, sizeof(tmp), "%s.new", kHeadFile);
    unlink(tmp);
    int f = picoet_open_root_file(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (f < 0) return false;
    uint32_t hdr[2] = {0x31444854u /* "THD1" */, uint32_t(v.size())}, sum = checksum(v.data(), v.size());
    bool ok = write(f, hdr, 8) == 8 && write(f, v.data(), v.size() * 4) == ssize_t(v.size() * 4) && write(f, &sum, 4) == 4;
    close(f);
    return ok && rename(tmp, kHeadFile) == 0;
}
static bool load_head(tongue_seg::Head& h) {
    int f = picoet_open_root_file(kHeadFile, O_RDONLY, 0);
    if (f < 0) return false;
    uint32_t hdr[2] = {}, sum = 0; std::vector<float> v(tongue_seg::kHeadFloats);
    bool ok = read(f, hdr, 8) == 8 && hdr[0] == 0x31444854u && hdr[1] == v.size() &&
              read(f, v.data(), v.size() * 4) == ssize_t(v.size() * 4) && read(f, &sum, 4) == 4 && sum == checksum(v.data(), v.size());
    close(f);
    return ok && unflatten(v, h);
}

// ---------- recording (worker thread) ----------
static void record(const unsigned char* frame, const float* logits) {
    uint32_t label = recording.load(std::memory_order_relaxed);
    if (label == NONE || state.load() == RUNNING) return;
    std::lock_guard<std::mutex> g(lock);
    if (samples.size() >= kMaxSamples) return;
    Sample s; s.label = uint8_t(label);
    s.frame.assign(frame, frame + 200 * 200);
    s.model_mask.resize(56 * 56);
    for (int i = 0; i < 56 * 56; i++) s.model_mask[i] = logits[i] > 0;   // prob > 0.5
    samples.push_back(std::move(s));
}
static void clear_recording() { std::lock_guard<std::mutex> g(lock); samples.clear(); recording = NONE; }
[[maybe_unused]] static int count(uint8_t label) { int n = 0; for (auto& s : samples) n += s.label == label; return n; }

// ---------- automatic masks ----------
static std::vector<uint8_t> largest_blob(const std::vector<uint8_t>& m) {
    std::vector<int> comp(56 * 56, -1); std::vector<int> stack; int best = -1, best_n = 0, id = 0;
    for (int s = 0; s < 56 * 56; s++) {
        if (!m[s] || comp[s] >= 0) continue;
        int n = 0; stack.push_back(s); comp[s] = id;
        while (!stack.empty()) {
            int p = stack.back(); stack.pop_back(); n++;
            int y = p / 56, x = p % 56;
            const int nb[4] = {y > 0 ? p - 56 : -1, y < 55 ? p + 56 : -1, x > 0 ? p - 1 : -1, x < 55 ? p + 1 : -1};
            for (int q : nb) if (q >= 0 && m[q] && comp[q] < 0) { comp[q] = id; stack.push_back(q); }
        }
        if (n > best_n) { best_n = n; best = id; }
        id++;
    }
    std::vector<uint8_t> out(56 * 56, 0);
    for (int i = 0; i < 56 * 56; i++) out[i] = comp[i] == best && best >= 0;
    return out;
}
// Tongue mask for a pose frame: brighter than the open-mouth reference by 20+ grey levels and not
// glare, as 56x56 cells (>half covered), joined with the model's own mask, largest blob.
static std::vector<uint8_t> auto_mask(const Sample& s, const std::vector<float>& open_ref) {
    std::vector<uint8_t> m(56 * 56, 0);
    for (int gy = 0; gy < 56; gy++) for (int gx = 0; gx < 56; gx++) {
        int y0 = gy * 200 / 56, y1 = (gy + 1) * 200 / 56, x0 = gx * 200 / 56, x1 = (gx + 1) * 200 / 56, hit = 0, all = 0;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
            float v = s.frame[y * 200 + x]; all++;
            hit += v < 235 && v - open_ref[y * 200 + x] > 20;
        }
        m[gy * 56 + gx] = hit * 2 > all || s.model_mask[gy * 56 + gx];
    }
    return largest_blob(m);
}

// ---------- training ----------
struct Grad { std::vector<float> w1, b1, w2, b2, w3, b3; };
static void zero_like(Grad& g) {
    g.w1.assign(16 * 89, 0); g.b1.assign(16, 0); g.w2.assign(16 * 16 * 9, 0); g.b2.assign(16, 0); g.w3.assign(16, 0); g.b3.assign(1, 0);
}

// Loss = BCE-with-logits (positive weight 3) + Dice, as in train_shadow.py; fills grad, returns loss.
static float loss_and_grad(const tongue_seg::Head& h, tongue_seg::Context& ctx, const uint8_t* mask, Grad& g) {
    const int N = 56 * 56; static float z[56 * 56];
    tongue_seg::head_forward(h, ctx, z);
    float* cat = ctx.cat.d.data(); float* h1 = ctx.h1.d.data(); float* h2 = ctx.h2.d.data();
    double I = 0, S = 0, Y = 0, bce = 0; static float s[56 * 56], dz[56 * 56];
    for (int p = 0; p < N; p++) {
        s[p] = 1.f / (1.f + std::exp(-z[p])); float y = mask[p];
        I += s[p] * y; S += s[p]; Y += y;
        float sp_pos = std::log1p(std::exp(-std::fabs(z[p]))) + std::max(-z[p], 0.f);   // softplus(-z)
        float sp_neg = sp_pos + z[p];                                                       // softplus(z)
        bce += 3.f * y * sp_pos + (1 - y) * sp_neg;
    }
    double denom = S + Y + 1, dice = 1 - (2 * I + 1) / denom;
    for (int p = 0; p < N; p++) {
        float y = mask[p];
        float d_bce = (-3.f * y * (1 - s[p]) + (1 - y) * s[p]) / N;
        float d_ds = float(-(2 * y * denom - (2 * I + 1)) / (denom * denom));
        dz[p] = d_bce + d_ds * s[p] * (1 - s[p]);
    }
    // layer 3: z = w3 . h2 + b3
    static float da2[16 * 56 * 56], da1[16 * 56 * 56];
    for (int c = 0; c < 16; c++) {
        double acc = 0; const float* hc = h2 + c * N;
        for (int p = 0; p < N; p++) { acc += dz[p] * hc[p]; da2[c * N + p] = hc[p] > 0 ? h.w3[c] * dz[p] : 0; }
        g.w3[c] += float(acc);
    }
    { double acc = 0; for (int p = 0; p < N; p++) acc += dz[p]; g.b3[0] += float(acc); }
    // layer 2: a2 = conv3x3(h1) + b2, pad 1
    std::fill(da1, da1 + 16 * N, 0.f);
    for (int o = 0; o < 16; o++) {
        const float* d = da2 + o * N;
        double bs = 0; for (int p = 0; p < N; p++) bs += d[p]; g.b2[o] += float(bs);
        for (int i = 0; i < 16; i++) {
            const float* hi = h1 + i * N; float* di = da1 + i * N;
            for (int ky = 0; ky < 3; ky++) for (int kx = 0; kx < 3; kx++) {
                int widx = ((o * 16 + i) * 3 + ky) * 3 + kx; float wv = h.w2[widx]; double acc = 0;
                for (int y = 0; y < 56; y++) {
                    int iy = y + ky - 1; if (iy < 0 || iy >= 56) continue;
                    int x0 = kx == 0 ? 1 : 0, x1 = kx == 2 ? 55 : 56;
                    const float* dr = d + y * 56; const float* hr = hi + iy * 56 + kx - 1; float* ir = di + iy * 56 + kx - 1;
                    for (int x = x0; x < x1; x++) { acc += dr[x] * hr[x]; ir[x] += wv * dr[x]; }
                }
                g.w2[widx] += float(acc);
            }
        }
    }
    for (int i = 0; i < 16 * N; i++) if (h1[i] <= 0) da1[i] = 0;   // through ReLU of layer 1
    // layer 1: a1 = w1 . cat + b1
    for (int o = 0; o < 16; o++) {
        const float* d = da1 + o * N;
        double bs = 0; for (int p = 0; p < N; p++) bs += d[p]; g.b1[o] += float(bs);
        for (int i = 0; i < 89; i++) {
            const float* ci = cat + i * N; double acc = 0;
            for (int p = 0; p < N; p++) acc += d[p] * ci[p];
            g.w1[o * 89 + i] += float(acc);
        }
    }
    return float(bce / N + dice);
}

struct Adam {
    std::vector<float> m, v; int t = 0;
    void step(std::vector<float>& w, const std::vector<float>& g, float lr) {
        if (m.empty()) { m.assign(w.size(), 0); v.assign(w.size(), 0); }
        t++; float b1 = .9f, b2 = .999f, c1 = 1 - std::pow(b1, t), c2 = 1 - std::pow(b2, t);
        for (size_t i = 0; i < w.size(); i++) {
            m[i] = b1 * m[i] + (1 - b1) * g[i]; v[i] = b2 * v[i] + (1 - b2) * g[i] * g[i];
            w[i] -= lr * (m[i] / c1) / (std::sqrt(v[i] / c2) + 1e-8f);
        }
    }
};

// ---------- evaluation ----------
struct Eval { float tp = 0, fp = 0, left = 0, center = 0, right = 0; bool direction = false; float score = -9; };
static float median(std::vector<float> v) { if (v.empty()) return NAN; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; }
static Eval evaluate(const tongue_seg::Head& h, const std::vector<const Sample*>& set, tongue_seg::Context& ctx) {
    static float logits[56 * 56];
    const tongue_seg::Calibration fixed{.33f, .66f, 1.11f, 100};   // same yardstick for old and new
    int npos = 0, nneg = 0, tp = 0, fp = 0; std::vector<float> a[6];
    for (const Sample* s : set) {
        tongue_seg::features(ctx, s->frame.data());
        tongue_seg::head_forward(h, ctx, logits);
        auto g = tongue_seg::geometry(s->frame.data(), logits, fixed);
        if (s->label == CLOSED || s->label == OPEN) { nneg++; fp += g.present; }
        else { npos++; tp += g.present; if (g.raw) a[s->label].push_back(g.raw_angle); }
    }
    Eval e; e.tp = npos ? float(tp) / npos : 0; e.fp = nneg ? float(fp) / nneg : 0;
    e.left = median(a[LEFT]); e.center = median(a[CENTER]); e.right = median(a[RIGHT]);
    e.direction = a[LEFT].size() >= 3 && a[CENTER].size() >= 3 && a[RIGHT].size() >= 3 &&
                  std::fabs(e.left - e.center) >= .08f && std::fabs(e.right - e.center) >= .08f && (e.left - e.center) * (e.right - e.center) < 0;
    e.score = e.tp - 2 * e.fp;
    return e;
}

// ---------- the training run (own thread, big cores 5-7) ----------
static void* run(void*) {
    cpu_set_t set; CPU_ZERO(&set); for (int c = 5; c < 8; c++) CPU_SET(c, &set);
    sched_setaffinity(int(syscall(SYS_gettid)), sizeof(set), &set);
    std::vector<Sample> data;
    { std::lock_guard<std::mutex> g(lock); data = samples; }
    auto fail = [] { state = FAILED; return nullptr; };
    std::vector<float> open_ref(200 * 200, 0); int nopen = 0;
    for (auto& s : data) if (s.label == OPEN) { for (int i = 0; i < 200 * 200; i++) open_ref[i] += s.frame[i]; nopen++; }
    if (nopen < 10) return fail();
    for (float& v : open_ref) v /= nopen;
    std::vector<const Sample*> train, hold; std::vector<std::vector<uint8_t>> masks; std::vector<int> train_mask;
    int seen[6] = {};
    for (auto& s : data) {
        bool held_out = (seen[s.label]++ % 4) == 3;
        if (held_out) { hold.push_back(&s); continue; }
        std::vector<uint8_t> m(56 * 56, 0);
        if (s.label >= CENTER) {
            m = auto_mask(s, open_ref);
            int area = 0; for (uint8_t v : m) area += v;
            if (area < 60) continue;   // could not find the tongue in this frame: do not teach from it
        }
        train.push_back(&s); masks.push_back(std::move(m));
    }
    int pos = 0, neg = 0; for (auto* s : train) (s->label >= CENTER ? pos : neg)++;
    if (pos < 20 || neg < 20) return fail();

    tongue_seg::Context ctx;
    tongue_seg::Head old_head;
    if (!load_head(old_head)) old_head = tongue_seg::standard_head();
    Eval before = evaluate(old_head, hold, ctx);
    old_score = before.score;

    tongue_seg::Head h = tongue_seg::standard_head();
    Adam a1, a2, a3, a4, a5, a6; Grad g; std::mt19937 rng(12345);
    std::vector<int> pos_idx, neg_idx;
    for (int i = 0; i < int(train.size()); i++) (train[i]->label >= CENTER ? pos_idx : neg_idx).push_back(i);
    const int steps = 900; const float lr = 1e-3f;
    for (int step = 0; step < steps; step++) {
        zero_like(g);
        for (int b = 0; b < 2; b++) {   // one tongue frame and one no-tongue frame per step
            auto& pool = b == 0 ? pos_idx : neg_idx;
            int i = pool[rng() % pool.size()];
            float gamma = std::exp(std::uniform_real_distribution<float>(-.25f, .25f)(rng));
            tongue_seg::features(ctx, train[i]->frame.data(), gamma);
            loss_and_grad(h, ctx, masks[i].data(), g);
        }
        a1.step(h.w1, g.w1, lr); a2.step(h.b1, g.b1, lr); a3.step(h.w2, g.w2, lr);
        a4.step(h.b2, g.b2, lr); a5.step(h.w3, g.w3, lr); a6.step(h.b3, g.b3, lr);
        progress = .9f * (step + 1) / steps;
    }
    for (float x : flatten(h)) if (!std::isfinite(x)) return fail();
    Eval after = evaluate(h, hold, ctx);
    new_score = after.score;
    progress = .95f;
    bool better = after.direction && after.score >= before.score + .05f && after.fp <= before.fp + .02f;
#ifdef TONGUE_TRAIN_ACCEPT_ANY   // tests only: exercise the accept path
    better = after.direction;
#endif
    if (!better) { state = REJECTED; progress = 1; return nullptr; }

    // Personal calibration from the new head on every recorded frame (same rule as the app).
    static float logits[56 * 56];
    std::vector<float> ang[6]; std::vector<int> negA, posA;
    const tongue_seg::Calibration probe{.33f, .66f, 1.11f, 100};
    for (auto& s : data) {
        tongue_seg::features(ctx, s.frame.data());
        tongue_seg::head_forward(h, ctx, logits);
        auto geo = tongue_seg::geometry(s.frame.data(), logits, probe);
        if (s.label <= OPEN) negA.push_back(geo.raw ? geo.area : 0);
        else if (geo.raw) { ang[s.label].push_back(geo.raw_angle); posA.push_back(geo.area); }
    }
    std::sort(negA.begin(), negA.end()); std::sort(posA.begin(), posA.end());
    int negHigh = negA.empty() ? 0 : negA[std::min(negA.size() - 1, size_t(negA.size() * .95))];
    int posLow = posA.empty() ? 100 : posA[size_t(posA.size() * .05)];
    int area = negHigh + 10 < posLow ? std::max(40, std::min(300, int(negHigh + (posLow - negHigh) * .4f))) : 100;
    tongue_seg::Calibration cal{median(ang[LEFT]), median(ang[CENTER]), median(ang[RIGHT]), area};

    if (cancel.load()) { state = IDLE; return nullptr; }   // user restored the standard meanwhile
#ifndef TONGUE_TRAIN_NO_SAVE
    if (!save_head(h)) return fail();
#endif
    {
        std::lock_guard<std::mutex> lg(lock);
        pending_head.reset(new tongue_seg::Head(h));
        pending_calibration = cal; pending_calibration_valid = true;
    }
    state = ACCEPTED; progress = 1;
    return nullptr;
}

static void start() {
    uint32_t idle = IDLE;
    if (state.load() == RUNNING) return;
    state = RUNNING; progress = 0; cancel = false; (void)idle;
    recording = NONE;
    pthread_t t; pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    sched_param sp0{}; pthread_attr_setschedpolicy(&attr, SCHED_OTHER); pthread_attr_setschedparam(&attr, &sp0);
    if (pthread_create(&t, &attr, run, nullptr) != 0) state = FAILED;
    pthread_attr_destroy(&attr);
}

static void restore_standard() {
    cancel = true;
    unlink(kHeadFile);
    std::lock_guard<std::mutex> g(lock);
    pending_head.reset(); pending_reset = true; samples.clear(); recording = NONE;
    if (state.load() != RUNNING) state = IDLE;
}
}
