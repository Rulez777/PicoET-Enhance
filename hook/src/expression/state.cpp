#include "state.h"

#include <cstring>

namespace expression_independent {
namespace {

static_assert(sizeof(void*) == 8, "stock AArch64 pointer ABI");
constexpr size_t reference_offsets[2] = {0x8, 0x68};
constexpr size_t queue_offsets[2] = {0x120, 0x150};
constexpr size_t count_offsets[2] = {0x110, 0x114};

template<class T> T load(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
    return value;
}

template<class T> void store(void* object, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}

// Reproduces the _OutputArray built at 0x62b90..0x62ba8 / 0x62bac..0x62bc0.
// It refers to a stock-constructed Mat; it is not a memcpy of the Mat header.
struct MatOutput {
    int32_t flags;
    int32_t padding;
    void* object;
    uint64_t size;
};
static_assert(sizeof(MatOutput) == 0x18, "stock AArch64 OutputArray ABI");
static_assert(offsetof(MatOutput, object) == 8, "stock OutputArray object offset");

void copy_reference(uintptr_t base, void* destination, const void* current) {
    MatOutput output {0x02010000, 0, destination, 0};
    using CopyTo = void (*)(const void*, const MatOutput*);
    reinterpret_cast<CopyTo>(base + 0x12953c)(current, &output);
}

int32_t advance_legacy_quality(std::deque<uint8_t>& samples, int32_t& good,
                              uint64_t window, int32_t threshold, float probability) {
    // Stock 0x62810..0x62904 removes exactly ONE oldest sample if size >= window.
    // The count subtraction is skipped when the current good count is zero.
    if (samples.size() >= window) {
        if (good > 0) good -= samples.front();
        samples.pop_front();
    }
    // 0x62974 loads float bits 0x3ecccccd; cset gt is a strict comparison.
    const uint8_t sample = probability > 0.4f;
    samples.push_back(sample);
    good += sample;
    if (good > threshold) return 1;
    // Match the unsigned false-count comparison at 0x62a28/0x62a4c.
    const uint64_t bad = samples.size() - static_cast<uint64_t>(int64_t(good));
    return bad > static_cast<uint64_t>(int64_t(threshold)) ? 0 : -1;
}

uint64_t& coverage(QualityTimeWindow& window, bool good) {
    return good ? window.good_ns : window.bad_ns;
}

void advance_time_quality(QualityTimeWindow& window, uint64_t source_ns, bool good) {
    window.source_dt_ns = source_ns - window.last_source_ns;
    window.observation_evidence_ns = window.source_dt_ns <= quality_max_contiguous_gap_ns
        ? window.source_dt_ns : quality_nominal_period_ns;
    window.paused_ns = window.source_dt_ns - window.observation_evidence_ns;
    window.paused_total_ns += window.paused_ns;
    const uint64_t begin_ns = window.effective_clock_ns;
    window.effective_clock_ns += window.observation_evidence_ns;
    if (!window.evidence.empty() && window.evidence.back().end_ns == begin_ns &&
        window.evidence.back().good == good) {
        window.evidence.back().end_ns = window.effective_clock_ns;
    } else {
        window.evidence.push_back({begin_ns, window.effective_clock_ns, good});
    }
    coverage(window, good) += window.observation_evidence_ns;
    if (window.effective_clock_ns - window.window_start_ns > window.window_limit_ns) {
        window.window_start_ns = window.effective_clock_ns - window.window_limit_ns;
    }
    while (!window.evidence.empty() &&
           window.evidence.front().end_ns <= window.window_start_ns) {
        const auto& oldest = window.evidence.front();
        coverage(window, oldest.good) -= oldest.end_ns - oldest.begin_ns;
        window.evidence.pop_front();
    }
    if (!window.evidence.empty() && window.evidence.front().begin_ns < window.window_start_ns) {
        auto& oldest = window.evidence.front();
        coverage(window, oldest.good) -= window.window_start_ns - oldest.begin_ns;
        oldest.begin_ns = window.window_start_ns;
    }
    window.last_source_ns = source_ns;
    window.window_span_ns = window.effective_clock_ns - window.window_start_ns;
}

void clear_time_quality(QualityTimeWindow& window) {
    window.evidence.clear();
    window.window_start_ns = window.effective_clock_ns;
    window.window_span_ns = 0;
    window.good_ns = 0;
    window.bad_ns = 0;
}

} // namespace

void reset(State& state) {
    state = State {};
}

void begin_reference_frame(State& state) {
    state.frame_current[0] = nullptr;
    state.frame_current[1] = nullptr;
    for (auto& value : state.eyes) {
        value.time.advanced = false;
        value.time.reference_pending = false;
        value.time.update = QualityTimeUpdate::None;
    }
}

Validity observe_quality(State& state, const void* owner) {
    const int32_t threshold = load<int32_t>(owner, 0x104);
    Validity validity {};
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& value = state.eyes[eye];
        value.window_samples = load<uint64_t>(owner, queue_offsets[eye] + 0x28);
        value.window_good = load<int32_t>(owner, count_offsets[eye]);
        const int64_t bad = static_cast<int64_t>(value.window_samples) - value.window_good;
        value.classification = value.window_good > threshold ? 1 : bad > threshold ? 0 : -1;
        ++value.successful_updates;
        validity.eye[eye] = value.classification != 0;
    }
    // The stock 0xd8 return has only one eye-group validity float. Preserve its
    // OR policy while exposing both local decisions separately to the caller.
    validity.combined = (validity.eye[0] || validity.eye[1]) ? 1.0f : 0.0f;
    return validity;
}

