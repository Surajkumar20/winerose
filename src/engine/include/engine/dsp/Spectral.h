#pragma once

#include "engine/dsp/Fft.h"
#include "engine/dsp/SampleData.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace winerose::dsp {

/**
 * @brief Spectral oscillator (SPEC §1.4): analysis/resynthesis with independent time and pitch.
 *
 * Analysis (off the audio thread, once per sample): STFT, N = 2048, hop 512 (4x overlap, Hann). Per frame
 * and bin it keeps the log spectral envelope (true-envelope cepstral smoothing), the fine structure (log
 * magnitude minus envelope), the instantaneous frequency, the phase, and a spectral-flux transient flag.
 *
 * Resynthesis (per voice): the fine structure is remapped by the pitch ratio while the envelope stays put (or
 * moves by the timbre shift), so formants survive pitch changes. Peaks advance their phase by their own
 * instantaneous frequency; bins around a peak keep their analysed phase offset to it (identity phase locking).
 * Phases reset to the analysis at transients.
 */
class SpectralData {
public:
    static constexpr int kFft = 2048;
    static constexpr int kHop = 512;
    static constexpr int kBins = kFft / 2 + 1;
    static constexpr double kMaxSeconds = 15.0;

    static std::shared_ptr<const SpectralData> analyze(const SampleData& sample);

    std::uint64_t id() const noexcept { return m_id; }
    int    frameCount() const noexcept { return m_frames; }
    double sampleRate() const noexcept { return m_sampleRate; }
    int    rootKey() const noexcept { return m_rootKey; }

    const float* envelope(int frame) const noexcept { return &m_env[static_cast<std::size_t>(frame) * kBins]; }
    const float* fine(int frame) const noexcept { return &m_fine[static_cast<std::size_t>(frame) * kBins]; }
    const float* frequency(int frame) const noexcept { return &m_freq[static_cast<std::size_t>(frame) * kBins]; }   // in bins
    const float* phase(int frame) const noexcept { return &m_phase[static_cast<std::size_t>(frame) * kBins]; }
    bool         transient(int frame) const noexcept { return m_transient[static_cast<std::size_t>(frame)] != 0; }

private:
    std::uint64_t m_id = nextAssetId();
    int    m_frames = 0;
    double m_sampleRate = 48000.0;
    int    m_rootKey = 60;
    std::vector<float> m_env, m_fine, m_freq, m_phase;
    std::vector<std::uint8_t> m_transient;
};

namespace spectral {
void init() noexcept;   // window-transform table; call once off the audio thread

struct Params {
    float  position = 0.0f;    // 0..1 start position in the sample
    float  scan = 1.0f;        // playback speed of the analysis frames (-2..2; 0 = freeze)
    float  timbreSemis = 0.0f; // envelope shift (formants up/down), -24..24
    float  formant = 0.0f;     // 0: formants follow the pitch (plain shift); 1: formants preserved
    float  lowCutHz = 0.0f;    // spectral filter
    float  highCutHz = 24000.0f;
    bool   transients = true;  // reset phases at detected transients
    double pitch = 1.0;        // frequency ratio relative to the sample's root
    double sampleRate = 48000.0;
};

/** Per-voice resynthesis state (≈ 20 KB). render() is realtime-safe; it shares one RealFft across voices. */
class Voice {
public:
    void start(const SpectralData& data, const Params& p) noexcept;
    void render(const SpectralData& data, const Params& p, RealFft& fft, float* outL, float* outR, int n) noexcept;

private:
    void synthesize(const SpectralData& data, const Params& p, RealFft& fft) noexcept;

    std::array<float, SpectralData::kBins> m_centre {};    // partial phase at the window centre, by output bin
    std::array<float, SpectralData::kBins> m_mag {};
    std::array<float, SpectralData::kFft>  m_acc {};       // overlap-add accumulator
    std::array<float, SpectralData::kFft + 2> m_work {};   // re/im of the output spectrum (2·kBins), then time
    std::array<float, SpectralData::kFft>  m_time {};
    static constexpr int kMaxPeaks = 400;
    std::array<std::int16_t, kMaxPeaks> m_peaks {};   // source peak bins of the current frame
    std::array<float, SpectralData::kHop>  m_out {};       // finished samples of the current hop
    double m_frame = 0.0;                                   // analysis frame position
    int    m_lastFrame = -1;
    int    m_outPos = SpectralData::kHop;                   // next sample of m_out to emit
    bool   m_first = true;
};
}

} // namespace winerose::dsp
