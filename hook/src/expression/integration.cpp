// Runtime subset of the verified source-dt / effective-observation-time candidate.
#include "integration.h"
#include "numerical.h"
#include "state.h"
#include "high_rate.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace expression_independent {
namespace {
uintptr_t plugin;
Merge original_merge;
struct OwnerState {
    State reference;
    SourceDtState dt_filter;
    NativeBrowFilter left, right;
    uint64_t mouth_source = 0;
    bool have_mouth_source = false;
    uint64_t published_sources[3] {};
    uint32_t published_mask = 0;
    explicit OwnerState(uintptr_t base) : left(base), right(base) {}
};
// Hooks may run before this translation unit's dynamic initializers.
std::mutex& registry_lock() { static std::mutex value; return value; }
auto& registry() {
    static std::unordered_map<void*, std::unique_ptr<OwnerState>> value;
    return value;
}
struct Frame {
    void* owner = nullptr;
    OwnerState* state = nullptr;
    unsigned raw_calls = 0, merge_calls[2] {};
    size_t raw_length = 0;
    bool completion_seen = false, completed = false, applied = false, pending_export = false;
    int32_t export_state[2] {};
    float raw[24] {}, post[20] {}, filtered[20] {}, final[52] {};
    float right_inner = 0;
    FrameTiming timing;
    bool eye_only_completed = false;
    uint32_t updated_mask = 0;
    uint64_t export_sources[3] {};
};
thread_local Frame frame;
struct ReferenceFrameGuard {
    State& state;
    ~ReferenceFrameGuard() {
        state.frame_current[0] = nullptr;
        state.frame_current[1] = nullptr;
    }
};
template<class T> T load(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
    return value;
}
template<class T> void store(void* object, size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(object) + offset, &value, sizeof(value));
}
void mark_observation(unsigned group, uint64_t source) {
    const uint32_t bit = 1u << group;
    if (source && (!(frame.state->published_mask & bit) || source > frame.state->published_sources[group])) {
        frame.state->published_mask |= bit;
        frame.state->published_sources[group] = source;
        frame.updated_mask |= bit;
    }
    frame.export_sources[group] = frame.state->published_sources[group];
}
}

