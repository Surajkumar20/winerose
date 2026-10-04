#include "engine/fx/FxRack.h"

#include <cstring>

namespace winerose::fx {

using namespace dsp;

void FxRack::setSlotType(int slot, FxType type, double sampleRate)
{
    m_published[static_cast<std::size_t>(slot)] = type;
    m_effects[static_cast<std::size_t>(slot)].publish(createEffect(type, sampleRate));
}

void FxRack::collectGarbage()
{
    for (auto& e : m_effects) e.collectGarbage();
}

void FxRack::prepare(double sampleRate)
{
    m_sampleRate = sampleRate;
    beginBlock();   // adopt anything pending (the audio thread is stopped)
    for (auto* e : m_current)
        if (e != nullptr) e->prepare(sampleRate);
    reset();
}

void FxRack::beginBlock() noexcept
{
    for (int s = 0; s < kSlotsPerRack; ++s) m_current[static_cast<std::size_t>(s)] = m_effects[static_cast<std::size_t>(s)].acquire();
}

void FxRack::reset() noexcept
{
    for (auto* e : m_current)
        if (e != nullptr) e->reset();
    for (auto& sp : m_splitters) {
        for (auto* arr : {&sp.x1, &sp.x2, &sp.ap})
            for (auto& x : *arr) x.reset();
    }
}

void FxRack::setParams(const Params& params, const FxContext& ctx) noexcept
{
    m_params = params;
    for (int s = 0; s < kSlotsPerRack; ++s) {
        const auto& sp = params[static_cast<std::size_t>(s)];
        Effect* e = m_current[static_cast<std::size_t>(s)];
        // Only drive an effect that matches the slot's current type (a newly built one may still be in flight).
        if (e != nullptr && e->type() == sp.type) e->setParams(sp.p, ctx);
        if (isSplitter(sp.type)) {
            auto& split = m_splitters[static_cast<std::size_t>(s)];
            const double f1 = expo(sp.p[0], 20.0f, 20000.0f);
            const double f2 = sp.type == FxType::SplitLowMidHigh ? std::max(f1 * 1.05, static_cast<double>(expo(sp.p[1], 20.0f, 20000.0f))) : f1;
            for (int c = 0; c < 2; ++c) {
                split.x1[static_cast<std::size_t>(c)].setKeepState(f1, m_sampleRate);
                split.x2[static_cast<std::size_t>(c)].setKeepState(f2, m_sampleRate);
                split.ap[static_cast<std::size_t>(c)].setKeepState(f2, m_sampleRate);
            }
        }
    }
}

bool FxRack::active() const noexcept
{
    for (int s = 0; s < kSlotsPerRack; ++s) {
        const auto& sp = m_params[static_cast<std::size_t>(s)];
        if (sp.enabled && sp.type != FxType::None) return true;
    }
    return false;
}

void FxRack::runSlot(int slot, float* l, float* r, int n) noexcept
{
    const auto& sp = m_params[static_cast<std::size_t>(slot)];
    Effect* e = m_current[static_cast<std::size_t>(slot)];
    if (!sp.enabled || e == nullptr || e->type() != sp.type || sp.mix <= 0.0f) return;
    if (sp.mix >= 1.0f) {
        e->process(l, r, n);
        return;
    }
    float dryL[32], dryR[32];
    std::memcpy(dryL, l, sizeof(float) * static_cast<std::size_t>(n));
    std::memcpy(dryR, r, sizeof(float) * static_cast<std::size_t>(n));
    e->process(l, r, n);
    for (int i = 0; i < n; ++i) {
        l[i] = dryL[i] + (l[i] - dryL[i]) * sp.mix;
        r[i] = dryR[i] + (r[i] - dryR[i]) * sp.mix;
    }
}

void FxRack::processSplitter(int slot, float* l, float* r, int n) noexcept
{
    const auto& sp = m_params[static_cast<std::size_t>(slot)];
    auto& split = m_splitters[static_cast<std::size_t>(slot)];
    const int bands = splitterBands(sp.type);
    float band[3][2][32];

    for (int i = 0; i < n; ++i) {
        const float x[2] = {l[i], r[i]};
        if (sp.type == FxType::SplitMidSide) {
            const float mid = 0.5f * (x[0] + x[1]), side = 0.5f * (x[0] - x[1]);
            band[0][0][i] = band[0][1][i] = mid;
            band[1][0][i] = band[1][1][i] = side;
            continue;
        }
        for (int c = 0; c < 2; ++c) {
            float low, high;
            split.x1[static_cast<std::size_t>(c)].split(x[c], low, high);
            if (bands == 2) {
                band[0][c][i] = low;
                band[1][c][i] = high;
            } else {
                float mid, top;
                split.x2[static_cast<std::size_t>(c)].split(high, mid, top);
                band[0][c][i] = split.ap[static_cast<std::size_t>(c)].allpass(low);   // align phase with mid + top
                band[1][c][i] = mid;
                band[2][c][i] = top;
            }
        }
    }

    // Each band through the slot after the splitter (if that slot holds an ordinary effect).
    for (int k = 0; k < bands; ++k) {
        const int target = slot + 1 + k;
        if (target < kSlotsPerRack && !isSplitter(m_params[static_cast<std::size_t>(target)].type))
            runSlot(target, band[k][0], band[k][1], n);
    }

    // Band levels, then recombine (sum, or M/S decode).
    const int levelBase = sp.type == FxType::SplitLowMidHigh ? 2 : (sp.type == FxType::SplitLowHigh ? 1 : 0);
    float gain[3];
    for (int k = 0; k < bands; ++k) gain[k] = dbToGain(bipolarDb(sp.p[static_cast<std::size_t>(levelBase + k)], 24.0f));
    for (int i = 0; i < n; ++i) {
        float outL, outR;
        if (sp.type == FxType::SplitMidSide) {
            outL = band[0][0][i] * gain[0] + band[1][0][i] * gain[1];
            outR = band[0][1][i] * gain[0] - band[1][1][i] * gain[1];
        } else {
            outL = outR = 0.0f;
            for (int k = 0; k < bands; ++k) {
                outL += band[k][0][i] * gain[k];
                outR += band[k][1][i] * gain[k];
            }
        }
        l[i] = l[i] + (outL - l[i]) * sp.mix;
        r[i] = r[i] + (outR - r[i]) * sp.mix;
    }
}

void FxRack::process(float* left, float* right, int numSamples) noexcept
{
    for (int s = 0; s < kSlotsPerRack; ++s) {
        const auto& sp = m_params[static_cast<std::size_t>(s)];
        if (!sp.enabled || sp.type == FxType::None) continue;
        if (isSplitter(sp.type)) {
            processSplitter(s, left, right, numSamples);
            s += splitterBands(sp.type);   // its band slots have been used
            continue;
        }
        runSlot(s, left, right, numSamples);
    }
}

} // namespace winerose::fx
