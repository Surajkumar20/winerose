# measure_host — JUCE console app hosting a VST3 for black-box measurement (SPEC §5.7). Needs JUCE, so it is
# only built with WINEROSE_BUILD_PLUGIN=ON. It is a tool, not part of the product: no layering rules apply
# beyond "nothing links to it".
juce_add_console_app(measure_host PRODUCT_NAME "measure_host")

target_sources(measure_host PRIVATE main.cpp)
target_compile_features(measure_host PRIVATE cxx_std_20)
target_compile_definitions(measure_host PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
if(MSVC)
    target_compile_options(measure_host PRIVATE /utf-8)
endif()
target_link_libraries(measure_host
    PRIVATE juce::juce_audio_processors juce::juce_audio_utils
    PUBLIC  juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
set_target_properties(measure_host PROPERTIES FOLDER "Winerose/Tools")

if(WINEROSE_BUILD_TESTS)
    # Smoke test: host our own VST3 and find its parameters. Requires the Winerose_VST3 target to be built
    # in the same configuration first (scripts/build.ps1 builds everything before running ctest).
    add_test(NAME tools.measure_host.list_winerose
        COMMAND measure_host list "$<TARGET_FILE_DIR:Winerose_VST3>/../../../Winerose.vst3")
    set_tests_properties(tools.measure_host.list_winerose PROPERTIES
        PASS_REGULAR_EXPRESSION "Global masterVolume")
    add_test(NAME tools.measure_host.sweep_winerose
        COMMAND measure_host sweep "$<TARGET_FILE_DIR:Winerose_VST3>/../../../Winerose.vst3" --steps 3 --match quality)
    set_tests_properties(tools.measure_host.sweep_winerose PROPERTIES
        PASS_REGULAR_EXPRESSION "1\\.000000,Ultra")
endif()

if(WINEROSE_BUILD_TESTS)
    add_test(NAME tools.measure_host.state_winerose
        COMMAND measure_host state "$<TARGET_FILE_DIR:Winerose_VST3>/../../../Winerose.vst3"
                --out "${CMAKE_CURRENT_BINARY_DIR}/state_test/winerose.bin" --set 0=0.25)
    set_tests_properties(tools.measure_host.state_winerose PROPERTIES
        PASS_REGULAR_EXPRESSION "plugin component state"
        FIXTURES_SETUP measure_host_state)
    # End to end: a value set through the VST3 host interface reaches the plugin's saved state.
    if(TARGET preset_dump)
        add_test(NAME tools.measure_host.state_roundtrip
            COMMAND preset_dump "${CMAKE_CURRENT_BINARY_DIR}/state_test/winerose.bin")
        set_tests_properties(tools.measure_host.state_roundtrip PROPERTIES
            FIXTURES_REQUIRED measure_host_state
            PASS_REGULAR_EXPRESSION "\"Global\.masterVolume\": 0\.25")
    endif()
endif()
