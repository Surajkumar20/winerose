#pragma once

#include <atomic>
#include <cstdint>

namespace winerose {

// POD types crossing the engine boundary. No JUCE, no std::string: any host (VST3/CLAP via JUCE,
// a standalone app, a Node addon, the offline renderer) can produce and consume these.

struct MidiEvent {
    std::int32_t sampleOffset = 0;   // relative to the start of the process() block
    std::uint8_t data[3] {};         // status, data1, data2 (SysEx is not carried)
    std::uint8_t size = 0;           // 1..3
};

struct TransportInfo {
    double bpm         = 120.0;
    double ppqPosition = 0.0;
    bool   isPlaying   = false;
};

class MidiEventSink {
public:
    virtual ~MidiEventSink() = default;
    virtual void push(const MidiEvent& event) noexcept = 0;
};

// Written by the audio thread, read by any UI through IController::meters().
struct MeterState {
    std::atomic<float> peakLeft  { 0.0f };
    std::atomic<float> peakRight { 0.0f };
};

} // namespace winerose
