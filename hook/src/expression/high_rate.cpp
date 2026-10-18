// Source-clock dual-rate dispatch and same-frame stock mouth worker, without profiling.
#include "high_rate.h"
#include "integration.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <unordered_map>

namespace expression_high_rate {
namespace {
static_assert(sizeof(Mat) == 0x60 && sizeof(Vector) == 24 && sizeof(Output) == 0xd8 &&
              sizeof(Array) == 24, "stock AAPCS64 POD/Mat/Array ABI");
constexpr uint64_t slow_period = 41666667;
RateConfig configured_rates;
uint64_t period_ns(uint32_t hz) {
    return hz == 24 ? slow_period : (1000000000ULL + hz - 1) / hz;
}
// Consume only the latest due slot: a source gap never creates catch-up work.
uint64_t consume_slot(uint64_t stamp, uint64_t period, uint64_t& next) {
    const uint64_t slot = next ? next + ((stamp - next) / period) * period : stamp;
    next = slot + period;
    return slot;
}
Trackor stock_trackor;
Infer observed_infer, stock_infer;
Resize resize_image;
using MatCall = void (*)(Mat*);
using MatCopy = void (*)(Mat*, const Mat*);
using Allocate = void* (*)(size_t);
using Release = void (*)(void*);
using DestroyVector = void (*)(Vector*);
MatCall mat_construct, mat_destroy;
MatCopy mat_copy;
Allocate allocate;
Release release;
DestroyVector destroy_mats;
template<class T> T load(const void* object, size_t offset) {
    T result;
    std::memcpy(&result, static_cast<const unsigned char*>(object) + offset, sizeof(result));
    return result;
}
struct MatOwner {
    Mat value;
    MatOwner() { mat_construct(&value); }
    explicit MatOwner(const Mat* source) { mat_copy(&value, source); }
    ~MatOwner() { mat_destroy(&value); }
    MatOwner(const MatOwner&) = delete;
    MatOwner& operator=(const MatOwner&) = delete;
};
struct MatVector {
    Vector value;
    explicit MatVector(unsigned count) {
        auto* data = static_cast<Mat*>(allocate(count * sizeof(Mat)));
        value = {data, data, data + count};
    }
    void append(const Mat* source) {
        auto* next = static_cast<Mat*>(value.end);
        mat_copy(next, source);
        value.end = next + 1;
    }
    ~MatVector() { destroy_mats(&value); }
    MatVector(const MatVector&) = delete;
    MatVector& operator=(const MatVector&) = delete;
};
struct FloatVector {
    Vector value;
    ~FloatVector() { release(value.begin); }
};
struct OwnerState {
    uint64_t next_eye = 0, next_slow = 0, mouth_source = 0;
    uint64_t eye_sources[2] {};
    uint64_t full_sources[3] {};
    bool cached = false, has_eye_sources = false, has_full_sources = false, blocked = false;
    Output held_output {};
};
std::mutex& owners_lock() { static std::mutex value; return value; }
auto& owners() {
    static std::unordered_map<void*, std::unique_ptr<OwnerState>> value;
    return value;
}
struct CurrentFrame {
    void* owner = nullptr;
    const Mat* mouth = nullptr;
    uint64_t sources[3] {}, job_id = 0;
    bool active = false, stock_completed = false, eye_valid = false, mouth_valid = false;
    FrameInfo info;
    // Keep fast resized headers and pixels through independent end_frame.
    std::unique_ptr<MatOwner> resized_eyes[2];
};
thread_local CurrentFrame current;
thread_local uint64_t next_job_id;
struct MouthJob {
    void* engine;
    uint64_t id;
    MatVector images {1};
    Vector result {};
    bool failed = false;
    MouthJob(void* e, uint64_t identity, const Mat* image) : engine(e), id(identity) {
        images.append(image); // Retain owned, already-resized pixels.
    }
    ~MouthJob() { release(result.begin); }
};
class MouthExecutor {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
    pthread_t thread {};
    bool started = false, stopping = false, done = false;
    MouthJob* job = nullptr;
    static void* entry(void* self) { static_cast<MouthExecutor*>(self)->loop(); return nullptr; }
    void loop() {
        pthread_mutex_lock(&mutex);
        for (;;) {
            while ((!job || done) && !stopping) pthread_cond_wait(&changed, &mutex);
            if (stopping) break;
            MouthJob* task = job;
            pthread_mutex_unlock(&mutex);
            // Native mouth engine only; no main-thread integration state.
            try { task->result = stock_infer(nullptr, task->engine, &task->images.value); }
            catch (...) { task->failed = true; }
            pthread_mutex_lock(&mutex);
            done = true;
            pthread_cond_broadcast(&changed);
        }
        pthread_mutex_unlock(&mutex);
    }
public:
    bool submit(void* engine, const Mat* image) {
        if (!started) {
            if (pthread_create(&thread, nullptr, entry, this)) return false;
            started = true;
        }
        auto* task = new MouthJob(engine, current.job_id, image);
        pthread_mutex_lock(&mutex);
        job = task;
        done = false;
        pthread_cond_signal(&changed);
        pthread_mutex_unlock(&mutex);
        return true;
    }
    bool pending() const { return job != nullptr; } // Owner thread alone changes the pointer.
    MouthJob* take() {
        pthread_mutex_lock(&mutex);
        while (job && !done) pthread_cond_wait(&changed, &mutex);
        MouthJob* result = job;
        job = nullptr;
        done = false;
        pthread_mutex_unlock(&mutex);
        return result;
    }
    ~MouthExecutor() {
        delete take();
        if (started) {
            pthread_mutex_lock(&mutex);
            stopping = true;
            pthread_cond_signal(&changed);
            pthread_mutex_unlock(&mutex);
            pthread_join(thread, nullptr);
        }
        pthread_cond_destroy(&changed);
        pthread_mutex_destroy(&mutex);
    }
};
MouthExecutor& mouth_worker() { static thread_local MouthExecutor executor; return executor; }
void drain_mouth() {
    if (mouth_worker().pending()) {
        std::unique_ptr<MouthJob> job(mouth_worker().take());
    }
}
struct FrameGuard {
    OwnerState& state;
    bool returned = false;
    explicit FrameGuard(OwnerState& value) : state(value) {}
    ~FrameGuard() {
        drain_mouth();
        if (!returned) {
            state.blocked = true;
            current.info.completed = false;
            current.info.owner_blocked = true;
        }
        current.active = false;
        current.mouth = nullptr;
        current.owner = nullptr;
        current.resized_eyes[0].reset();
        current.resized_eyes[1].reset();
    }
};
void eye_only(void* owner, const Mat* eye0, const Mat* eye1) {
    current.resized_eyes[0] = std::make_unique<MatOwner>(eye0);
    current.resized_eyes[1] = std::make_unique<MatOwner>(eye1);
    Mat* resized[2] = {&current.resized_eyes[0]->value, &current.resized_eyes[1]->value};
    MatOwner merged[2];
    const uint64_t size = load<uint64_t>(owner, 0x8d0);
    for (unsigned eye = 0; eye < 2; ++eye) {
        Array input {0x01010000, 0, resized[eye], 0};
        Array output {0x02010000, 0, resized[eye], 0};
        resize_image(&input, &output, size, 0., 0., 1);
    }
    for (unsigned eye = 0; eye < 2; ++eye)
        expression_independent::merge_eye(eye, resized[eye], &merged[eye].value);
    MatVector images(2);
    images.append(&merged[0].value);
    images.append(&merged[1].value);
    FloatVector result {observed_infer(owner, load<void*>(owner, 0x880), &images.value)};
    if (!current.eye_valid) throw std::runtime_error("eye_only_raw24_incomplete");
    expression_independent::observe_eye_only_completion(true);
}
bool finite_output(const Output& output) {
    for (float value : output.values) if (!std::isfinite(value)) return false;
    return true;
}
}

RateConfig configure_rates(uint32_t eye_hz, uint32_t mouth_hz) {
    configured_rates.requested_eye_hz = eye_hz;
    configured_rates.requested_mouth_hz = mouth_hz;
    eye_hz = eye_hz ? eye_hz : 90;
    mouth_hz = mouth_hz ? mouth_hz : 90;
    configured_rates.eye_hz = eye_hz > 90 ? 90 : eye_hz;
    configured_rates.mouth_hz = mouth_hz > configured_rates.eye_hz
        ? configured_rates.eye_hz : mouth_hz;
    return configured_rates;
}
void configure(uintptr_t base, Trackor stock, Infer observed, Infer native, Resize resize) {
    stock_trackor = stock;
    observed_infer = observed;
    stock_infer = native;
    resize_image = resize;
    mat_construct = reinterpret_cast<MatCall>(base + 0x18dd44);
    mat_copy = reinterpret_cast<MatCopy>(base + 0x18e610);
    mat_destroy = reinterpret_cast<MatCall>(base + 0x18ed0c);
    allocate = reinterpret_cast<Allocate>(base + 0x3426f8);
    release = reinterpret_cast<Release>(base + 0x34278c);
    destroy_mats = reinterpret_cast<DestroyVector>(base + 0x56860);
}
Output dispatch(void* owner, const Mat* eye0, const Mat* eye1, const Mat* mouth,
                const uint64_t sources[3]) {
    OwnerState* state;
    {
        std::lock_guard<std::mutex> lock(owners_lock());
        auto& entry = owners()[owner];
        if (!entry) entry = std::make_unique<OwnerState>();
        state = entry.get();
    }
    current = {};
    current.owner = owner;
    current.active = true;
    current.mouth = mouth;
    current.info.held_mouth_source = state->mouth_source;
    current.job_id = ++next_job_id;
    std::memcpy(current.sources, sources, sizeof(current.sources));
    FrameGuard guard(*state);
    const uint64_t phase_stamp = sources[0] > sources[1] ? sources[0] : sources[1];
    const bool eyes_fresh = !state->has_eye_sources ||
        (sources[0] > state->eye_sources[0] || sources[1] > state->eye_sources[1]);
    const bool eye_due = configured_rates.eye_hz == 90 || !state->next_eye ||
        phase_stamp >= state->next_eye;
    const bool attempted = !state->blocked && eyes_fresh && eye_due;
    current.info.skipped = !state->blocked && !attempted;
    // At 90, every fresh source is admitted, including slightly faster-than-90
    // cameras. Lower rates use fixed source-clock slots, not completion time.
    uint64_t eye_slot = phase_stamp;
    if (attempted && configured_rates.eye_hz != 90)
        eye_slot = consume_slot(phase_stamp, period_ns(configured_rates.eye_hz), state->next_eye);
    bool full_fresh = !state->has_full_sources;
    if (state->has_full_sources) full_fresh = sources[0] > state->full_sources[0] &&
        sources[1] > state->full_sources[1] && sources[2] > state->full_sources[2];
    // Mouth deadlines are evaluated on the admitted eye's logical slot. Because
    // mouth <= eye, consecutive eye slots cannot skip a mouth deadline. This
    // preserves the target ratio without adding eye work on a skipped source;
    // source gaps still coalesce elapsed slots instead of creating catch-up work.
    // With eye=90, eye_slot is the real source stamp, preserving the old 24Hz
    // phase exactly. Mouth=90 bypasses phase gating altogether.
    const bool slow_due = configured_rates.mouth_hz == 90 || !state->cached ||
        !state->next_slow || eye_slot >= state->next_slow;
    current.info.slow = attempted && full_fresh && slow_due;
    if (current.info.slow && configured_rates.mouth_hz != 90)
        consume_slot(eye_slot, period_ns(configured_rates.mouth_hz), state->next_slow);
    expression_independent::FrameTiming timing;
    std::memcpy(timing.source_ns, sources, sizeof(timing.source_ns));
    timing.slow = current.info.slow;
    timing.attempted = attempted;
    timing.skipped = current.info.skipped;
    if (!timing.slow) timing.source_ns[2] = state->mouth_source;
    expression_independent::begin_frame(owner, timing);
    Output output = state->held_output;
    if (timing.skipped) {
        output.values[52] = output.values[53] = 0.f;
        // integration's skipped path exports held52 and mask=0 without
        // advancing any filters, references, quality windows or source history.
        expression_independent::end_frame(output.values);
        current.info.completed = expression_independent::frame_applied();
        guard.returned = true;
        return output;
    }
    bool body_returned = false;
    try {
        if (state->blocked) {
            output = {};
            current.info.owner_blocked = true;
        } else if (timing.slow) {
            output = stock_trackor(owner, eye0, eye1, mouth);
            body_returned = true;
        } else {
            output.values[53] = 0.f; // Held mouth coefficients are not a new observation.
            eye_only(owner, eye0, eye1);
            current.info.eye_only_completed = true;
            body_returned = true;
        }
    } catch (...) {
        output = {};
        current.info.eye_only_completed = false;
    }
    drain_mouth(); // Consume orphaned work before end_frame or engine teardown.
    const bool full_completed = timing.slow && body_returned && current.stock_completed &&
        current.eye_valid && current.mouth_valid && finite_output(output);
    current.info.eye_completed = current.eye_valid && (timing.slow || body_returned);
    expression_independent::observe_eye_only_completion(current.info.eye_completed);
    if (timing.slow && !full_completed) {
        output = state->held_output;
        output.values[53] = 0.f;
        expression_independent::observe_completion(false);
    }
    try { expression_independent::end_frame(output.values); }
    catch (...) {
        expression_independent::fail_frame(output.values,
            timing.attempted ? (timing.slow ? 7u : 3u) : 0u);
    }
    current.info.completed = expression_independent::frame_applied() && current.eye_valid &&
        (timing.slow ? full_completed : body_returned && current.info.eye_only_completed) &&
        finite_output(output);
    if (current.info.completed) {
        state->held_output = output;
        for (unsigned eye = 0; eye < 2; ++eye)
            if (!state->has_eye_sources || sources[eye] > state->eye_sources[eye])
                state->eye_sources[eye] = sources[eye];
        state->has_eye_sources = true;
    }
    if (current.info.completed && timing.slow) {
        state->cached = true;
        state->mouth_source = sources[2];
        std::memcpy(state->full_sources, sources, sizeof(state->full_sources));
        state->has_full_sources = true;
        current.info.mouth_fresh = true;
    }
    current.info.held_mouth_source = state->mouth_source;
    if (!current.info.completed) {
        state->blocked = true; // Never retry partially advanced stock or sidecar history.
        current.info.owner_blocked = true;
    }
    guard.returned = true;
    return output;
}
const FrameInfo& frame_info() { return current.info; }
void forget_owner(void* owner) {
    // Normal lifecycle reaches this only after the original worker has drained/joined.
    if (current.owner == owner) {
        drain_mouth();
        current.active = false;
        current.mouth = nullptr;
        current.owner = nullptr;
    }
    std::lock_guard<std::mutex> lock(owners_lock());
    owners().erase(owner);
}
void observe_stock_completion(bool completed) {
    if (current.active && current.info.slow) current.stock_completed = completed;
}
void observe_infer(void* engine, const float* values, size_t count) {
    if (!current.active) return;
    bool finite = values != nullptr;
    for (size_t i = 0; finite && i < count; ++i) finite = std::isfinite(values[i]);
    if (engine == load<void*>(current.owner, 0x880)) current.eye_valid = count == 24 && finite;
    if (engine == load<void*>(current.owner, 0x890)) current.mouth_valid = count == 35 && finite;
}
void submit_before_eye(void* engine) {
    if (current.active && current.info.slow && engine == load<void*>(current.owner, 0x880) &&
        !mouth_worker().pending() && load<void*>(current.mouth, 0x38))
        mouth_worker().submit(load<void*>(current.owner, 0x890), current.mouth);
}
bool take_mouth(void* engine, Vector* result) {
    if (!current.active || !current.info.slow || engine != load<void*>(current.owner, 0x890)) return false;
    if (mouth_worker().pending()) {
        std::unique_ptr<MouthJob> job(mouth_worker().take());
        *result = {};
        if (!job->failed && job->engine == engine && job->id == current.job_id) {
            *result = job->result;
            job->result = {}; // Exactly one transfer; stock caller now owns the float storage.
        }
        return true;
    }
    return false;
}
}
