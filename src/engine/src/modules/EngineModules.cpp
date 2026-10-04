#include "engine/modules/EngineModules.h"

namespace winerose::modules {

EngineModules::EngineModules(std::shared_ptr<ConfigManager> config)
{
    for (int o = 0; o < voice::kOscCount; ++o)
        osc[static_cast<std::size_t>(o)] = std::make_unique<OscillatorModule>(config, o, targets);
    noise   = std::make_unique<NoiseModule>(config, targets);
    sub     = std::make_unique<SubOscModule>(config, targets);
    for (int f = 0; f < FilterModule::kCount; ++f)
        filter[static_cast<std::size_t>(f)] = std::make_unique<FilterModule>(config, f, targets);
    routing = std::make_unique<RoutingModule>(config, targets);
    for (int e = 0; e < modulation::kEnvCount; ++e)
        env[static_cast<std::size_t>(e)] = std::make_unique<EnvelopeModule>(config, e, targets);
    for (int l = 0; l < modulation::kLfoCount; ++l)
        lfo[static_cast<std::size_t>(l)] = std::make_unique<LfoModule>(config, l, targets);
    for (int m = 0; m < modulation::kMacroCount; ++m) {
        macro[static_cast<std::size_t>(m)] = std::make_unique<MacroModule>(config, m, targets);
        macroTarget[static_cast<std::size_t>(m)] = macro[static_cast<std::size_t>(m)]->valueIndex();
    }
    matrix = std::make_unique<MatrixModule>(config);

    // Audio-rate destinations: oscillator pitch (coarse, fine), level, wavetable position, filter cutoff.
    fastDest.assign(static_cast<std::size_t>(targets.count()), FastDest{});
    auto span = [&](int index) {
        const auto& m = std::get<NumericMeta>(targets.at(index).meta);
        return static_cast<float>(m.max_val - m.min_val);
    };
    for (int o = 0; o < voice::kOscCount; ++o) {
        const auto& i = osc[static_cast<std::size_t>(o)]->indices();
        const auto oi = static_cast<std::uint8_t>(o);
        fastDest[static_cast<std::size_t>(i.coarse)] = {FastDest::Kind::OscPitch, oi, span(i.coarse)};
        fastDest[static_cast<std::size_t>(i.fine)]   = {FastDest::Kind::OscPitch, oi, span(i.fine) / 100.0f};   // cents → st
        fastDest[static_cast<std::size_t>(i.level)]  = {FastDest::Kind::OscLevel, oi, span(i.level)};
        fastDest[static_cast<std::size_t>(i.wtPos)]  = {FastDest::Kind::OscWtPos, oi, span(i.wtPos)};
    }
    for (int f = 0; f < FilterModule::kCount; ++f)
        fastDest[static_cast<std::size_t>(filter[static_cast<std::size_t>(f)]->cutoffIndex())] =
            {FastDest::Kind::FilterCutoff, static_cast<std::uint8_t>(f), 0.0f};
}

void EngineModules::build(const float* plain, double sampleRate, voice::VoiceSettings& out) const noexcept
{
    auto& c = out.control;
    for (int o = 0; o < voice::kOscCount; ++o)
        c.osc[static_cast<std::size_t>(o)] = osc[static_cast<std::size_t>(o)]->read(plain);
    c.noise  = noise->read(plain);
    c.sub    = sub->read(plain);
    for (int f = 0; f < FilterModule::kCount; ++f)
        c.filter[static_cast<std::size_t>(f)] = filter[static_cast<std::size_t>(f)]->read(plain);
    c.filterRouting = routing->read(plain);
    for (int e = 0; e < modulation::kEnvCount; ++e)
        out.env[static_cast<std::size_t>(e)] = env[static_cast<std::size_t>(e)]->read(plain);
    c.env = out.env[0];
    c.sampleRate = sampleRate;
    for (int l = 0; l < modulation::kLfoCount; ++l)
        out.lfo[static_cast<std::size_t>(l)] = lfo[static_cast<std::size_t>(l)]->read(plain);
    for (int m = 0; m < modulation::kMacroCount; ++m)
        out.macro[static_cast<std::size_t>(m)] = macro[static_cast<std::size_t>(m)]->read(plain);
}

} // namespace winerose::modules
