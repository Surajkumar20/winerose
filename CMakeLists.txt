cmake_minimum_required(VERSION 3.25)

project(Winerose VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: a plugin .vst3 must not depend on the user having the matching VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# WINEROSE_BUILD_PLUGIN=OFF builds every layer below the JUCE adapters without fetching JUCE at all —
# the proof that params/engine/presets/control are host- and UI-agnostic (PLAN.md §1).
option(WINEROSE_BUILD_PLUGIN "Build the JUCE plugin adapter + JUCE UI (VST3/Standalone)" ON)
option(WINEROSE_BUILD_CLAP   "Also build a CLAP via clap-juce-extensions"                ON)
option(WINEROSE_BUILD_TESTS  "Build Catch2 unit tests"                                   ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(WineroseDependencies)
include(WineroseTargetDefaults)
include(WineroseLayering)

# Layers, bottom-up (PLAN.md §1.1). Each one is its own target with its own include root.
add_subdirectory(src/params)
add_subdirectory(src/engine)
add_subdirectory(src/presets)
add_subdirectory(src/control)
if(WINEROSE_BUILD_PLUGIN)
    add_subdirectory(src/ui)
    add_subdirectory(src/plugin)
endif()

if(WINEROSE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Fails configuration if any layer links or includes something PLAN.md §1.1 forbids.
winerose_check_layering()
