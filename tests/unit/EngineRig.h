#pragma once

// Shared helpers for engine-level tests: an Engine on its own ConfigManager, rendered in 512-sample blocks.

#include "engine/Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace winerose::test {

inline MidiEvent midi(int offset, std::uint8_t status, std::uint8_t d1, std::uint8_t d2)
{
    MidiEvent e;
    e.sampleOffset = offset;
    e.data[0] = status;
    e.data[1] = d1;
    e.data[2] = d2;
    e.size = 3;
    return e;
}

struct Rig {
    std::shared_ptr<ConfigManager> cm = std::make_shared<ConfigManager>();
    Engine engine { cm };
    std::vector<float> l = std::vector<float>(512), r = std::vector<float>(512);

    Rig() { engine.prepare(48000.0, 512); }

    ParamRegistry& reg(const std::string& name) { return *cm->findParamRegistry(name); }

    /** Renders one 512-sample block; returns the peak level. */
    float block(std::vector<MidiEvent> events = {})
    {
        float* chans[] = {l.data(), r.data()};
        engine.process(chans, 2, 512, events.data(), static_cast<int>(events.size()), TransportInfo{});
        float peak = 0.0f;
        for (std::size_t i = 0; i < 512; ++i) peak = std::max({peak, std::abs(l[i]), std::abs(r[i])});
        return peak;
    }

    /** Holds `note` from the first block for `seconds`; returns the left channel. */
    std::vector<float> renderNote(int note, double seconds)
    {
        std::vector<float> out;
        const int blocks = static_cast<int>(seconds * 48000.0 / 512.0);
        for (int b = 0; b < blocks; ++b) {
            if (b == 0) block({midi(0, 0x90, static_cast<std::uint8_t>(note), 100)});
            else        block();
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }
};

} // namespace winerose::test
