# winerose_presets — preset/wavetable file I/O and the Serum compatibility layer (feature/presets).
# Depends on winerose_params only: it writes into registries, never into DSP (PLAN.md §1.1).
add_library(winerose_presets STATIC
    src/PresetFormat.cpp)
add_library(winerose::presets ALIAS winerose_presets)

target_include_directories(winerose_presets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(winerose_presets PUBLIC winerose_params)
winerose_target_defaults(winerose_presets)

winerose_forbid_links(winerose_presets "^juce" "^winerose_(engine|control|ui_juce)$" "^Winerose")
winerose_forbid_includes("${CMAKE_CURRENT_SOURCE_DIR}" "juce_" "\"(engine|control|ui|plugin)/")
