#pragma once
#include <cstddef>
#include <cstdint>

namespace expression_independent {
using Merge = void (*)(const void*, const void*, void*);
struct FrameTiming {
    uint64_t source_ns[3] {};
    bool slow = true;
    bool attempted = true;
};
// Hook ownership and installation stay in picoet_hook.c.
void configure(uintptr_t base, Merge merge);
void reset_owner(void* owner);
void forget_owner(void* owner);
void begin_frame(void* owner, const FrameTiming& timing);
void merge_stock(const void* current, const void* reference, void* result, uintptr_t caller);
void merge_eye(unsigned eye, const void* current, void* result);
void export_prefix(void* prefix);
void observe_eye_only_completion(bool completed);
void observe_infer(void* engine, const float* values, size_t count);
void observe_completion(bool completed);
void end_frame(float* expression54);
void fail_frame(float* expression54, uint32_t attempted_mask);
bool frame_applied();
}