Validity observe_quality_time(State& state, const void* owner,
                              const float raw24[24], const uint64_t source_ns[2]) {
    const uint64_t window_ns = static_cast<uint64_t>(load<int32_t>(owner, 0x100)) *
                               quality_nominal_period_ns;
    const uint64_t threshold_ns = static_cast<uint64_t>(load<int32_t>(owner, 0x104)) *
                                  quality_nominal_period_ns;
    Validity validity {};
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& value = state.eyes[eye];
        auto& window = value.time;
        window.source_ns = source_ns[eye];
        window.advanced = false;
        window.reference_pending = false;
        window.source_dt_ns = 0;
        window.observation_evidence_ns = 0;
        window.paused_ns = 0;
        if (window.has_source && source_ns[eye] <= window.last_source_ns) {
            window.update = source_ns[eye] == window.last_source_ns
                ? QualityTimeUpdate::Repeated : QualityTimeUpdate::Backward;
        } else {
            window.window_limit_ns = window_ns;
            window.threshold_ns = threshold_ns;
            if (!window.has_source) {
                window.has_source = true;
                window.last_source_ns = source_ns[eye];
            } else {
                // Normal adjacent real observations support their full interval;
                // above 2*T0 only the recovery observation contributes T0. Missing
                // source time is paused, so it cannot age out previous evidence.
                advance_time_quality(window, source_ns[eye], raw24[20 + 2 * eye] > 0.4f);
            }
            value.classification = window.good_ns > threshold_ns ? 1
                : window.bad_ns > threshold_ns ? 0 : -1;
            ++value.successful_updates;
            window.advanced = true;
            window.reference_pending = true;
            window.update = QualityTimeUpdate::Updated;
        }
        validity.eye[eye] = value.classification != 0;
    }
    validity.combined = (validity.eye[0] || validity.eye[1]) ? 1.0f : 0.0f;
    return validity;
}

const void* select_reference(State& state, void* owner, unsigned eye,
                             const void* current) {
    state.frame_current[eye] = current;
    return state.eyes[eye].reference_present
        ? static_cast<unsigned char*>(owner) + reference_offsets[eye]
        : current;
}

