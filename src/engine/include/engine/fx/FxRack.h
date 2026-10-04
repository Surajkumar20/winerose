#pragma once

#include "engine/fx/Effect.h"
#include "engine/fx/FxDsp.h"

#include "params/ObjectExchange.h"

#include <array>
#include <memory>

namespace winerose::fx {

/** Per-tick parameters of one rack slot (read from its registry). */
struct SlotParams {
    FxType type = FxType::None;
    bool   enabled = false;
    float  mix = 1.0f;
    std::array<float, kParamCount> p {};
};

/**
 * @class FxRack
 * @brief One FX chain (Main, Bus 1 or Bus 2): kSlotsPerRack slots processed in order (SPEC §1.8).
 *
 * Effect objects are created on the message thread (setSlotType) and adopted by the audio thread at block
 * start (beginBlock) through an ObjectExchange per slot — the audio thread never allocates. Splitters are
 * handled here: a splitter's bands each run through the slot(s) right after it, then recombine.
 */
class FxRack {
public:
    using Params = std::array<SlotParams, kSlotsPerRack>;

    // --- Message thread ---
    /** Build (or remove) the effect for a slot. sampleRate is the rate it is prepared for. */
    void setSlotType(int slot, FxType type, double sampleRate);
    FxType publishedType(int slot) const noexcept { return m_published[static_cast<std::size_t>(slot)]; }
    void collectGarbage();

    // --- Audio thread stopped (prepare) ---
    void prepare(double sampleRate);

    // --- Audio thread ---
    void beginBlock() noexcept;                                     // adopt newly published effects
    void setParams(const Params& params, const FxContext& ctx) noexcept;   // control tick
    void process(float* left, float* right, int numSamples) noexcept;     // numSamples <= 32
    void reset() noexcept;

    /** True if any enabled slot holds something (lets the engine skip silent racks). */
    bool active() const noexcept;

private:
    struct Splitter {
        std::array<dsp::Lr4, 2> x1, x2, ap;   // [channel]: first crossover, second crossover, low-band allpass
    };

    void runSlot(int slot, float* l, float* r, int n) noexcept;
    void processSplitter(int slot, float* l, float* r, int n) noexcept;

    std::array<ObjectExchange<Effect>, kSlotsPerRack> m_effects;
    std::array<Effect*, kSlotsPerRack> m_current {};
    std::array<FxType, kSlotsPerRack> m_published {};   // message-thread view of what was built
    std::array<Splitter, kSlotsPerRack> m_splitters {};
    Params m_params {};
    double m_sampleRate = 48000.0;
};

} // namespace winerose::fx
