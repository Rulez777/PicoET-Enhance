#pragma once

#include "numerical.h"
#include "state.h"

namespace expression_independent {

constexpr unsigned kMouthChannels = 33;
constexpr unsigned kMouthRawChannels = 35;

struct MouthDiagnostics {
    SourceUpdate update = SourceUpdate::None;
    double ratio = 1.0;
    bool warmup = true;
    int32_t classification = -1;
    uint64_t updates = 0;
};

struct MouthState {
    float baseline[32] {};
    float previous[kMouthChannels] {};
    uint64_t first_source_ns = 0, last_source_ns = 0;
    uint32_t window_samples = 30, threshold_samples = 15;
    QualityTimeWindow quality;
    MouthDiagnostics diagnostics;
};

// Both entry points write ONLY the 33 non-eye slots. Raw quality is raw35[33];
// invalid quality does not freeze numerical history. A fresh owner starts with
// zero baselines and two unfiltered successful outputs, as stock 0x61c10.
// Source-dt warmup also spans 2*T0. Gated baseline adaptation and effective-time
// quality are declared approximations, not equality to a differently sampled run.
float process_mouth_dt(MouthState& state, const float raw35[kMouthRawChannels],
                       uint64_t source_ns, float expression52[52]);

// Transcription oracle: force every accepted source to one stock step, including
// count-equivalent quality and two-call warmup. Not a runtime configuration mode.
float process_mouth_step(MouthState& state, const float raw35[kMouthRawChannels],
                         uint64_t source_ns, float expression52[52]);

// Legacy shared-reference capture clears quality AFTER the current validity was
// produced. It does not clear baselines, numerical history or the source anchor.
void reset_mouth_quality(MouthState& state);

} // namespace expression_independent
