#pragma once

#include "engine/dsp/SampleData.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace winerose::dsp {

/**
 * @brief A multisampled instrument (SPEC §1.4 Multisample), already resolved from SFZ by the control layer:
 *        every region carries its decoded sample. Immutable; shared with the audio thread via the snapshot.
 *
 * Supported SFZ semantics: lokey/hikey/key, lovel/hivel, pitch_keycenter, pitch_keytrack, tune, transpose,
 * volume, pan, offset/end, loop_mode (no_loop, one_shot, loop_continuous, loop_sustain), loop_start/end,
 * loop_crossfade, amp_velcurve_N, seq_length/seq_position (round robin), trigger=attack|release.
 */
struct Multisample {
    enum class Trigger : int { Attack = 0, Release };

    struct Region {
        std::shared_ptr<const SampleData> sample;
        int   loKey = 0, hiKey = 127, loVel = 1, hiVel = 127;
        int   keyCenter = 60;
        float keytrack = 100.0f;       // cents per key
        float tuneCents = 0.0f;        // tune + transpose·100
        float gain = 1.0f;             // from volume (dB)
        float pan = 0.0f;              // -1..1
        std::int64_t offset = 0, end = -1;   // end = -1: to the sample end
        SamplePlayer::Loop loop = SamplePlayer::Loop::Off;
        bool  oneShot = false;         // loop_mode=one_shot: ignores note-off (the amp envelope still applies)
        std::int64_t loopStart = -1, loopEnd = -1;   // -1: from the sample's smpl chunk
        double loopXfadeSeconds = 0.0;
        std::array<float, 128> velCurve {};          // amplitude by velocity (amp_velcurve, default linear-squared)
        int   seqLength = 1, seqPosition = 1;
        Trigger trigger = Trigger::Attack;
    };

    std::vector<Region> regions;
    std::string name;
    std::uint64_t id = nextAssetId();
};

} // namespace winerose::dsp
