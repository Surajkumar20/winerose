# winerose_control — the UI-agnostic control API (IController) every UI talks to (PLAN.md §1.4).
# Engine/params/presets are PRIVATE: a UI linking this target sees IController and the JSON-able
# control types, nothing below them.
add_library(winerose_control STATIC
    src/Controller.cpp
    src/AssetLoader.cpp
    src/EditHistory.cpp)
add_library(winerose::control ALIAS winerose_control)

target_include_directories(winerose_control PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(winerose_control
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE winerose_params winerose_engine winerose_presets)
winerose_target_defaults(winerose_control)

winerose_forbid_links(winerose_control "^juce" "^winerose_ui_juce$" "^Winerose")
winerose_forbid_includes("${CMAKE_CURRENT_SOURCE_DIR}" "juce_" "\"(ui|plugin)/")