uint32_t finish_references(State& state, uintptr_t base, void* owner,
                           const void* eye0, const void* eye1) {
    const void* current[2] = {eye0, eye1};
    uint32_t captured = 0;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& value = state.eyes[eye];
        if (value.classification == 0) {
            value.reference_present = false;
        } else if (value.classification == 1 && !value.reference_present) {
            copy_reference(base, static_cast<unsigned char*>(owner) + reference_offsets[eye],
                           current[eye]);
            using ClearQueue = void (*)(void*);
            reinterpret_cast<ClearQueue>(base + 0x5e7a0)(
                static_cast<unsigned char*>(owner) + queue_offsets[eye]);
            store<int32_t>(owner, count_offsets[eye], 0);
            value.reference_present = true;
            ++value.reference_generation;
            value.reference_update = value.successful_updates;
            captured |= uint32_t(1) << eye;
        }
    }
    // Force stock's fallback path; select_reference supplies the independent
    // second channel. A single stock byte cannot encode both reference states.
    store<uint8_t>(owner, 1, 0);
    return captured;
}

uint32_t finish_references(State& state, uintptr_t base, void* owner) {
    return finish_references(state, base, owner, state.frame_current[0], state.frame_current[1]);
}

uint32_t finish_references_time(State& state, uintptr_t base, void* owner,
                               const void* eye0, const void* eye1) {
    const void* current[2] = {eye0, eye1};
    uint32_t captured = 0;
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& value = state.eyes[eye];
        auto& window = value.time;
        if (!window.advanced || !window.reference_pending) continue;
        if (value.classification == 0) {
            value.reference_present = false;
        } else if (value.classification == 1 && !value.reference_present) {
            copy_reference(base, static_cast<unsigned char*>(owner) + reference_offsets[eye],
                           current[eye]);
            value.reference_good_ns = window.good_ns;
            value.reference_window_span_ns = window.window_span_ns;
            value.reference_effective_clock_ns = window.effective_clock_ns;
            clear_time_quality(window);
            value.reference_present = true;
            ++value.reference_generation;
            value.reference_update = value.successful_updates;
            value.reference_source_ns = window.last_source_ns;
            captured |= uint32_t(1) << eye;
        }
        window.reference_pending = false;
    }
    // Same stock fallback contract as the legacy reference profile.
    store<uint8_t>(owner, 1, 0);
    return captured;
}

bool preserve_mouth_quality_reset(State& state, uintptr_t base, void* owner,
                                  const float raw24[24]) {
    auto& legacy = state.mouth_compatibility;
    const uint64_t window = static_cast<uint64_t>(int64_t(load<int32_t>(owner, 0x100)));
    const int32_t threshold = load<int32_t>(owner, 0x104);
    int32_t classification[2];
    for (unsigned eye = 0; eye < 2; ++eye) {
        classification[eye] = advance_legacy_quality(legacy.eye_samples[eye], legacy.eye_good[eye],
                                                     window, threshold, raw24[20 + 2 * eye]);
    }
    // Stock 0x62b70..0x62b8c: both 0 clears the legacy shared flag; otherwise
    // only an explicit 1 with no existing reference triggers queue resets.
    if (classification[0] == 0 && classification[1] == 0) {
        legacy.shared_reference_present = false;
        return false;
    }
    if ((classification[0] == 1 || classification[1] == 1) && !legacy.shared_reference_present) {
        // 0x62bcc zeroes the mouth count, then 0x62be0 clears its stock deque.
        store<int32_t>(owner, 0x118, 0);
        using ClearQueue = void (*)(void*);
        reinterpret_cast<ClearQueue>(base + 0x5e7a0)(static_cast<unsigned char*>(owner) + 0x180);
        for (unsigned eye = 0; eye < 2; ++eye) {
            legacy.eye_samples[eye].clear();
            legacy.eye_good[eye] = 0;
        }
        legacy.shared_reference_present = true;
        return true;
    }
    return false;
}

} // namespace expression_independent
