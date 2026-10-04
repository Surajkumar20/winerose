#pragma once

#include <cstdint>

namespace winerose::dsp {

/**
 * @brief Winerose's filter list (SPEC §1.5, Phase 4).
 *
 * This is OUR list, in OUR order, with descriptive names: the enum index is what hosts store for
 * automation, so it must never depend on measurements of another product. Serum's ~107-entry filter menu is
 * mapped onto these types by the compat layer (feature/presets, compat/serum_tables.hpp), so the
 * TODO-MEASURE work on Serum's list never renumbers ours. Append new types at the end only.
 *
 * Families and their published algorithms (SPEC §1.5 table):
 *   Svf / OnePole   TPT state-variable / one-pole (Zavalishin ch. 3-4, Simper)          linear
 *   Ladder          4 × TPT one-pole with zero-delay global feedback (Zavalishin ch. 5)  linear
 *   DrivenLadder    ladder with tanh on the feedback path and stage inputs, 2× OS       nonlinear (INFERRED)
 *   Acid            driven ladder with mismatched stages + asymmetric drive, 2× OS      nonlinear (INFERRED)
 *   SallenKey       2-pole with tanh-limited resonance path, 2× OS                       nonlinear (INFERRED)
 *   Comb / Flange   (fractional) delay at 1/cutoff with feedback / feedforward          linear
 *   Phaser          N first-order TPT allpasses + feedback                              linear
 *   Formant         3 parallel band-passes over vowel formant tables (Peterson & Barney) linear
 *   SampleHold      zero-order-hold decimation at the cutoff rate                       nonlinear
 *   RingMod         multiply by a sine at the cutoff frequency                           linear (time-varying)
 *   Reverb          4-line Householder FDN tuned by the cutoff (Jot & Chaigne)           linear
 *   Disperser       cascade of 2nd-order allpasses at the cutoff                         linear
 *   Diffuser        Schroeder allpass delays scaled by the cutoff                        linear
 *   PzMorph         SVF responses morphed by X (LP→BP→HP) and Y (notch ↔ peak)          linear
 */
enum class FilterType : int {
    // SVF / one-pole
    Lp6 = 0, Lp12, Lp18, Lp24,
    Hp6, Hp12, Hp18, Hp24,
    Bp12, Bp24, Notch12, Notch24, Peak12, Peak24, Allpass12, Allpass24,
    // SVF morphs (Var = morph position)
    MorphLBH, MorphLPH, MorphLNH, MorphBPN,
    // Dual SVF (Var = second cutoff, ±4 octaves)
    DualLp, DualHp, DualBp, DualNotch, DualPeak, LpHp,
    // Ladders
    Ladder6, Ladder12, Ladder18, Ladder24, LadderHp24,
    DrivenLadder12, DrivenLadder24,
    Acid18, Acid24,
    SkLp12, SkHp12, SkBp12,
    // Delay-based (Var: damping / mix / spread)
    CombPlus, CombMinus, CombDampedPlus, CombDampedMinus, FlangePlus, FlangeMinus,
    Phaser4, Phaser8, Phaser12, PhaserMinus8,
    // Formant (Var = vowel A-E-I-O-U)
    FormantMale, FormantFemale,
    // Misc
    SampleHold, RingMod, TwinLp, Reverb, Disperser, Diffuser, PzMorph,
    Count
};

enum class FilterFamily : std::uint8_t {
    OnePole, Svf, SvfMorph, SvfDual, Ladder, DrivenLadder, Acid, SallenKey, Comb, Flange, Phaser,
    Formant, SampleHold, RingMod, TwinLp, Reverb, Disperser, Diffuser, PzMorph
};

enum class FilterMode : std::uint8_t { Lp, Hp, Bp, Notch, Peak, Allpass, None };

struct FilterInfo {
    const char*  name;
    FilterFamily family;
    FilterMode   mode;
    int          poles;       // slope in poles (6 dB each) where meaningful
    bool         nonlinear;   // runs at 2× oversampling
    int          variant;     // family-specific (morph kind, polarity, stage count, ...)
};

inline constexpr FilterInfo kFilterInfo[] = {
    {"LP 6",            FilterFamily::OnePole, FilterMode::Lp,      1, false, 0},
    {"LP 12",           FilterFamily::Svf,     FilterMode::Lp,      2, false, 0},
    {"LP 18",           FilterFamily::Svf,     FilterMode::Lp,      3, false, 0},
    {"LP 24",           FilterFamily::Svf,     FilterMode::Lp,      4, false, 0},
    {"HP 6",            FilterFamily::OnePole, FilterMode::Hp,      1, false, 0},
    {"HP 12",           FilterFamily::Svf,     FilterMode::Hp,      2, false, 0},
    {"HP 18",           FilterFamily::Svf,     FilterMode::Hp,      3, false, 0},
    {"HP 24",           FilterFamily::Svf,     FilterMode::Hp,      4, false, 0},
    {"BP 12",           FilterFamily::Svf,     FilterMode::Bp,      2, false, 0},
    {"BP 24",           FilterFamily::Svf,     FilterMode::Bp,      4, false, 0},
    {"Notch 12",        FilterFamily::Svf,     FilterMode::Notch,   2, false, 0},
    {"Notch 24",        FilterFamily::Svf,     FilterMode::Notch,   4, false, 0},
    {"Peak 12",         FilterFamily::Svf,     FilterMode::Peak,    2, false, 0},
    {"Peak 24",         FilterFamily::Svf,     FilterMode::Peak,    4, false, 0},
    {"Allpass 12",      FilterFamily::Svf,     FilterMode::Allpass, 2, false, 0},
    {"Allpass 24",      FilterFamily::Svf,     FilterMode::Allpass, 4, false, 0},
    {"Morph L-B-H",     FilterFamily::SvfMorph, FilterMode::None,   2, false, 0},
    {"Morph L-P-H",     FilterFamily::SvfMorph, FilterMode::None,   2, false, 1},
    {"Morph L-N-H",     FilterFamily::SvfMorph, FilterMode::None,   2, false, 2},
    {"Morph B-P-N",     FilterFamily::SvfMorph, FilterMode::None,   2, false, 3},
    {"Dual LP",         FilterFamily::SvfDual, FilterMode::Lp,      2, false, 0},
    {"Dual HP",         FilterFamily::SvfDual, FilterMode::Hp,      2, false, 0},
    {"Dual BP",         FilterFamily::SvfDual, FilterMode::Bp,      2, false, 0},
    {"Dual Notch",      FilterFamily::SvfDual, FilterMode::Notch,   2, false, 0},
    {"Dual Peak",       FilterFamily::SvfDual, FilterMode::Peak,    2, false, 0},
    {"LP + HP",         FilterFamily::SvfDual, FilterMode::None,    2, false, 1},
    {"Ladder 6",        FilterFamily::Ladder,  FilterMode::Lp,      1, false, 0},
    {"Ladder 12",       FilterFamily::Ladder,  FilterMode::Lp,      2, false, 0},
    {"Ladder 18",       FilterFamily::Ladder,  FilterMode::Lp,      3, false, 0},
    {"Ladder 24",       FilterFamily::Ladder,  FilterMode::Lp,      4, false, 0},
    {"Ladder HP 24",    FilterFamily::Ladder,  FilterMode::Hp,      4, false, 0},
    {"Driven Ladder 12", FilterFamily::DrivenLadder, FilterMode::Lp, 2, true, 0},
    {"Driven Ladder 24", FilterFamily::DrivenLadder, FilterMode::Lp, 4, true, 0},
    {"Acid 18",         FilterFamily::Acid,    FilterMode::Lp,      3, true,  0},
    {"Acid 24",         FilterFamily::Acid,    FilterMode::Lp,      4, true,  0},
    {"SK LP 12",        FilterFamily::SallenKey, FilterMode::Lp,    2, true,  0},
    {"SK HP 12",        FilterFamily::SallenKey, FilterMode::Hp,    2, true,  0},
    {"SK BP 12",        FilterFamily::SallenKey, FilterMode::Bp,    2, true,  0},
    {"Comb +",          FilterFamily::Comb,    FilterMode::None,    0, false, 0},
    {"Comb -",          FilterFamily::Comb,    FilterMode::None,    0, false, 1},
    {"Comb Damped +",   FilterFamily::Comb,    FilterMode::None,    0, false, 2},
    {"Comb Damped -",   FilterFamily::Comb,    FilterMode::None,    0, false, 3},
    {"Flange +",        FilterFamily::Flange,  FilterMode::None,    0, false, 0},
    {"Flange -",        FilterFamily::Flange,  FilterMode::None,    0, false, 1},
    {"Phaser 4",        FilterFamily::Phaser,  FilterMode::None,    4, false, 0},
    {"Phaser 8",        FilterFamily::Phaser,  FilterMode::None,    8, false, 0},
    {"Phaser 12",       FilterFamily::Phaser,  FilterMode::None,   12, false, 0},
    {"Phaser 8 -",      FilterFamily::Phaser,  FilterMode::None,    8, false, 1},
    {"Formant Male",    FilterFamily::Formant, FilterMode::None,    0, false, 0},
    {"Formant Female",  FilterFamily::Formant, FilterMode::None,    0, false, 1},
    {"Sample & Hold",   FilterFamily::SampleHold, FilterMode::None, 0, false, 0},
    {"Ring Mod",        FilterFamily::RingMod, FilterMode::None,    0, false, 0},
    {"Twin LP",         FilterFamily::TwinLp,  FilterMode::Lp,      4, false, 0},
    {"Reverb",          FilterFamily::Reverb,  FilterMode::None,    0, false, 0},
    {"Disperser",       FilterFamily::Disperser, FilterMode::None,  0, false, 0},
    {"Diffuser",        FilterFamily::Diffuser, FilterMode::None,   0, false, 0},
    {"PZ Morph",        FilterFamily::PzMorph, FilterMode::None,    2, false, 0},
};
static_assert(sizeof(kFilterInfo) / sizeof(kFilterInfo[0]) == static_cast<int>(FilterType::Count),
              "every FilterType needs a FilterInfo entry");

inline const FilterInfo& filterInfo(FilterType t) noexcept { return kFilterInfo[static_cast<int>(t)]; }

} // namespace winerose::dsp
