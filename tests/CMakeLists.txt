# Unit tests for the JUCE-free layers. They link only params/engine/presets/control, so they also prove
# those layers build and run with no JUCE present. feature/build grows this into the full test suite
# (golden renders, layering tests, pluginval) — PLAN.md §2.
include(Catch)

add_executable(winerose_tests
    unit/test_param_types.cpp
    unit/test_param_registry.cpp
    unit/test_config_manager.cpp
    unit/test_snapshot_exchange.cpp
    unit/test_global_settings.cpp
    unit/test_engine.cpp
    unit/test_controller.cpp
    unit/test_preset_format.cpp)

target_link_libraries(winerose_tests PRIVATE
    Catch2::Catch2WithMain
    winerose_params winerose_engine winerose_presets winerose_control)
winerose_target_defaults(winerose_tests)
set_target_properties(winerose_tests PROPERTIES FOLDER "Winerose/Tests")

catch_discover_tests(winerose_tests)
