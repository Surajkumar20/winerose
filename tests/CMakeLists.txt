# Tests. Everything here is JUCE-free except the measure_host smoke test (tools/measure_host).
#
#   winerose_tests        unit tests for params / engine / presets / control
#   winerose_golden_tests golden renders + block-size invariance via the offline render harness
#   arch.*                the layering checker itself fails on violations and passes clean graphs
#   (tools/*/CMakeLists.txt add their own smoke tests)
include(Catch)

# --- Unit ------------------------------------------------------------------------------------------
add_executable(winerose_tests
    unit/test_param_types.cpp
    unit/test_param_registry.cpp
    unit/test_config_manager.cpp
    unit/test_snapshot_exchange.cpp
    unit/test_global_settings.cpp
    unit/test_engine.cpp
    unit/test_dsp.cpp
    unit/test_warp_unison.cpp
    unit/test_engine_phase2.cpp
    unit/test_modulation.cpp
    unit/test_controller.cpp
    unit/test_preset_format.cpp
    unit/test_vendored_libs.cpp)

target_link_libraries(winerose_tests PRIVATE
    Catch2::Catch2WithMain
    winerose_params winerose_engine winerose_presets winerose_control
    hiir pffft)
winerose_target_defaults(winerose_tests)
set_target_properties(winerose_tests PROPERTIES FOLDER "Winerose/Tests")
catch_discover_tests(winerose_tests TEST_PREFIX "unit.")

# --- Golden ----------------------------------------------------------------------------------------
add_executable(winerose_golden_tests golden/test_golden.cpp)
target_link_libraries(winerose_golden_tests PRIVATE Catch2::Catch2WithMain winerose_render)
target_compile_definitions(winerose_golden_tests PRIVATE
    WINEROSE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden/data"
    WINEROSE_GOLDEN_OUT="${CMAKE_BINARY_DIR}/golden-actual"
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)   # std::getenv
winerose_target_defaults(winerose_golden_tests)
set_target_properties(winerose_golden_tests PROPERTIES FOLDER "Winerose/Tests")
catch_discover_tests(winerose_golden_tests TEST_PREFIX "golden.")

# --- Benchmarks (label "benchmark"; the CPU threshold is asserted only in optimized builds) ------------
add_executable(winerose_benchmark benchmark/test_benchmark.cpp)
target_link_libraries(winerose_benchmark PRIVATE Catch2::Catch2WithMain winerose_engine)
winerose_target_defaults(winerose_benchmark)
set_target_properties(winerose_benchmark PROPERTIES FOLDER "Winerose/Tests")
add_test(NAME benchmark.phase2_cpu COMMAND winerose_benchmark --success)
set_tests_properties(benchmark.phase2_cpu PROPERTIES LABELS "benchmark" TIMEOUT 600)

# --- Architecture (layering checker) ---------------------------------------------------------------
set(_arch_generator_args -G "${CMAKE_GENERATOR}")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND _arch_generator_args -A "${CMAKE_GENERATOR_PLATFORM}")
endif()

function(winerose_arch_case name expect_failure)
    add_test(NAME arch.${name}
        COMMAND "${CMAKE_COMMAND}" ${_arch_generator_args}
                -S "${CMAKE_CURRENT_SOURCE_DIR}/arch/fixture"
                -B "${CMAKE_CURRENT_BINARY_DIR}/arch/${name}"
                -DCASE=${name}
                "-DWINEROSE_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake")
    if(expect_failure)
        set_tests_properties(arch.${name} PROPERTIES PASS_REGULAR_EXPRESSION "Layering violation")
    else()
        set_tests_properties(arch.${name} PROPERTIES PASS_REGULAR_EXPRESSION "layering check passed")
    endif()
endfunction()

winerose_arch_case(clean                   FALSE)
winerose_arch_case(link_only_allowed       FALSE)
winerose_arch_case(include_comment_allowed FALSE)
winerose_arch_case(direct_link             TRUE)
winerose_arch_case(transitive_link         TRUE)
winerose_arch_case(include                 TRUE)