void configure(uintptr_t base, Merge merge) {
    plugin = base;
    original_merge = merge;
}
void reset_owner(void* owner) { forget_owner(owner); }
void forget_owner(void* owner) {
    expression_high_rate::forget_owner(owner);
    if (frame.owner == owner) frame = Frame {};
    std::lock_guard<std::mutex> lock(registry_lock());
    registry().erase(owner);
}
void begin_frame(void* owner, const FrameTiming& timing) {
    frame = Frame {};
    std::lock_guard<std::mutex> lock(registry_lock());
    auto& state = registry()[owner];
    if (!state) state = std::make_unique<OwnerState>(plugin);
    frame.owner = owner;
    frame.state = state.get();
    frame.timing = timing;
    std::memcpy(frame.export_sources, state->published_sources, sizeof(frame.export_sources));
    begin_reference_frame(state->reference);
}
void merge_stock(const void* current, const void* reference, void* result, uintptr_t caller) {
    if (frame.state) {
        int eye = (caller == 0x626dc || caller == 0x62cb0) ? 0 :
                  (caller == 0x626ec || caller == 0x62cc0) ? 1 : -1;
        if (eye >= 0) {
            ++frame.merge_calls[eye];
            reference = select_reference(frame.state->reference, frame.owner, unsigned(eye), current);
        }
    }
    original_merge(current, reference, result);
}
void merge_eye(unsigned eye, const void* current, void* result) {
    if (!frame.state || eye >= 2) throw std::runtime_error("eye-only merge has no frame context");
    ++frame.merge_calls[eye];
    const void* reference = select_reference(frame.state->reference, frame.owner, eye, current);
    original_merge(current, reference, result);
}
void export_prefix(void* prefix) {
    if (!frame.pending_export) return;
    if (std::memcmp(static_cast<unsigned char*>(prefix) + 8, frame.final, sizeof(frame.final))) return;
    if (load<uint64_t>(prefix, 0) != frame.timing.source_ns[0]) return;
    store<float>(prefix, 216, frame.right_inner);
    store<int32_t>(prefix, 220, frame.export_state[0]);
    store<int32_t>(prefix, 224, frame.export_state[1]);
    store<uint32_t>(prefix, 228, 0x31495045); // EPI1
    store<uint32_t>(prefix, 232, 2);
    store<uint32_t>(prefix, 236, frame.updated_mask);
    store<uint64_t>(prefix, 240, frame.export_sources[2]);
    store<uint64_t>(prefix, 248, frame.export_sources[1]);
    frame.pending_export = false;
}
void observe_eye_only_completion(bool completed) { frame.eye_only_completed = completed; }
void observe_infer(void* engine, const float* values, size_t count) {
    if (!frame.state || engine != load<void*>(frame.owner, 0x880)) return;
    ++frame.raw_calls;
    frame.raw_length = count;
    if (count == 24 && values) std::memcpy(frame.raw, values, sizeof(frame.raw));
    else if (!values) frame.raw_length = 0;
}
void observe_completion(bool completed) {
    if (!frame.state) return;
    frame.completion_seen = true;
    frame.completed = completed;
}
void fail_frame(float* output, uint32_t attempted_mask) {
    if (!frame.state) return;
    const float neutral_eye[20] {};
    frame.right_inner = writeback_expression52(neutral_eye, output);
    for (unsigned i = 0; i < 54; ++i) if (!std::isfinite(output[i])) output[i] = 0.0f;
    output[52] = 0.0f;
    if (attempted_mask & 4) output[53] = 0.0f;
    frame.applied = false;
    frame.updated_mask = 0;
    frame.export_state[0] = frame.export_state[1] = 0;
    for (unsigned group = 0; group < 3; ++group)
        if (attempted_mask & (1u << group)) mark_observation(group, frame.timing.source_ns[group]);
    std::memcpy(frame.final, output, sizeof(frame.final));
    frame.pending_export = true;
}
void end_frame(float* output) {
    if (!frame.state) return;
    ReferenceFrameGuard reference_guard {frame.state->reference};
    const bool full_completed = frame.completion_seen && frame.completed;
    // A later mouth/stock-tail failure may still export this completed eye once;
    // the dispatcher seals the partially advanced owner before the next frame.
    bool completed = frame.eye_only_completed;
    for (float value : frame.raw) completed = completed && std::isfinite(value);
    if (!completed || frame.raw_calls != 1 || frame.raw_length != 24 ||
        frame.merge_calls[0] != 1 || frame.merge_calls[1] != 1) {
        fail_frame(output, frame.timing.attempted ? (frame.timing.slow ? 7u : 3u) : 0u);
        return;
    }
    postprocess_eye20(frame.raw, frame.post);
    void* contexts[2] = {&frame.state->left, &frame.state->right};
    filter_eye20_dt(frame.state->dt_filter, frame.raw, frame.post, frame.filtered,
                    frame.timing.source_ns, NativeBrowFilter::predict_dt, contexts);
    frame.right_inner = writeback_expression52(frame.filtered, output);
    const Validity validity = observe_quality_time(frame.state->reference, frame.owner,
                                                   frame.raw, frame.timing.source_ns);
    output[52] = validity.combined;
    (void)finish_references_time(frame.state->reference, plugin, frame.owner,
        frame.state->reference.frame_current[0], frame.state->reference.frame_current[1]);
    if (frame.timing.slow && full_completed) {
        (void)preserve_mouth_quality_reset(frame.state->reference, plugin, frame.owner, frame.raw);
    } else if (frame.timing.slow) output[53] = 0.0f;
    std::memcpy(frame.final, output, sizeof(frame.final));
    for (unsigned i = 0; i < 2; ++i)
        frame.export_state[i] = frame.state->reference.eyes[i].classification;
    frame.applied = true;
    frame.pending_export = true;
    for (unsigned eye = 0; eye < 2; ++eye)
        if (frame.state->dt_filter.eye[eye].diagnostics.update == SourceUpdate::Updated)
            mark_observation(eye, frame.timing.source_ns[eye]);
    if (frame.timing.slow && full_completed &&
        (!frame.state->have_mouth_source || frame.timing.source_ns[2] > frame.state->mouth_source)) {
        mark_observation(2, frame.timing.source_ns[2]);
        frame.state->mouth_source = frame.timing.source_ns[2];
        frame.state->have_mouth_source = true;
    } else if (frame.timing.slow && !full_completed) {
        // A failed new mouth attempt is an invalid update, not an unchanged mouth.
        mark_observation(2, frame.timing.source_ns[2]);
    }
}
bool frame_applied() { return frame.applied; }
}
