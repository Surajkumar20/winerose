# Fixture project for the layering-checker tests (tests/CMakeLists.txt). Each CASE configures a tiny
# target graph; the "violation" cases must FAIL configuration and the "allowed" cases must pass.
cmake_minimum_required(VERSION 3.25)
project(winerose_arch_fixture LANGUAGES NONE)

include("${WINEROSE_CMAKE_DIR}/WineroseLayering.cmake")

add_library(juce_fake INTERFACE)
add_library(lower INTERFACE)
add_library(middle INTERFACE)
add_library(upper INTERFACE)

if(CASE STREQUAL "clean")
    target_link_libraries(upper INTERFACE middle)
    target_link_libraries(middle INTERFACE lower)
elseif(CASE STREQUAL "direct_link")
    target_link_libraries(lower INTERFACE juce_fake)
elseif(CASE STREQUAL "transitive_link")
    target_link_libraries(lower INTERFACE middle)
    target_link_libraries(middle INTERFACE juce_fake)
elseif(CASE STREQUAL "link_only_allowed")
    # A PRIVATE dependency of a static library shows up as $<LINK_ONLY:...>: it is not a usage
    # requirement, so it must not count (this is how control hides engine from the UI).
    target_link_libraries(lower INTERFACE middle)
    set_property(TARGET middle PROPERTY INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:juce_fake>")
elseif(CASE STREQUAL "include")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/src/bad.cpp" "#include <juce_core/juce_core.h>\n")
elseif(CASE STREQUAL "include_comment_allowed")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/src/ok.cpp" "// mentions juce_core but includes nothing\n#include <vector>\n")
else()
    message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()

winerose_forbid_links(lower "^juce")
winerose_forbid_includes("${CMAKE_CURRENT_BINARY_DIR}/src" "juce_")
winerose_check_layering()
