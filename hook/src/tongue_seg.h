#pragma once
// Tongue segmentation (shadow_model.Segment) for a 200x200 uint8 frame. Float32 port of the
// PyTorch model: bilinear 200->224, sqrt, ImageNet normalisation, MobileNetV3-small blocks 0..8
// (BN folded), taps of blocks 1/3/8 resized to 56x56, 3-layer mask head -> 56x56 logits.
// Allocation-free after the first call; not thread-safe (one worker thread owns it).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "tongue_seg_weights.h"

namespace tongue_seg {
using tongue_seg_weights::Op;

struct Tensor { int c = 0, h = 0, w = 0; std::vector<float> d;
    void shape(int cc, int hh, int ww) { c = cc; h = hh; w = ww; d.resize(size_t(c) * h * w); }
    float* ch(int k) { return d.data() + size_t(k) * h * w; } };

static inline float act(float v, int a) {
    if (a == 1) return v > 0 ? v : 0;
    if (a == 2) return v * std::min(std::max(v + 3.f, 0.f), 6.f) / 6.f;
    return v;
}

// PyTorch bilinear, align_corners=False, no antialias.
static void resize(const float* src, int sh, int sw, float* dst, int dh, int dw) {
    float sy = float(sh) / dh, sx = float(sw) / dw;
    for (int y = 0; y < dh; y++) {
        float fy = std::max((y + .5f) * sy - .5f, 0.f); int y0 = std::min(int(fy), sh - 1);
        int y1 = std::min(y0 + 1, sh - 1); float ly = fy - y0;
        for (int x = 0; x < dw; x++) {
            float fx = std::max((x + .5f) * sx - .5f, 0.f); int x0 = std::min(int(fx), sw - 1);
            int x1 = std::min(x0 + 1, sw - 1); float lx = fx - x0;
            dst[y * dw + x] = (1 - ly) * ((1 - lx) * src[y0 * sw + x0] + lx * src[y0 * sw + x1]) +
                              ly * ((1 - lx) * src[y1 * sw + x0] + lx * src[y1 * sw + x1]);
        }
    }
}

// Generic conv (pad k/2), grouped. 1x1 dense convs use an output-row-major accumulation that
// clang vectorises over the spatial dimension.
static void conv(Tensor& in, Tensor& out, int cout, int k, int stride, int groups,
                 const float* w, const float* b, int a) {
    int pad = k / 2, oh = (in.h + 2 * pad - k) / stride + 1, ow = (in.w + 2 * pad - k) / stride + 1;
    out.shape(cout, oh, ow);
    int cin_g = in.c / groups, cout_g = cout / groups;
    if (k == 1 && stride == 1 && groups == 1) {
        int n = oh * ow;
        for (int o = 0; o < cout; o++) {
            float* dst = out.ch(o); std::fill(dst, dst + n, b[o]);
            for (int i = 0; i < in.c; i++) {
                float wv = w[o * in.c + i]; const float* s = in.ch(i);
                for (int p = 0; p < n; p++) dst[p] += wv * s[p];
            }
            for (int p = 0; p < n; p++) dst[p] = act(dst[p], a);
        }
        return;
    }
    for (int o = 0; o < cout; o++) {
        int g = o / cout_g; float* dst = out.ch(o);
        std::fill(dst, dst + oh * ow, b[o]);
        for (int i = 0; i < cin_g; i++) {
            const float* s = in.ch(g * cin_g + i);
            const float* wk = w + (size_t(o) * cin_g + i) * k * k;
            for (int ky = 0; ky < k; ky++) for (int kx = 0; kx < k; kx++) {
                float wv = wk[ky * k + kx];
                for (int y = 0; y < oh; y++) {
                    int iy = y * stride + ky - pad; if (iy < 0 || iy >= in.h) continue;
                    const float* srow = s + iy * in.w; float* drow = dst + y * ow;
                    int x0 = 0, x1 = ow;   // valid x range for this kx
                    while (x0 < ow && x0 * stride + kx - pad < 0) x0++;
                    while (x1 > x0 && (x1 - 1) * stride + kx - pad >= in.w) x1--;
                    if (stride == 1) { const float* sp = srow + kx - pad;
                        for (int x = x0; x < x1; x++) drow[x] += wv * sp[x]; }
                    else for (int x = x0; x < x1; x++) drow[x] += wv * srow[x * stride + kx - pad];
                }
            }
        }
        for (int p = 0; p < oh * ow; p++) dst[p] = act(dst[p], a);
    }
}

static void squeeze_excite(Tensor& t, const Op& op, const float* W) {
    int n = t.h * t.w; std::vector<float> m(t.c), s(op.sq);
    for (int c = 0; c < t.c; c++) { double acc = 0; const float* p = t.ch(c); for (int i = 0; i < n; i++) acc += p[i]; m[c] = float(acc / n); }
    for (int q = 0; q < op.sq; q++) { float v = W[op.boff + q]; for (int c = 0; c < t.c; c++) v += W[op.woff + q * t.c + c] * m[c]; s[q] = v > 0 ? v : 0; }
    for (int c = 0; c < t.c; c++) {
        float v = W[op.b2off + c]; for (int q = 0; q < op.sq; q++) v += W[op.w2off + c * op.sq + q] * s[q];
        float g = std::min(std::max(v + 3.f, 0.f), 6.f) / 6.f; float* p = t.ch(c);
        for (int i = 0; i < n; i++) p[i] *= g;
    }
}

// Working buffers. Inference and on-device training each own one, so they never share state.
struct Context { Tensor a, b, res, cat, h1, h2, out; std::vector<float> f200 = std::vector<float>(200 * 200), gray = std::vector<float>(224 * 224); };

// The mask head (89->16 1x1 ReLU, 16->16 3x3 ReLU, 16->1 1x1). The shipped weights live in the
// generated header; a personal head (on-device retraining) replaces it while installed.
struct Head { std::vector<float> w1, b1, w2, b2, w3, b3; };
static const int kHeadFloats = 16 * 89 + 16 + 16 * 16 * 9 + 16 + 16 + 1;
static Head standard_head() {
    using namespace tongue_seg_weights;
    auto take = [](int off, int n) { return std::vector<float>(w + off, w + off + n); };
    return {take(head[0][3], 16 * 89), take(head[0][4], 16), take(head[1][3], 16 * 16 * 9), take(head[1][4], 16),
            take(head[2][3], 16), take(head[2][4], 1)};
}
static Head active_head = standard_head();   // only the worker thread reads or replaces it

// Encoder: frame (200x200 uint8) -> ctx.cat (89x56x56). gamma != 1 is a training augmentation.
static void features(Context& ctx, const unsigned char* frame, float gamma = 1.f) {
    using namespace tongue_seg_weights;
    for (int i = 0; i < 200 * 200; i++) ctx.f200[i] = frame[i] / 255.f;
    if (gamma != 1.f) for (float& v : ctx.f200) v = std::pow(v, gamma);
    resize(ctx.f200.data(), 200, 200, ctx.gray.data(), 224, 224);
    for (float& v : ctx.gray) v = std::sqrt(v);
    ctx.a.shape(3, 224, 224);
    const float mean[3] = {.485f, .456f, .406f}, stdv[3] = {.229f, .224f, .225f};
    for (int c = 0; c < 3; c++) { float* p = ctx.a.ch(c); for (int i = 0; i < 224 * 224; i++) p[i] = (ctx.gray[i] - mean[c]) / stdv[c]; }
    ctx.cat.shape(89, 56, 56);
    resize(ctx.gray.data(), 224, 224, ctx.cat.ch(0), 56, 56);
    int catc = 1;
    Tensor* cur = &ctx.a; Tensor* nxt = &ctx.b;
    for (const Op& op : ops) {
        if (op.type == 0) { conv(*cur, *nxt, op.out, op.k, op.stride, op.groups, w + op.woff, w + op.boff, op.act); std::swap(cur, nxt); }
        else if (op.type == 1) squeeze_excite(*cur, op, w);
        else if (op.type == 2) ctx.res = *cur;
        else if (op.type == 3) { for (size_t i = 0; i < cur->d.size(); i++) cur->d[i] += ctx.res.d[i]; }
        else if (op.type == 4) { for (int c = 0; c < cur->c; c++) resize(cur->ch(c), cur->h, cur->w, ctx.cat.ch(catc + c), 56, 56); catc += cur->c; }
    }
}

// Head on ctx.cat -> 56x56 logits; ctx.h1 / ctx.h2 keep the hidden activations for training.
static void head_forward(const Head& h, Context& ctx, float* logits) {
    conv(ctx.cat, ctx.h1, 16, 1, 1, 1, h.w1.data(), h.b1.data(), 1);
    conv(ctx.h1, ctx.h2, 16, 3, 1, 1, h.w2.data(), h.b2.data(), 1);
    conv(ctx.h2, ctx.out, 1, 1, 1, 1, h.w3.data(), h.b3.data(), 0);
    std::memcpy(logits, ctx.out.d.data(), 56 * 56 * sizeof(float));
}

// frame: 200x200 uint8. logits: 56x56 output. Worker thread only.
static void infer(const unsigned char* frame, float* logits) {
    static Context ctx;
    features(ctx, frame);
    head_forward(active_head, ctx, logits);
}

// shadow_model.geometry + angle_to_position. Returns presence; angle/position valid when true.
// Per-user calibration: tongue angles (radians around the nose glare) for the user's left,
// centre and right, and the minimum mask size. Defaults = the shipped model's own calibration.
// The calibration app measures these per person (tongue_shadow.h loads/saves them).
struct Calibration { float left, center, right; int min_area; };
static Calibration calibration{tongue_seg_weights::angle_left, tongue_seg_weights::angle_center,
                               tongue_seg_weights::angle_right, 100};

// raw: a plausible tongue-shaped mask (>=20 cells, below the nose) with raw_angle, used by the
// calibration app; present: raw and at least calibration.min_area cells.
struct Geometry { bool present; int area; float angle, position; bool raw; float raw_angle, nose_x, nose_y; int nose; };
static Geometry geometry(const unsigned char* frame, const float* logits, const Calibration& cal) {
    Geometry g{false, 0, 0, 0, false, 0, 0, 0, 0};
    double nx = 0, ny = 0; int nose = 0;
    for (int y = 0; y < 200; y++) for (int x = 0; x < 200; x++)
        if (frame[y * 200 + x] / 255.f >= .94f) { nx += x; ny += y; nose++; }
    g.nose = nose;
    if (nose) { g.nose_x = float(nx / nose); g.nose_y = float(ny / nose); }
    double tx = 0, ty = 0, ws = 0;
    for (int y = 0; y < 56; y++) for (int x = 0; x < 56; x++) {
        float p = 1.f / (1.f + std::exp(-logits[y * 56 + x]));
        if (p > .5f) { g.area++; tx += (x + .5) * (200. / 56) * p; ty += (y + .5) * (200. / 56) * p; ws += p; }
    }
    if (g.area < 20 || nose < 50) return g;
    float angle = float(std::atan2(ty / ws - ny / nose, tx / ws - nx / nose));
    if (!(angle > 0.f && angle < 1.7f)) return g;   // level with/above nose: not a tongue
    g.raw = true; g.raw_angle = angle;
    // Default 100 cells: false masks on lips (half-pucker, wide-open, lip-out, closed) were 20-80
    // cells, real tongues 95+ on the author's head-mount; the calibration app may set it per user.
    if (g.area < cal.min_area) return g;
    g.present = true; g.angle = angle;
    const float L = cal.left, C = cal.center, R = cal.right, dead = .06f;
    // Works for either angular order of left/right (mirrored head-mounts).
    float p = (angle - C) * (L - C) > 0 ? -(angle - C) / (L - C) : (angle - C) / (R - C);
    p = std::max(-1.f, std::min(1.f, p));
    g.position = std::fabs(p) < dead ? 0.f : (std::fabs(p) - dead) / (1 - dead) * (p > 0 ? 1.f : -1.f);
    return g;
}
static Geometry geometry(const unsigned char* frame, const float* logits) { return geometry(frame, logits, calibration); }
}
