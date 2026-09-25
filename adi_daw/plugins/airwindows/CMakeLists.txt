# SPDX-License-Identifier: GPL-3.0-or-later
#
# ADI Airwindows (ADR-0166): the kept Airwindows effects, each a CLAP plug-in,
# in one "ADI Airwindows.clap". No JUCE and no plug-in SDK: CLAP is a header-only
# C API (third_party/clap, pinned for the DAW's own host).
#
# The effects' sources come from an airwin2rack checkout (MIT), unchanged; only
# the kept ones (effects.cmake, written by tools/airwindows_catalogue.py) are
# compiled. Airwindows is MIT; auto gain is ours (GPLv3), so the module is GPLv3.

include(${CMAKE_CURRENT_SOURCE_DIR}/effects.cmake)
set(AW ${ADI_AIRWIN_SOURCE}/src)

set(aw_sources ${AW}/airwin_consolidated_base.cpp)
foreach(fx IN LISTS ADI_AW_EFFECTS)
    list(APPEND aw_sources ${AW}/autogen_airwin/${fx}.cpp ${AW}/autogen_airwin/${fx}Proc.cpp)
endforeach()

# Airwindows' own code, built as upstream builds it: its warnings are not ours.
add_library(adi_airwindows_fx STATIC ${aw_sources})
target_include_directories(adi_airwindows_fx PUBLIC ${AW})
target_compile_definitions(adi_airwindows_fx PUBLIC _USE_MATH_DEFINES)
set_target_properties(adi_airwindows_fx PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(adi_airwindows_fx PRIVATE /bigobj /w)
else()
    target_compile_options(adi_airwindows_fx PRIVATE -w)
endif()

# The wrapper and auto gain, as a library the module and the tests share.
add_library(adi_airwindows_core STATIC
    Source/airwindows_clap.cpp
    ${ADI_ROOT}/src/adi/dsp/auto_gain.cpp)
target_include_directories(adi_airwindows_core PUBLIC
    Source ${ADI_ROOT}/src ${ADI_ROOT}/third_party/clap/include)
target_link_libraries(adi_airwindows_core PUBLIC adi_airwindows_fx)
set_target_properties(adi_airwindows_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(adi_airwindows_core PRIVATE /W4 /bigobj)
endif()

add_library(adi_airwindows MODULE Source/entry.cpp)
target_link_libraries(adi_airwindows PRIVATE adi_airwindows_core)
set_target_properties(adi_airwindows PROPERTIES
    OUTPUT_NAME "ADI Airwindows"
    PREFIX ""
    SUFFIX ".clap"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/airwindows/CLAP
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
if (APPLE)
    set_target_properties(adi_airwindows PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION clap)
endif()

# Every kept effect, the way a host drives it: the factory, the parameters,
# processing with events, state and auto gain.
add_executable(adi_airwindows_tests tests/test_airwindows_clap.cpp)
target_link_libraries(adi_airwindows_tests PRIVATE adi_airwindows_core)
add_test(NAME adi_airwindows_tests COMMAND adi_airwindows_tests)
