# winerose_engine — the synth. Depends on winerose_params (+ vendored DSP libs); never on JUCE
# (PLAN.md §1.1-1.3).
add_library(winerose_engine STATIC
    src/Engine.cpp
    src/dsp/Fft.cpp
    src/dsp/WavetableBank.cpp
    src/dsp/WavetableRead4.cpp
    src/dsp/NoiseTable.cpp
    src/dsp/Curve.cpp
    src/dsp/SampleData.cpp
    src/dsp/Granular.cpp
    src/dsp/Spectral.cpp
    src/dsp/filters/FilterUnit.cpp
    src/modulation/ModTargets.cpp
    src/modulation/Lfo.cpp
    src/modules/EngineModules.cpp
    src/modules/FxModules.cpp
    src/modules/MidiModules.cpp
    src/midi/Midi.cpp
    src/midi/NoteProcessor.cpp
    src/fx/EffectRegistry.cpp
    src/fx/EffectsBasic.cpp
    src/fx/EffectsTime.cpp
    src/fx/EffectsReverb.cpp
    src/fx/EffectsConvolve.cpp
    src/fx/FxRack.cpp
    src/modules/Modules.cpp
    src/voice/Voice.cpp
    src/voice/VoiceManager.cpp)
add_library(winerose::engine ALIAS winerose_engine)

target_include_directories(winerose_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(winerose_engine
    PUBLIC  winerose_params
    PRIVATE pffft
    PUBLIC  hiir)   # Voice.h holds HIIR downsamplers by value
winerose_target_defaults(winerose_engine)

winerose_forbid_links(winerose_engine "^juce" "^winerose_(presets|control|ui_juce)$" "^Winerose")
winerose_forbid_includes("${CMAKE_CURRENT_SOURCE_DIR}" "juce_" "\"(presets|control|ui|plugin)/")
