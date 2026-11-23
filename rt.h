#pragma once
// Real-time helpers.
//
// Adapted from Soundboard Lab native/SoundboardAudio.cpp (Loukious/soundboard-lab,
// commit b3a78bc): the float<->bit-pattern helpers (Bits/Float), the interlocked
// Load/Store accessors used to exchange control values with the real-time
// callback, and the per-sample output guard from Capture::APOProcess
// ("if(!std::isfinite(value))value=0; value=std::clamp(value,-1.f,1.f);").
// The originals exchanged values through shared memory with an APO; here they
// exchange values between the UI thread and the engine's audio thread.
#include <atomic>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace mp {

inline int32_t Bits(float v) { int32_t x; memcpy(&x, &v, 4); return x; }
inline float Float(int32_t v) { float x; memcpy(&x, &v, 4); return x; }

// Lock-free float shared between the UI thread (writer) and audio thread (reader).
class AtomicFloat {
    std::atomic<int32_t> bits;
public:
    explicit AtomicFloat(float v = 0) : bits(Bits(v)) {}
    float Load() const { return Float(bits.load(std::memory_order_relaxed)); }
    void Store(float v) { bits.store(Bits(v), std::memory_order_relaxed); }
    // Peak meter: audio thread raises, UI thread reads and resets.
    void Max(float v) {
        int32_t cur = bits.load(std::memory_order_relaxed);
        while (Float(cur) < v && !bits.compare_exchange_weak(cur, Bits(v), std::memory_order_relaxed)) {}
    }
    float Exchange(float v) { return Float(bits.exchange(Bits(v), std::memory_order_relaxed)); }
};

// Output guard from Soundboard Lab's APOProcess: never forward NaN/Inf and
// never exceed full scale.
inline float GuardSample(float value) {
    if (!std::isfinite(value)) value = 0;
    return std::clamp(value, -1.f, 1.f);
}

inline float DbToGain(float db) { return std::pow(10.f, db / 20.f); }
inline float GainToDb(float g) { return g > 1e-9f ? 20.f * std::log10(g) : -180.f; }

inline float PeakOf(const float* x, int n) {
    float p = 0;
    for (int i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
    return std::isfinite(p) ? p : 0.f;
}

} // namespace mp
