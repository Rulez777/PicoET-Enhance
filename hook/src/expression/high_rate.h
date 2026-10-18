#pragma once
#include <cstddef>
#include <cstdint>

namespace expression_high_rate {
struct Mat { alignas(8) unsigned char bytes[0x60]; };
struct Vector { void *begin, *end, *capacity; };
struct Output { float values[54]; };
struct Array { uint32_t flags, padding; const Mat* object; uint64_t size; };
using Trackor = Output (*)(void*, const Mat*, const Mat*, const Mat*);
using Infer = Vector (*)(void*, void*, const Vector*);
using Resize = void (*)(const Array*, const Array*, uint64_t, double, double, int);
struct FrameInfo {
    bool slow = true, mouth_fresh = false, completed = false, eye_only_completed = false;
    bool eye_completed = false, owner_blocked = false, skipped = false;
    uint64_t held_mouth_source = 0;
};
struct RateConfig {
    uint32_t requested_eye_hz = 90, requested_mouth_hz = 90;
    uint32_t eye_hz = 90, mouth_hz = 90;
};
// Startup-only, before dispatch/owner creation; shared by the payload and replay.
// Zero selects the corresponding default; cap at 90, then clamp mouth to eye.
RateConfig configure_rates(uint32_t eye_hz, uint32_t mouth_hz);
void configure(uintptr_t base, Trackor stock, Infer observed_infer, Infer stock_infer, Resize resize);
Output dispatch(void* owner, const Mat* eye0, const Mat* eye1, const Mat* mouth,
                const uint64_t sources[3]);
const FrameInfo& frame_info();
void forget_owner(void* owner);
void observe_stock_completion(bool completed);
void observe_infer(void* engine, const float* values, size_t count);
void submit_before_eye(void* engine);
bool take_mouth(void* engine, Vector* result);
}
