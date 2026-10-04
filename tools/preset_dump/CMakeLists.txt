# preset_dump — preset container inspector (JUCE-free).
add_executable(preset_dump main.cpp)
target_link_libraries(preset_dump PRIVATE winerose_presets nlohmann_json::nlohmann_json)
winerose_target_defaults(preset_dump)
set_target_properties(preset_dump PROPERTIES FOLDER "Winerose/Tools")
winerose_forbid_links(preset_dump "^juce")

if(WINEROSE_BUILD_TESTS)
    # Reads the default-patch state written by param_schema.
    add_test(NAME tools.preset_dump.winerose_state
        COMMAND preset_dump "${CMAKE_BINARY_DIR}/tools/param_schema/schema_test/a.state.json")
    set_tests_properties(tools.preset_dump.winerose_state PROPERTIES
        FIXTURES_REQUIRED param_schema_a
        PASS_REGULAR_EXPRESSION "Winerose state.*Global\\.masterVolume")
endif()
