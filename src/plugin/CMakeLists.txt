# Winerose — the JUCE host adapter (VST3 / Standalone / AU on macOS / CLAP). PLAN.md §3.4.
# No DSP or parameter logic lives here: it wires JUCE to the engine, the control layer and the JUCE UI.

set(WINEROSE_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND WINEROSE_FORMATS AU)
endif()

juce_add_plugin(Winerose
    PRODUCT_NAME                "Winerose"
    COMPANY_NAME                "Winerose"
    BUNDLE_ID                   "com.winerose.winerose"
    PLUGIN_MANUFACTURER_CODE    Wnro
    PLUGIN_CODE                 Wnrs
    FORMATS                     ${WINEROSE_FORMATS}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Instrument Synth
    COPY_PLUGIN_AFTER_BUILD     FALSE)

target_sources(Winerose PRIVATE
    WineroseProcessor.cpp
    RegistryParameter.cpp
    HostSync.cpp)

target_compile_features(Winerose PRIVATE cxx_std_20)

target_compile_definitions(Winerose PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_WEB_BROWSER=0            # flips to 1 when the WebView UI starts (PLAN.md §1.4)
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(MSVC)
    target_compile_options(Winerose PRIVATE /utf-8 /Zc:__cplusplus)
endif()

target_link_libraries(Winerose
    PRIVATE
        winerose_ui_juce
        winerose_control
        winerose_engine
        winerose_params
        winerose_presets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(WINEROSE_BUILD_CLAP)
    clap_juce_extensions_plugin(TARGET Winerose
        CLAP_ID       "com.winerose.winerose"
        CLAP_FEATURES instrument synthesizer stereo)
endif()

# pluginval as a ctest (scripts/build.ps1 downloads pluginval and sets this). Strictness 10 is the
# release bar from SPEC §5.4.
set(WINEROSE_PLUGINVAL "" CACHE FILEPATH "pluginval executable; when set, ctest validates the VST3 with it")
set(WINEROSE_PLUGINVAL_STRICTNESS 10 CACHE STRING "pluginval --strictness-level")
if(WINEROSE_BUILD_TESTS AND WINEROSE_PLUGINVAL)
    add_test(NAME plugin.pluginval
        COMMAND "${WINEROSE_PLUGINVAL}"
                --strictness-level ${WINEROSE_PLUGINVAL_STRICTNESS}
                --validate-in-process
                --output-dir "${CMAKE_BINARY_DIR}/pluginval"
                --validate "$<TARGET_FILE_DIR:Winerose_VST3>/../../../Winerose.vst3")
    set_tests_properties(plugin.pluginval PROPERTIES TIMEOUT 1200 LABELS "slow")
endif()
