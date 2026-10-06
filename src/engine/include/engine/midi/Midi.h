#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace winerose::midi {

// MIDI generators in front of the voices (SPEC §1.7): key/scale quantizer, arpeggiator, clip sequencer.
// Enum orders are Winerose's own (append only); Serum orders are mapped by the preset layer.

enum class Scale : int {
    Chromatic = 0, Major, Minor, HarmonicMinor, MelodicMinor, Dorian, Phrygian, Lydian, Mixolydian, Locrian,
    MajorPentatonic, MinorPentatonic, Blues, WholeTone, Count
};
inline constexpr const char* kScaleNames[] = {"Chromatic", "Major", "Minor", "Harmonic Minor", "Melodic Minor", "Dorian",
                                              "Phrygian", "Lydian", "Mixolydian", "Locrian", "Major Pentatonic",
                                              "Minor Pentatonic", "Blues", "Whole Tone"};
inline constexpr const char* kKeyNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/** Pitch classes in a scale (bit n = n semitones above the key). */
std::uint16_t scaleMask(Scale s) noexcept;

/** Nearest in-scale note (ties go down), after transposition. Chromatic returns note + transpose. */
int quantize(int note, int key, Scale scale, int transpose) noexcept;

enum class ArpMode : int { Up = 0, Down, UpDown, DownUp, Order, Random, Chord, Count };
inline constexpr const char* kArpModeNames[] = {"Up", "Down", "Up/Down", "Down/Up", "As played", "Random", "Chord"};

enum class Division : int { D1 = 0, D2, D4, D4Dot, D4T, D8, D8Dot, D8T, D16, D16Dot, D16T, D32, Count };
inline constexpr const char* kDivisionNames[] = {"1/1", "1/2", "1/4", "1/4.", "1/4T", "1/8", "1/8.", "1/8T", "1/16", "1/16.", "1/16T", "1/32"};
inline constexpr double kDivisionBeats[] = {4.0, 2.0, 1.0, 1.5, 2.0 / 3.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 0.375, 1.0 / 6.0, 0.125};

enum class VelocityMode : int { AsPlayed = 0, Fixed, Ramp, Count };
inline constexpr const char* kVelocityModeNames[] = {"As played", "Fixed", "Ramp"};

enum class ClipTrigger : int { Note = 0, Host, Count };
inline constexpr const char* kClipTriggerNames[] = {"Note (transposed from C4)", "Host transport"};

inline constexpr int kClipSlots = 12;

/** A MIDI clip: notes in beats, looping every lengthBeats. Text form: "start,length,note,velocity;..." */
struct Clip {
    struct Note {
        double start = 0.0, length = 0.25;
        int    note = 60, velocity = 100;
    };
    std::vector<Note> notes;   // sorted by start
    double lengthBeats = 4.0;

    static Clip parse(const std::string& text, double lengthBeats);
    std::string serialize() const;
};

/** Everything the NoteProcessor needs, read from the parameters once per control tick. Plain data. */
struct Settings {
    bool  scaleOn = false;
    int   key = 0;
    Scale scale = Scale::Chromatic;
    int   transpose = 0;

    bool         arpOn = false;
    ArpMode      arpMode = ArpMode::Up;
    double       arpStepBeats = 0.25;
    int          arpOctaves = 1;
    float        arpGate = 0.5f;      // fraction of a step (1 = legato)
    float        arpSwing = 0.0f;     // 0..1: odd steps delayed by up to a third of a step
    float        arpChance = 1.0f;    // probability each step plays
    VelocityMode arpVelocityMode = VelocityMode::AsPlayed;
    int          arpVelocity = 100;
    bool         arpLatch = false;
    int          arpTranspose = 0;

    bool        clipOn = false;
    int         clip = 0;
    ClipTrigger clipTrigger = ClipTrigger::Note;
    float       clipSwing = 0.0f;
};

} // namespace winerose::midi
