# winerose_params — the bottom layer. Standard library + nlohmann_json only (PLAN.md §1.1, §3).
add_library(winerose_params STATIC
    src/ParamTypes.cpp
    src/ConfigManager.cpp
    src/ParamRegistry.cpp
    src/GlobalSettings.cpp)
add_library(winerose::params ALIAS winerose_params)

target_include_directories(winerose_params PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(winerose_params PRIVATE nlohmann_json::nlohmann_json)
winerose_target_defaults(winerose_params)

winerose_forbid_links(winerose_params "^juce" "^winerose_(engine|presets|control|ui_juce)$" "^Winerose")
winerose_forbid_includes("${CMAKE_CURRENT_SOURCE_DIR}" "juce_" "\"(engine|presets|control|ui|plugin)/")
