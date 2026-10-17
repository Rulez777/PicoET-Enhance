#include "numerical.h"

#include <math.h>
#include <float.h>
#include <string.h>

namespace expression_independent {
namespace {

template<class T> T entry(uintptr_t base, uintptr_t offset) {
    return reinterpret_cast<T>(base + offset);
}

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
    explicit NativeMat(uintptr_t plugin_base) : base(plugin_base) {
        entry<ObjectCall>(base, StockLayout::mat_construct)(bytes);
    }
    NativeMat(uintptr_t plugin_base, const float *measurement) : base(plugin_base) {
        entry<MatExternal>(base, StockLayout::mat_external)(
            bytes, 2, 1, 5, const_cast<float *>(measurement), sizeof(float));
    }
    ~NativeMat() { entry<ObjectCall>(base, StockLayout::mat_destroy)(bytes); }
    NativeMat(const NativeMat &) = delete;
    NativeMat &operator=(const NativeMat &) = delete;
};

float clamp01(float value) {
    // Stock 0x61c88/0x61f74 use comparisons, not fmin/fmax NaN replacement.
    if (value > 1.0f) value = 1.0f;
    if (value < 0.0f) value = 0.0f;
    return value;
}

float blink_endpoint(float value) {
    // 0x62568..0x62594 compares float converted to DOUBLE constants.
    // Strict thresholds matter: float(0.04) is below double(0.04), while
    // float(0.8) is above double(0.8).
    if (static_cast<double>(value) < 0.04) value = 0.0f;
    if (static_cast<double>(value) > 0.8) value = 1.0f;
    return value;
}

void postprocess_one(const float input[kChannelsPerEye], float output[kChannelsPerEye]) {
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

float adaptive(float previous, float current) {
    const float weight = expf(-9.0f * fabsf(previous - current));
    const float complement = 1.0f - weight;
    const float old_part = previous * weight;
    const float new_part = current * complement;
    return old_part + new_part;
}

float motion_weight(const float raw[kChannelsPerEye],
                    const float earlier[kChannelsPerEye]) {
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

float composite(float previous, float current, float motion) {
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

void filter_one(EyeHistory &history, const float raw[kChannelsPerEye],
                const float postprocessed[kChannelsPerEye],
                float output[kChannelsPerEye], BrowPredict predict, void *context) {
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

float adaptive_dt(float previous, float current, double ratio) {
    if (ratio == 1.0) return adaptive(previous, current);
    const float weight = static_cast<float>(exp(-9.0 *
        static_cast<double>(fabsf(previous - current)) * ratio));
    const float complement = 1.0f - weight;
    const float old_part = previous * weight;
    const float new_part = current * complement;
    return old_part + new_part;
}

float composite_dt(float previous, float current, float motion, double ratio) {
    if (ratio == 1.0) return composite(previous, current, motion);
    constexpr float stock_current_weight = 0x1.333334p-1f;
    const double stock_alpha = static_cast<double>(stock_current_weight) *
        static_cast<double>(stock_current_weight) * (1.0 - static_cast<double>(motion));
    const float alpha = static_cast<float>(-expm1(log1p(-stock_alpha) * ratio));
    const float new_part = current * alpha;
    const float old_part = previous * (1.0f - alpha);
    return new_part + old_part;
}

void filter_one_dt(SourceDtEyeHistory &history, const float raw[kChannelsPerEye],
                   const float postprocessed[kChannelsPerEye],
                   float output[kChannelsPerEye], uint64_t source_ns,
                   BrowPredictDt predict, void *context) {
    const auto &last = history.diagnostics;
    if (last.filter_steps && source_ns <= last.last_source_ns) {
        history.diagnostics.update = source_ns == last.last_source_ns
            ? SourceUpdate::Repeated : SourceUpdate::Backward;
        memcpy(output, history.outputs.back().values, sizeof(float) * kChannelsPerEye);
        return;
    }

    auto diagnostics = last;
    diagnostics.update = SourceUpdate::Updated;
    diagnostics.ratio = last.filter_steps
        ? static_cast<double>(source_ns - last.last_source_ns) / kSourceDtBaseNs : 1.0;
    diagnostics.warmup = last.filter_steps < 2 ||
        source_ns - history.first_source_ns < kSourceDtMotionNs;
    diagnostics.motion = 0.0f;
    diagnostics.motion_age_ns = 0;

    const uint64_t boundary = source_ns >= kSourceDtMotionNs
        ? source_ns - kSourceDtMotionNs : 0;
    float current[kChannelsPerEye];
    for (unsigned i = 0; i < kChannelsPerEye; ++i) current[i] = clamp01(postprocessed[i]);
    if (diagnostics.warmup) {
        memcpy(output, current, sizeof(current));
    } else {
        // Choose the actual past output nearest t-2*T0. Always rounding toward
        // older samples turns the recorded 77.8ms two-frame history into 122.2ms
        // on some 33/44ms input intervals and unnecessarily freezes brow motion.
        // Both candidates precede this observation; no interpolation/lookahead.
        size_t earlier = 0;
        while (earlier + 1 < history.outputs.size() &&
               history.outputs[earlier + 1].source_ns <= boundary) ++earlier;
        if (earlier + 1 < history.outputs.size() && history.outputs[earlier].source_ns <= boundary &&
            history.outputs[earlier + 1].source_ns - boundary <
                boundary - history.outputs[earlier].source_ns) ++earlier;
        const auto &sample = history.outputs[earlier];
        diagnostics.motion_age_ns = source_ns - sample.source_ns;
        diagnostics.motion = motion_weight(raw, sample.values);
        for (unsigned i = BrowDown; i <= BrowOuterUp; ++i)
            output[i] = composite_dt(history.outputs.back().values[i], current[i],
                                     diagnostics.motion, diagnostics.ratio);
        for (unsigned i = Blink; i < kChannelsPerEye; ++i)
            output[i] = adaptive_dt(history.outputs.back().values[i], current[i], diagnostics.ratio);
        const float measurement[2] = {output[BrowInnerUp], output[BrowOuterUp]};
        float prediction[2];
        BrowStepDiagnostics brow;
        predict(context, measurement, prediction, diagnostics.ratio, brow);
        output[BrowInnerUp] = prediction[0];
        output[BrowOuterUp] = prediction[1];
        ++diagnostics.kalman_steps;
        diagnostics.process_noise = brow.process_noise;
        diagnostics.process_noise_limited = brow.process_noise_limited;
        if (brow.process_noise_limited) ++diagnostics.process_noise_limit_steps;
    }
    for (unsigned i = 0; i < kChannelsPerEye; ++i) output[i] = clamp01(output[i]);

    TimedEyeOutput sample;
    sample.source_ns = source_ns;
    memcpy(sample.values, output, sizeof(sample.values));
    history.outputs.push_back(sample);
    while (history.outputs.size() > 1 && history.outputs[1].source_ns <= boundary)
        history.outputs.pop_front();
    if (!last.filter_steps) history.first_source_ns = source_ns;
    diagnostics.last_source_ns = source_ns;
    ++diagnostics.filter_steps;
    history.diagnostics = diagnostics;
}

float brow_process_noise(double ratio, bool &limited) {
    // q/R = K^2/(1-K), K=1-(1-K0)^r, K0=(sqrt(5)-1)/2.
    // Compute the DECAYING pole instead of exp(+r) or 1-rounded-to-one K.
    // A pole below FLT_EPSILON puts gain within one machine epsilon of unity.
    // Keep one epsilon of damping so q and Ppre remain finite; the maximum q is
    // float(.1*(1-eps)^2/eps) ~= 838860.625, not FLT_MAX or a silent r=1 clamp.
    if (ratio == 1.0) { limited = false; return 0.1f; }
    constexpr double log_pole = -0.9624236501192069;
    double pole = exp(log_pole * ratio);
    limited = pole < static_cast<double>(FLT_EPSILON);
    if (limited) pole = static_cast<double>(FLT_EPSILON);
    const double gain = limited ? 1.0 - pole : -expm1(log_pole * ratio);
    return static_cast<float>(0.1 * gain * gain / pole);
}

float *matrix_row(void *matrix, unsigned row) {
    // Stock CV_32F Mat: data at +0x10, row-step pointer at +0x48.
    unsigned char *data;
    const size_t *steps;
    memcpy(&data, static_cast<unsigned char *>(matrix) + 0x10, sizeof(data));
    memcpy(&steps, static_cast<unsigned char *>(matrix) + 0x48, sizeof(steps));
    return reinterpret_cast<float *>(data + row * steps[0]);
}

} // namespace

void postprocess_eye20(const float input[kEyeChannels], float output[kEyeChannels]) {
    float snapshot[kEyeChannels];
    memcpy(snapshot, input, sizeof(snapshot));
    for (unsigned eye = 0; eye < kEyeCount; ++eye) {
        const unsigned offset = eye * kChannelsPerEye;
        postprocess_one(snapshot + offset, output + offset);
    }
}

void reset_filter(FilterState &state) {
    state = {};
}

void filter_eye20(FilterState &state, const float raw[kEyeChannels],
                  const float postprocessed[kEyeChannels],
                  float filtered[kEyeChannels], BrowPredict predict,
                  void *const contexts[kEyeCount]) {
    float raw_snapshot[kEyeChannels], post_snapshot[kEyeChannels];
    memcpy(raw_snapshot, raw, sizeof(raw_snapshot));
    memcpy(post_snapshot, postprocessed, sizeof(post_snapshot));
    for (unsigned eye = 0; eye < kEyeCount; ++eye) {
        const unsigned offset = eye * kChannelsPerEye;
        filter_one(state.eye[eye], raw_snapshot + offset, post_snapshot + offset,
                   filtered + offset, predict, contexts[eye]);
    }
}

void process_eye20(FilterState &state, const float raw[kEyeChannels],
                   float filtered[kEyeChannels], BrowPredict predict,
                   void *const contexts[kEyeCount]) {
    float postprocessed[kEyeChannels];
    postprocess_eye20(raw, postprocessed);
    filter_eye20(state, raw, postprocessed, filtered, predict, contexts);
}

void reset_source_dt(SourceDtState &state) {
    state = {};
}

void filter_eye20_dt(SourceDtState &state, const float raw[kEyeChannels],
                     const float postprocessed[kEyeChannels],
                     float filtered[kEyeChannels], const uint64_t source_ns[kEyeCount],
                     BrowPredictDt predict, void *const contexts[kEyeCount]) {
    float raw_snapshot[kEyeChannels], post_snapshot[kEyeChannels];
    memcpy(raw_snapshot, raw, sizeof(raw_snapshot));
    memcpy(post_snapshot, postprocessed, sizeof(post_snapshot));
    for (unsigned eye = 0; eye < kEyeCount; ++eye) {
        const unsigned offset = eye * kChannelsPerEye;
        filter_one_dt(state.eye[eye], raw_snapshot + offset, post_snapshot + offset,
                       filtered + offset, source_ns[eye], predict, contexts[eye]);
    }
}

float writeback_expression52(const float filtered[kEyeChannels], float expression52[52]) {
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

NativeBrowFilter::NativeBrowFilter(uintptr_t plugin_base) : base_(plugin_base) {
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

void NativeBrowFilter::destroy() {
    for (uintptr_t offset = StockLayout::kalman_bytes; offset != 0;) {
        offset -= StockLayout::mat_bytes;
        entry<ObjectCall>(base_, StockLayout::mat_destroy)(kalman_ + offset);
    }
}

NativeBrowFilter::~NativeBrowFilter() {
    destroy();
}

void NativeBrowFilter::predict(void *context, const float measurement[2], float prediction[2]) {
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

void NativeBrowFilter::predict_dt(void *context, const float measurement[2], float prediction[2],
                                 double ratio, BrowStepDiagnostics &diagnostics) {
    auto &filter = *static_cast<NativeBrowFilter *>(context);
    BrowStepDiagnostics step;
    step.process_noise = brow_process_noise(ratio, step.process_noise_limited);
    NativeMat input(filter.base_, measurement);
    NativeMat empty_control(filter.base_);
    for (unsigned row = 0; row < 2; ++row) {
        float *noise = matrix_row(filter.kalman_ + StockLayout::kalman_process_noise, row);
        for (unsigned col = 0; col < 2; ++col)
            noise[col] = row == col ? step.process_noise : 0.0f;
        // Keep the first actual correct's stock zero Ppre. On later steps,
        // replace the previous predict's Q interval by THIS source interval.
        if (filter.dt_corrected_) {
            float *prior = matrix_row(filter.kalman_ + StockLayout::kalman_error_pre, row);
            const float *posterior = matrix_row(filter.kalman_ + StockLayout::kalman_error_post, row);
            for (unsigned col = 0; col < 2; ++col)
                prior[col] = posterior[col] + (row == col ? step.process_noise : 0.0f);
        }
    }
    entry<KalmanStep>(filter.base_, StockLayout::kalman_correct)(filter.kalman_, input.bytes);
    const void *result = entry<KalmanStep>(filter.base_, StockLayout::kalman_predict)(
        filter.kalman_, empty_control.bytes);
    const float *data;
    memcpy(&data, static_cast<const unsigned char *>(result) + 0x10, sizeof(data));
    memcpy(prediction, data, 2 * sizeof(float));
    filter.dt_corrected_ = true;
    diagnostics = step;
}

} // namespace expression_independent
