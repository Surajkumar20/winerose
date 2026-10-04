#include "engine/fx/Effects.h"

namespace winerose::fx {

namespace {

using Names = std::array<const char*, kParamCount>;
using Defaults = std::array<float, kParamCount>;

struct TypeInfo { Names names; Defaults defaults; };

// Generic slot parameters p0..p7 per type. A parameter is a 0..1 knob; "choice" knobs pick among the listed
// options in equal steps. Defaults are neutral-ish starting points (TODO-MEASURE against Serum's).
const TypeInfo kInfo[] = {
    /* None */       {{"", "", "", "", "", "", "", ""}, {0, 0, 0, 0, 0, 0, 0, 0}},
    /* Distortion */ {{"Drive (0-48 dB)", "Mode (Tube/Soft/Hard/Diode/Lin Fold/Sin Fold/Zero-Sq/Downsample)",
                       "Filter (Off/Pre LP/BP/HP/Post LP/BP/HP)", "Filter freq", "Filter Q", "DC bias",
                       "Output (±24 dB)", ""}, {0.3f, 0.2f, 0, 0.5f, 0.3f, 0.5f, 0.5f, 0}},
    /* Flanger */    {{"Rate (0.01-20 Hz)", "Depth", "Feedback (±)", "Stereo phase", "", "", "", ""},
                      {0.35f, 0.5f, 0.6f, 0.5f, 0, 0, 0, 0}},
    /* Phaser */     {{"Rate (0.01-10 Hz)", "Depth", "Centre freq", "Feedback (±)", "Stereo phase", "Stages (2-12)", "", ""},
                      {0.35f, 0.6f, 0.5f, 0.6f, 0.5f, 0.3f, 0, 0}},
    /* Chorus */     {{"Rate (0.05-5 Hz)", "Depth", "Delay 1 (1-30 ms)", "Delay 2 (1-30 ms)", "Feedback", "Low-pass",
                       "", ""}, {0.4f, 0.4f, 0.25f, 0.45f, 0, 1.0f, 0, 0}},
    /* Delay */      {{"Mode (Normal/Ping-Pong/Tap)", "Sync", "Time (1-2000 ms / division)", "Right/left ratio",
                       "Feedback", "Filter freq", "Filter width (0 = off)", ""}, {0, 0, 0.6f, 0.5f, 0.4f, 0.5f, 0, 0}},
    /* Compressor */ {{"Threshold (-60-0 dB)", "Ratio (1-20, top = limit)", "Attack (0.1-100 ms)", "Release (5-1000 ms)",
                       "Makeup (0-36 dB)", "Knee (0-12 dB)", "", ""}, {0.67f, 0.46f, 0.5f, 0.5f, 0, 0.25f, 0, 0}},
    /* Multiband */  {{"Depth", "Time", "Upward", "Downward", "Low gain (±12 dB)", "Mid gain", "High gain", "Output"},
                      {0.5f, 0.5f, 1.0f, 1.0f, 0.5f, 0.5f, 0.5f, 0.5f}},
    /* Reverb */     {{"Mode (Plate/Hall)", "Decay (0.1-20 s RT60)", "Size", "Pre-delay (0-250 ms)", "Low cut",
                       "High cut", "Damping", "Width"}, {0, 0.45f, 0.5f, 0, 0, 1.0f, 0.3f, 1.0f}},
    /* EQ */         {{"Low type (Shelf/Peak/HP)", "Low freq", "Low gain (±24 dB)", "Low Q", "High type (Shelf/Peak/LP)",
                       "High freq", "High gain (±24 dB)", "High Q"}, {0, 0.33f, 0.5f, 0.35f, 0, 0.6f, 0.5f, 0.35f}},
    /* Filter */     {{"Type", "Cutoff", "Resonance", "Drive", "Var", "Stereo", "X", "Y"},
                      {static_cast<float>(winerose::dsp::FilterType::Lp12) / static_cast<float>(winerose::dsp::FilterType::Count) + 0.001f,
                       0.7f, 0.1f, 0, 0.5f, 0, 0, 0.5f}},
    /* Hyper */      {{"Rate", "Detune", "Unison (1-7)", "Hyper mix", "", "Dimension size", "Dimension mix", ""},
                      {0.5f, 0.3f, 0.5f, 0.5f, 0, 0.5f, 0, 0}},
    /* Bode */       {{"Shift (±5 kHz, centre = 0)", "Sideband (up → down)", "Feedback", "", "", "", "", ""},
                      {0.6f, 0, 0, 0, 0, 0, 0, 0}},
    /* Convolve */   {{"Length (0.05-2.5 s)", "Pre-delay", "Low cut", "High cut", "", "", "", ""},
                      {0.6f, 0, 0, 1.0f, 0, 0, 0, 0}},
    /* Utility */    {{"Gain (-48-+12 dB)", "Pan", "Width (0-200%)", "Invert L", "Invert R", "Swap L/R",
                       "Bass mono (off / 20-500 Hz)", ""}, {0.8f, 0.5f, 0.5f, 0, 0, 0, 0, 0}},
    /* Split L/H */  {{"Crossover", "Low level (±24 dB)", "High level (±24 dB)", "", "", "", "", ""},
                      {0.5f, 0.5f, 0.5f, 0, 0, 0, 0, 0}},
    /* Split L/M/H */{{"Low crossover", "High crossover", "Low level", "Mid level", "High level", "", "", ""},
                      {0.3f, 0.7f, 0.5f, 0.5f, 0.5f, 0, 0, 0}},
    /* Split M/S */  {{"Mid level (±24 dB)", "Side level (±24 dB)", "", "", "", "", "", ""},
                      {0.5f, 0.5f, 0, 0, 0, 0, 0, 0}},
};
static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == static_cast<int>(FxType::Count));

} // namespace

const std::array<const char*, kParamCount>& paramNames(FxType type) noexcept
{
    return kInfo[static_cast<int>(type)].names;
}

const std::array<float, kParamCount>& defaultParams(FxType type) noexcept
{
    return kInfo[static_cast<int>(type)].defaults;
}

std::unique_ptr<Effect> createEffect(FxType type, double sampleRate)
{
    std::unique_ptr<Effect> e;
    switch (type) {
        case FxType::Distortion: e = std::make_unique<Distortion>(); break;
        case FxType::Flanger:    e = std::make_unique<Flanger>(); break;
        case FxType::Phaser:     e = std::make_unique<Phaser>(); break;
        case FxType::Chorus:     e = std::make_unique<Chorus>(); break;
        case FxType::Delay:      e = std::make_unique<Delay>(); break;
        case FxType::Compressor: e = std::make_unique<Compressor>(); break;
        case FxType::Multiband:  e = std::make_unique<Multiband>(); break;
        case FxType::Reverb:     e = std::make_unique<Reverb>(); break;
        case FxType::Eq:         e = std::make_unique<Eq>(); break;
        case FxType::Filter:     e = std::make_unique<FilterFx>(); break;
        case FxType::Hyper:      e = std::make_unique<Hyper>(); break;
        case FxType::Bode:       e = std::make_unique<Bode>(); break;
        case FxType::Convolve:   e = std::make_unique<Convolve>(); break;
        case FxType::Utility:    e = std::make_unique<Utility>(); break;
        default:                 return nullptr;   // None and splitters: handled by the rack
    }
    e->prepare(sampleRate);
    e->setParams(defaultParams(type), FxContext{});
    return e;
}

} // namespace winerose::fx
