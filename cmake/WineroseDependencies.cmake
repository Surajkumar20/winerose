# Third-party dependencies. Pins follow PDF SPEC §5.2, bumped where the SPEC's pins predate the
# toolchain (JUCE 8.0.4 → 8.0.15 for Visual Studio 2026 / MSVC 14.5x).
# Only what the current branch actually uses is fetched; later branches add theirs here
# (zstd → feature/presets, xsimd/pffft/HIIR → feature/audio_engine / feature/build).
include(FetchContent)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(nlohmann_json)

if(WINEROSE_BUILD_PLUGIN)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.15
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)

    if(WINEROSE_BUILD_CLAP)
        # Pinned to a commit: the 0.26.0 tag predates JUCE 8.0.11's move of LegacyAudioParameter into
        # juce_audio_processors_headless and fails to compile against 8.0.15.
        FetchContent_Declare(clap-juce-extensions
            GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
            GIT_TAG        55525c9858d4b25687be7759a5e0f70eccef218e)
        FetchContent_MakeAvailable(clap-juce-extensions)
    endif()
endif()

if(WINEROSE_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.9.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
