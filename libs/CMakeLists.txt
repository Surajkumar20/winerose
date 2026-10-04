# Vendored third-party code (SPEC §5.2: "Vendor HIIR and pffft into libs/"). Both are permissive:
#   hiir  — Laurent de Soras, WTFPL v2 (libs/hiir/LICENSE.txt). From http://ldesoras.free.fr/src/hiir-1.40.zip
#           (its readme reports version 1.33). Header-only apart from PolyphaseIir2Designer.cpp.
#   pffft — Julien Pommier, FFTPACK BSD-like licence (header of libs/pffft/pffft.c).
#           From https://bitbucket.org/jpommier/pffft at 0aec0327a6912e1a0ec5326eef737c2ce19bc836.
# Neither depends on JUCE, so the engine layer may link them.

add_library(hiir STATIC hiir/hiir/PolyphaseIir2Designer.cpp)
target_include_directories(hiir SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/hiir")
target_compile_features(hiir PUBLIC cxx_std_17)
set_target_properties(hiir PROPERTIES FOLDER "ThirdParty")

add_library(pffft STATIC pffft/pffft.c)
target_include_directories(pffft SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/pffft")
if(MSVC)
    target_compile_definitions(pffft PRIVATE _USE_MATH_DEFINES)
endif()
set_target_properties(pffft PROPERTIES FOLDER "ThirdParty")
