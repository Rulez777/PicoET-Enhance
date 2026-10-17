#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace expression_independent {

constexpr uint64_t quality_nominal_period_ns = 41666667;
constexpr uint64_t quality_max_contiguous_gap_ns = 2 * quality_nominal_period_ns;

enum class QualityTimeUpdate : uint8_t { None, Updated, Repeated, Backward };

struct QualityTimeEvidence {
    // Coordinates are on the per-eye effective observation clock, not source time.
    uint64_t begin_ns;
    uint64_t end_ns;
    bool good;
};

struct QualityTimeWindow {
    // Adjacent real observations support their full interval up to 2*T0.
    // Longer gaps advance only T0; the remaining source time is paused and
    // never enters or ages the effective-clock evidence window.
    std::deque<QualityTimeEvidence> evidence;
    bool has_source = false;
    bool advanced = false;
    bool reference_pending = false;
    QualityTimeUpdate update = QualityTimeUpdate::None;
    uint64_t source_ns = 0; // Current input, including repeated/backward inputs.
    uint64_t last_source_ns = 0; // Last accepted input; zero is a valid source.
    uint64_t source_dt_ns = 0;
    uint64_t observation_evidence_ns = 0;
    uint64_t effective_clock_ns = 0; // Lifetime cumulative evidence duration.
    uint64_t paused_ns = 0; // Current accepted source interval minus evidence.
    uint64_t paused_total_ns = 0; // Lifetime pause duration; retained across capture.
    // Window coordinates and durations below use the effective clock.
    uint64_t window_start_ns = 0;
    uint64_t window_span_ns = 0;
    uint64_t window_limit_ns = 0;
    uint64_t threshold_ns = 0;
    uint64_t good_ns = 0;
    uint64_t bad_ns = 0;
};

struct EyeState {
    int32_t classification = -1;
    uint64_t window_samples = 0;
    int32_t window_good = 0;
    bool reference_present = false;
    uint64_t reference_generation = 0;
    uint64_t reference_update = 0;
    uint64_t successful_updates = 0;
    uint64_t reference_source_ns = 0;
    uint64_t reference_effective_clock_ns = 0;
    uint64_t reference_good_ns = 0; // Capture evidence, retained across clear.
    uint64_t reference_window_span_ns = 0;
    QualityTimeWindow time;
};

// Replays only the stock shared-reference event that resets mouth quality.
// Neither independent eye's quality, reference, or numerical output reads it.
struct MouthQualityCompatibility {
    std::deque<uint8_t> eye_samples[2];
    int32_t eye_good[2] {};
    bool shared_reference_present = false;
};

// Each State belongs to one freshly constructed stock FaceTrackor. The caller
// serializes that owner's calls and removes the State when the owner is destroyed.
struct State {
    EyeState eyes[2];
    const void* frame_current[2] {};
    MouthQualityCompatibility mouth_compatibility;
};

struct Validity {
    bool eye[2];
    float combined;
};

void reset(State& state);

// Call from begin_frame(owner); these borrowed Mat pointers last only through
// the matching end_frame. select_reference records them at the merge hook.
void begin_reference_frame(State& state);

// Call after a normally completed 0x625b4, with the stock shared-reference tail
// bypassed. Reads the stock queues/counters; does not advance or clear them.
Validity observe_quality(State& state, const void* owner);

// Alternative owner-lifetime profile: call for each successful eye24 observation,
// independently of mouth completion. Uses raw24[20/22] and each eye's source clock
// to advance its effective observation clock. First input establishes only the
// source anchor without evidence or effective time. Adjacent real observations
// with dt <= quality_max_contiguous_gap_ns contribute the full dt of current
// quality; longer gaps contribute T0, with the remainder paused outside the window.
// This continuity policy adds no inference, interpolation, or repeated samples.
// Window/threshold are stock +0x100/+0x104 multiplied by nominal T0, not counts
// of fast calls. Evidence and trimming use effective time, so missing time never
// expires old evidence. Repeated/backward inputs change no quality/reference decision;
// backward time requires source catch-up or an explicit fresh-owner reset.
// This is a declared continuity approximation, not physical quality truth or
// stock equivalence. Do not alternate it with observe_quality on the same
// owner's state/history.
Validity observe_quality_time(State& state, const void* owner,
                              const float raw24[24], const uint64_t source_ns[2]);

// For the 0x62f90 hook's second Mat argument. eye is ABI input order, not a
// separately verified physical left/right identity. current is its first Mat.
const void* select_reference(State& state, void* owner, unsigned eye,
                             const void* current);

// Call after observe_quality, before the next trackor call. Inputs are the
// resized, still-live stock Mat objects. Returns a bitmask of captured eyes.
// Clears only the capturing eye's quality queue/count, never output history,
// Kalman, the other eye's window, or the mouth window.
uint32_t finish_references(State& state, uintptr_t base, void* owner,
                           const void* eye0, const void* eye1);

// For end_frame(expression54), after both merge hooks and normal completion.
uint32_t finish_references(State& state, uintptr_t base, void* owner);

// After observe_quality_time, while the supplied current Mats are still alive.
// Consumes only this observation's advanced-eye reference decisions. Capture
// clears only that eye's time evidence, retaining its accepted source anchor,
// cumulative effective clock and cumulative pause duration.
// Never clears a stock queue, mouth compatibility state, or numerical history.
uint32_t finish_references_time(State& state, uintptr_t base, void* owner,
                               const void* eye0, const void* eye1);

// Once per normally completed frame, AFTER observe_quality/finish_references
// and before the next stock trackor call. raw24 is the captured infer result,
// including quality probabilities at 20/22. Failed frames must not call this.
// Returns true if the stock shared-reference event clears owner+0x180/+0x118.
// Does not change this frame's mouth_valid or any independent eye state.
bool preserve_mouth_quality_reset(State& state, uintptr_t base, void* owner,
                                  const float raw24[24]);

} // namespace expression_independent
