# winerose_render — offline host for the engine (golden tests, measurement, benchmarks). JUCE-free.
add_library(winerose_render STATIC src/OfflineRender.cpp)
add_library(winerose::render ALIAS winerose_render)

target_include_directories(winerose_render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(winerose_render PUBLIC winerose_engine)
winerose_target_defaults(winerose_render)
set_target_properties(winerose_render PROPERTIES FOLDER "Winerose/Tools")

winerose_forbid_links(winerose_render "^juce" "^winerose_(control|ui_juce)$" "^Winerose")
winerose_forbid_includes("${CMAKE_CURRENT_SOURCE_DIR}" "juce_" "\"(control|ui|plugin)/")
