# param_schema — headless schema export (JUCE-free).
add_executable(param_schema main.cpp)
target_link_libraries(param_schema PRIVATE winerose_control winerose_engine winerose_params)
winerose_target_defaults(param_schema)
set_target_properties(param_schema PROPERTIES FOLDER "Winerose/Tools")
winerose_forbid_links(param_schema "^juce")

if(WINEROSE_BUILD_TESTS)
    # Two runs of the same build must produce byte-identical output.
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/schema_test")
    add_test(NAME tools.param_schema.run_a
        COMMAND param_schema --out "${_out}/a" --json "${_out}/a.json" --state "${_out}/a.state.json")
    add_test(NAME tools.param_schema.run_b
        COMMAND param_schema --out "${_out}/b" --json "${_out}/b.json" --state "${_out}/b.state.json")
    add_test(NAME tools.param_schema.deterministic_ini
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_out}/a/Global/schema.ini" "${_out}/b/Global/schema.ini")
    add_test(NAME tools.param_schema.deterministic_json
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_out}/a.json" "${_out}/b.json")
    set_tests_properties(tools.param_schema.run_a PROPERTIES FIXTURES_SETUP param_schema_a)
    set_tests_properties(tools.param_schema.run_b PROPERTIES FIXTURES_SETUP param_schema_b)
    set_tests_properties(tools.param_schema.deterministic_ini tools.param_schema.deterministic_json
        PROPERTIES FIXTURES_REQUIRED "param_schema_a;param_schema_b")
endif()
