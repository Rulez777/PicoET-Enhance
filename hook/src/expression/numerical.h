#pragma once

#include <stdint.h>
#include <deque>

// Local PICO OS 5.13.7 numerical reconstruction from stock AArch64 instructions.
// Compile with -ffp-contract=off, without fast-math. No device or hook dependency.
namespace expression_independent {

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

// Input is inference eye24[0..19], BEFORE the quality branch copies Blink.
// Retains stock per-eye brow nonlinearity, inner/outer linkage, Blink/Wide brow
// linkage and Blink endpoints. Directions, Squint and Wide pass through here.
// No pre-clamp: stock 0x62330 also runs before the final-filter input clamp.
// Exact input == output aliasing is supported by all array functions below.
void postprocess_eye20(const float input[kEyeChannels], float output[kEyeChannels]);

struct EyeHistory {
    uint32_t successful_frames; // Saturates at two: stock needs two prior outputs.
    float previous[kChannelsPerEye];
    float earlier[kChannelsPerEye];
};

struct FilterState {
    EyeHistory eye[kEyeCount];
};

// Each context must own a DIFFERENT stock Kalman object, initialized as stock:
// DP=2, MP=2, CP=0, CV_32F; transition/measurement identity, Q=R=0.1 identity.
// Callback calls stock correct(measurement), THEN predict(empty control), and
// copies the two predicted floats to prediction. It is mandatory, not an EMA
// fallback. The first two successful frames do not invoke it (stock warmup).
using BrowPredict = void (*)(void *context, const float measurement[2],
                             float prediction[2]);

// Reset numerical histories AND the caller-owned two Kalman objects together
// before each replay. This function only resets the numerical part.
void reset_filter(FilterState &state);

// raw must be the same pre-quality/pre-postprocess eye20 as postprocess_eye20.
// The composite brow motion weight uses only this eye's raw Blink/directions
// and its earlier FINAL output. Does not advance state for a failed inference:
// the caller invokes this only after a complete successful eye inference.
void filter_eye20(FilterState &state, const float raw[kEyeChannels],
                  const float postprocessed[kEyeChannels],
                  float filtered[kEyeChannels], BrowPredict predict,
                  void *const contexts[kEyeCount]);

// Convenience: postprocess plus independent final filter at the stock cadence.
void process_eye20(FilterState &state, const float raw[kEyeChannels],
                   float filtered[kEyeChannels], BrowPredict predict,
                   void *const contexts[kEyeCount]);

// Separate source-time profile. The r4 entry points/state above remain unchanged.
constexpr uint64_t kSourceDtBaseNs = 41666667;
constexpr uint64_t kSourceDtMotionNs = 2 * kSourceDtBaseNs;

enum class SourceUpdate : unsigned { None, Updated, Repeated, Backward };

struct BrowStepDiagnostics {
    float process_noise = 0.1f;
    bool process_noise_limited = false;
};

struct SourceDtEyeDiagnostics {
    SourceUpdate update = SourceUpdate::None;
    uint64_t last_source_ns = 0;
    uint64_t filter_steps = 0; // All accepted new sources, including warmup.
    uint64_t kalman_steps = 0;
    uint64_t motion_age_ns = 0;
    uint64_t process_noise_limit_steps = 0;
    double ratio = 1.0;
    float motion = 0.0f;
    float process_noise = 0.1f; // Last Q submitted to the native dt callback.
    bool process_noise_limited = false;
    bool warmup = true;
};

struct TimedEyeOutput {
    uint64_t source_ns;
    float values[kChannelsPerEye];
};

struct SourceDtEyeHistory {
    uint64_t first_source_ns = 0;
    // Retain the two-T0 horizon and one sample at/before its boundary.
    std::deque<TimedEyeOutput> outputs;
    SourceDtEyeDiagnostics diagnostics;
};

struct SourceDtState {
    SourceDtEyeHistory eye[kEyeCount];
};

using BrowPredictDt = void (*)(void *context, const float measurement[2],
                               float prediction[2], double ratio,
                               BrowStepDiagnostics &diagnostics);

// Reset this state AND recreate both caller-owned native Kalman objects together.
// Do not switch a populated native object between r4 and source-dt profiles.
void reset_source_dt(SourceDtState &state);

// source_ns is the two eye-image source timestamps, not inference completion time.
// Each eye derives dt from its own last successful source. Repeated/backward
// sources return that eye's last output without advancing histories/Kalman/counts.
// The caller invokes this only for a successful eye24; no mouth/slow-state gating.
// Warmup lasts at least two prior outputs AND two T0 of source time. Nonunit dt
// uses time-normalized approximations; r>1 is not silently clipped to one step.
// Native exceptions propagate, as on the r4 path; discard a partially advanced
// owner after failure rather than retrying it as an equivalent history prefix.
void filter_eye20_dt(SourceDtState &state, const float raw[kEyeChannels],
                     const float postprocessed[kEyeChannels],
                     float filtered[kEyeChannels], const uint64_t source_ns[kEyeCount],
                     BrowPredictDt predict, void *const contexts[kEyeCount]);

// Updates only the 19 stock eye slots in an existing final expression52.
// Returns filtered right inner brow, which the stock 53->52 reorder discards.
// Does not write slots 52/53: those are eye_valid/mouth_valid in the native ABI.
float writeback_expression52(const float filtered[kEyeChannels], float expression52[52]);

// Stock helper call layout for the native adapter, offsets from plugin base.
struct StockLayout {
    static constexpr uintptr_t eye_postprocess = 0x62330;
    static constexpr uintptr_t final_filter = 0x61c10;
    static constexpr uintptr_t kalman_construct = 0x159b90;
    static constexpr uintptr_t kalman_correct = 0x15a528;
    static constexpr uintptr_t kalman_predict = 0x15a260;
    static constexpr uintptr_t kalman_in_face_owner = 0x280;
    static constexpr uintptr_t kalman_bytes = 0x600;
    static constexpr uintptr_t kalman_transition = 0xc0;
    static constexpr uintptr_t kalman_measurement = 0x180;
    static constexpr uintptr_t kalman_process_noise = 0x1e0;
    static constexpr uintptr_t kalman_measurement_noise = 0x240;
    static constexpr uintptr_t kalman_error_pre = 0x2a0;
    static constexpr uintptr_t kalman_error_post = 0x360;
    static constexpr uintptr_t mat_bytes = 0x60;
    static constexpr uintptr_t mat_construct = 0x18dd44;
    static constexpr uintptr_t mat_external = 0x18e710;
    static constexpr uintptr_t mat_destroy = 0x18ed0c;
    static constexpr uintptr_t set_identity = 0x1a1678;
};

// Native adapter for the callback above; construct two objects inside the stock
// Android process, while the plugin remains loaded. Does not create engines,
// operate devices or install hooks. Non-copyable: stock Mat owns native buffers.
class NativeBrowFilter {
public:
    explicit NativeBrowFilter(uintptr_t plugin_base);
    ~NativeBrowFilter();
    NativeBrowFilter(const NativeBrowFilter &) = delete;
    NativeBrowFilter &operator=(const NativeBrowFilter &) = delete;
    static void predict(void *context, const float measurement[2], float prediction[2]);
    static void predict_dt(void *context, const float measurement[2], float prediction[2],
                           double ratio, BrowStepDiagnostics &diagnostics);

private:
    uintptr_t base_;
    alignas(8) unsigned char kalman_[StockLayout::kalman_bytes];
    bool dt_corrected_ = false;
    void destroy();
};

} // namespace expression_independent
