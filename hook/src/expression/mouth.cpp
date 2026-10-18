#include "mouth.h"

#include <cmath>
#include <cstring>

namespace expression_independent {
namespace {

// Stock 0x360cac: all and only the 33 mouth slots, in mouth33 order.
constexpr unsigned slots[kMouthChannels] = {
    43,29,37,25,32,7,15,5,49,23,27,40,10,24,50,6,20,
    22,34,48,18,14,17,9,19,21,33,42,39,8,1,13,51,
};

float clamp01(float value) {
    if (value > 1.0f) value = 1.0f;
    if (value < 0.0f) value = 0.0f;
    return value;
}

float alpha_dt(double stock_alpha, double ratio) {
    return static_cast<float>(-std::expm1(std::log1p(-stock_alpha) * ratio));
}

float baseline_step(float previous, float current, double ratio) {
    if (ratio == 1.0) return (current + previous) * 0.5f;
    const float alpha = alpha_dt(0.5f, ratio);
    const float current_part = current * alpha;
    const float previous_part = previous * (1.0f - alpha);
    return current_part + previous_part;
}

void postprocess(MouthState& state, const float raw35[kMouthRawChannels],
                 double ratio, float mouth[kMouthChannels]) {
    std::memcpy(mouth, raw35, sizeof(float) * kMouthChannels);
    // 0x62108..0x62244: comparisons and arithmetic are float, not double.
    for (unsigned i = 0; i < 32; ++i) {
        const float center = i == 5 ? -0.04f : -0.02f;
        if (std::fabs(mouth[i] + center) < 0.02f)
            state.baseline[i] = baseline_step(state.baseline[i], mouth[i], ratio);
        if (std::fabs(state.baseline[i]) < 0.1f) mouth[i] = mouth[i] - state.baseline[i];
        if (i == 7) {
            float correction = state.baseline[i] / 0.04f;
            correction = correction * 0.02f;
            correction = correction + 0.08f;
            correction = std::fmin(correction, 0.1f);
            mouth[i] = mouth[i] + correction;
        }
    }
    float low = 100000.0f, high = -100000.0f;
    for (unsigned i = 0; i < kMouthChannels; ++i) {
        low = low < mouth[i] ? low : mouth[i];
        high = high > mouth[i] ? high : mouth[i];
    }
    float denominator = high - low;
    denominator = denominator + 1e-10f; // 0x35e440: f32, not the eye f64 epsilon.
    for (unsigned i = 1; i < kMouthChannels; ++i) {
        float scale = mouth[i] - low;
        scale = scale / denominator;
        scale = scale + 0.1f;
        scale = ::log10f(scale);
        scale = scale * 0.6f;
        scale = scale + 1.0f;
        mouth[i] = mouth[i] * scale;
    }
    if (mouth[32] < 0.6f) mouth[32] = 0.0f;
    else if (mouth[32] > 0.6f) mouth[32] = mouth[32] * 1.5f;
}

float adaptive(float previous, float current, double ratio) {
    const float weight = ::expf(std::fabs(previous - current) * -6.0f);
    const float stock_alpha = 1.0f - weight;
    const float alpha = ratio == 1.0 ? stock_alpha : alpha_dt(stock_alpha, ratio);
    const float previous_part = previous * (ratio == 1.0 ? weight : 1.0f - alpha);
    const float current_part = current * alpha;
    return previous_part + current_part;
}

float adaptive_double(float previous, float current, double ratio) {
    // Both stock passes use the SAME previous final output. The second pole is
    // recomputed from the first result, not a separately stored cascade state.
    const float weight1 = ::expf(std::fabs(previous - current) * -6.0f);
    const float alpha1 = 1.0f - weight1;
    const float old1 = previous * weight1;
    const float new1 = current * alpha1;
    const float middle = old1 + new1;
    const float weight2 = ::expf(std::fabs(middle - previous) * -6.0f);
    const float alpha2 = 1.0f - weight2;
    if (ratio == 1.0) {
        const float new2 = middle * alpha2;
        const float old2 = previous * weight2;
        return old2 + new2;
    }
    // Normalize this actual composite step once. Scaling both passes first
    // would make the small-step response proportional to ratio squared.
    const float alpha = alpha_dt(static_cast<double>(alpha1) * alpha2, ratio);
    const float old_part = previous * (1.0f - alpha);
    const float new_part = current * alpha;
    return old_part + new_part;
}

float tongue(float previous, float current, double ratio) {
    const float alpha = ratio == 1.0 ? 0.6f : alpha_dt(0.6f, ratio);
    const float current_part = current * alpha;
    // 0x35e44c=0x3ecccccd, NOT the eye composite's 0x3ecccccc.
    const float previous_part = previous * (ratio == 1.0 ? 0.4f : 1.0f - alpha);
    return current_part + previous_part;
}

float quality(MouthState& state, float probability, uint64_t source_ns, bool stock_step) {
    auto& window = state.quality;
    window.window_limit_ns = uint64_t(state.window_samples) * kSourceDtBaseNs;
    window.threshold_ns = uint64_t(state.threshold_samples) * kSourceDtBaseNs;
    window.source_ns = source_ns;
    window.source_dt_ns = state.diagnostics.updates ? source_ns - state.last_source_ns : 0;
    window.observation_evidence_ns = stock_step ? kSourceDtBaseNs :
        !state.diagnostics.updates ? 0 : window.source_dt_ns <= quality_max_contiguous_gap_ns
            ? window.source_dt_ns : kSourceDtBaseNs;
    window.paused_ns = window.source_dt_ns > window.observation_evidence_ns
        ? window.source_dt_ns - window.observation_evidence_ns : 0;
    window.paused_total_ns += window.paused_ns;
    const bool good = probability > 0.1f; // raw35[34] is not consulted by stock.
    const uint64_t begin = window.effective_clock_ns;
    window.effective_clock_ns += window.observation_evidence_ns;
    if (window.observation_evidence_ns) {
        if (!window.evidence.empty() && window.evidence.back().good == good)
            window.evidence.back().end_ns = window.effective_clock_ns;
        else window.evidence.push_back({begin, window.effective_clock_ns, good});
        (good ? window.good_ns : window.bad_ns) += window.observation_evidence_ns;
    }
    if (window.effective_clock_ns - window.window_start_ns > window.window_limit_ns)
        window.window_start_ns = window.effective_clock_ns - window.window_limit_ns;
    while (!window.evidence.empty() && window.evidence.front().end_ns <= window.window_start_ns) {
        const auto& oldest = window.evidence.front();
        (oldest.good ? window.good_ns : window.bad_ns) -= oldest.end_ns - oldest.begin_ns;
        window.evidence.pop_front();
    }
    if (!window.evidence.empty() && window.evidence.front().begin_ns < window.window_start_ns) {
        auto& oldest = window.evidence.front();
        (oldest.good ? window.good_ns : window.bad_ns) -= window.window_start_ns - oldest.begin_ns;
        oldest.begin_ns = window.window_start_ns;
    }
    window.window_span_ns = window.effective_clock_ns - window.window_start_ns;
    window.last_source_ns = source_ns;
    window.has_source = true;
    window.advanced = true;
    window.update = QualityTimeUpdate::Updated;
    state.diagnostics.classification = window.good_ns > window.threshold_ns ? 1 :
        window.bad_ns > window.threshold_ns ? 0 : -1;
    return state.diagnostics.classification == 0 ? 0.0f : 1.0f;
}

float process(MouthState& state, const float raw35[kMouthRawChannels],
              uint64_t source_ns, float expression52[52], bool stock_step) {
    auto& diagnostics = state.diagnostics;
    if (diagnostics.updates && source_ns <= state.last_source_ns) {
        diagnostics.update = source_ns == state.last_source_ns ? SourceUpdate::Repeated : SourceUpdate::Backward;
        for (unsigned i = 0; i < kMouthChannels; ++i) expression52[slots[i]] = state.previous[i];
        return 0.0f;
    }
    diagnostics.update = SourceUpdate::Updated;
    diagnostics.ratio = stock_step || !diagnostics.updates ? 1.0 :
        static_cast<double>(source_ns - state.last_source_ns) / kSourceDtBaseNs;
    diagnostics.warmup = diagnostics.updates < 2 ||
        (!stock_step && source_ns - state.first_source_ns < 2 * kSourceDtBaseNs);
    const float valid = quality(state, raw35[33], source_ns, stock_step);
    float mouth[kMouthChannels];
    postprocess(state, raw35, diagnostics.ratio, mouth);
    for (unsigned i = 0; i < kMouthChannels; ++i) {
        float value = clamp01(mouth[i]);
        const unsigned slot = slots[i];
        if (!diagnostics.warmup) {
            if (slot == 51) value = tongue(state.previous[i], value, diagnostics.ratio);
            else if (slot == 7 || slot == 19 || slot == 21 || slot == 24 || slot == 48)
                value = adaptive_double(state.previous[i], value, diagnostics.ratio);
            else value = adaptive(state.previous[i], value, diagnostics.ratio);
        }
        state.previous[i] = clamp01(value);
        expression52[slot] = state.previous[i];
    }
    if (!diagnostics.updates) state.first_source_ns = source_ns;
    state.last_source_ns = source_ns;
    ++diagnostics.updates;
    return valid;
}

} // namespace

float process_mouth_dt(MouthState& state, const float raw35[kMouthRawChannels],
                       uint64_t source_ns, float expression52[52]) {
    return process(state, raw35, source_ns, expression52, false);
}

float process_mouth_step(MouthState& state, const float raw35[kMouthRawChannels],
                         uint64_t source_ns, float expression52[52]) {
    return process(state, raw35, source_ns, expression52, true);
}

void reset_mouth_quality(MouthState& state) {
    auto& window = state.quality;
    window.evidence.clear();
    window.window_start_ns = window.effective_clock_ns;
    window.window_span_ns = window.good_ns = window.bad_ns = 0;
}

} // namespace expression_independent
