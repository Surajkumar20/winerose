#pragma once

#include "engine/dsp/SampleData.h"

#include <array>
#include <cstdint>

namespace winerose::dsp {

/**
 * @brief Granular oscillator (SPEC §1.4): an asynchronous scheduler over a fixed pool of kMaxGrains grains,
 *        each with its own read position, rate, window and pan. Cost per sample is bounded by the pool size,
 *        never by the settings ("granular CPU bounded", SPEC Phase 7). No allocation.
 */
namespace granular {

inline constexpr int kMaxGrains = 256;

enum class Window : int { Hann = 0, Tukey, Gaussian, Triangle, Rectangle, Count };
inline constexpr const char* kWindowNames[] = {"Hann", "Tukey", "Gaussian", "Triangle", "Rectangle"};

struct Params {
    float  position = 0.0f;     // 0..1 of the sample
    float  scan = 0.0f;         // playhead speed relative to real time (-2..2; 0 = frozen)
    float  sizeMs = 80.0f;      // grain length
    float  density = 20.0f;     // grains per second
    float  posRandom = 0.0f;    // 0..1 position scatter (fraction of the sample)
    float  pitchRandom = 0.0f;  // 0..1 → ±12 semitones
    float  panRandom = 0.0f;    // 0..1
    Window window = Window::Hann;
    float  windowAmount = 0.5f; // Tukey taper / Gaussian width
    double pitch = 1.0;         // playback ratio (note + oscillator pitch), relative to the sample's root
    double sampleRate = 48000.0;
};

void init() noexcept;   // window tables; call once off the audio thread

struct Grain {
    double pos = 0.0;        // level-0 frames
    double inc = 0.0;        // level-0 frames per output sample
    float  phase = 0.0f;     // 0..1 through the window
    float  phaseInc = 0.0f;
    float  gainL = 0.0f, gainR = 0.0f;
    float  shape = 0.0f;     // window parameter
    int    level = 0;        // mip level (increment <= 1 there), fixed for the grain's life
    double scale = 1.0;      // 1 / 2^level
    Window window = Window::Hann;
};

class Engine {
public:
    void start(const Params& p, std::uint64_t seed) noexcept;
    void stop() noexcept { m_active = 0; }

    /** Adds n samples to outL/outR. */
    void render(const SampleData& s, const Params& p, float* outL, float* outR, int n) noexcept;

    int activeGrains() const noexcept { return m_active; }

private:
    void spawn(const SampleData& s, const Params& p) noexcept;
    float random() noexcept;   // [0,1)

    std::array<Grain, kMaxGrains> m_grains {};
    int           m_active = 0;
    double        m_scan = 0.0;        // playhead offset (fraction of the sample)
    double        m_untilNext = 0.0;   // samples until the next grain
    std::uint64_t m_rng = 1;
};

} // namespace granular
} // namespace winerose::dsp
