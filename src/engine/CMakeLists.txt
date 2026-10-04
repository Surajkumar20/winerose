# winerose_engine — the synth. Depends on winerose_params only; never on JUCE (PLAN.md §1.1-1.3).
add_library(winerose_engine STATIC
    src/Engine.cpp)
add_library(winerose::engine ALIAS winerose_engine)

target_include_directories(winerose_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(winerose_engine PUBLIC winerose_params)
winerose_target_defaults(winerose_engine)

winerose_forbid_links(winerose_engine "^juce" "^winerose_(presets|control|ui_juce)$" "^Winerose")
winerose_forbid_includes("${CMAKE_CURRENT_SOURCE_DIR}" "juce_" "\"(presets|control|ui|plugin)/")
