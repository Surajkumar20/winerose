# winerose_ui_juce — JUCE editor components. Talks to winerose_control (IController) ONLY (PLAN.md §1.1).
#
# INTERFACE library: JUCE module code is compiled into whichever target links JUCE, so a second static
# library linking JUCE modules would compile them twice. The sources are therefore compiled inside the
# plugin target; the rule "UI never sees params/engine/presets" is enforced by the link check below plus
# an #include scan of this directory.
add_library(winerose_ui_juce INTERFACE)
add_library(winerose::ui_juce ALIAS winerose_ui_juce)

target_sources(winerose_ui_juce INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WineroseEditor.cpp")
target_include_directories(winerose_ui_juce INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(winerose_ui_juce INTERFACE winerose_control)

winerose_forbid_links(winerose_ui_juce "^winerose_(params|engine|presets)$")
winerose_forbid_includes("${CMAKE_CURRENT_SOURCE_DIR}" "\"(params|engine|presets|plugin)/" "control/Controller\\.h")
