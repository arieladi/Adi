# SPDX-License-Identifier: AGPL-3.0-only
# Standalone build: the pinned ZL JUCE fork and DSP stay outside the DAW binary.
cmake_minimum_required(VERSION 3.24.1)
project(adi_dynamic_eq VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ZL_SOURCE "" CACHE PATH "ZLEqualizer 3468a3a checkout, with submodules")
find_package(Git REQUIRED)
execute_process(COMMAND ${GIT_EXECUTABLE} -C "${ZL_SOURCE}" rev-parse HEAD
    OUTPUT_VARIABLE zl_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT zl_revision STREQUAL "3468a3ac85f5c1f9d16083acbee5b1339984d53b")
    message(FATAL_ERROR "Dynamic EQ requires the ADR-0166 pinned ZL revision; got ${zl_revision}")
endif()
add_subdirectory(${ZL_SOURCE}/JUCE juce)
set(BUILD_SHARED_LIBS OFF)
set(NLOPT_PYTHON OFF CACHE BOOL "" FORCE)
set(NLOPT_OCTAVE OFF CACHE BOOL "" FORCE)
set(NLOPT_MATLAB OFF CACHE BOOL "" FORCE)
set(NLOPT_GUILE OFF CACHE BOOL "" FORCE)
set(NLOPT_SWIG OFF CACHE BOOL "" FORCE)
add_subdirectory(${ZL_SOURCE}/nlopt nlopt)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
add_compile_definitions(HWY_COMPILE_ONLY_STATIC)
add_subdirectory(${ZL_SOURCE}/highway highway)
include(FetchContent)
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG 55525c9858d4b25687be7759a5e0f70eccef218e
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(clap-juce-extensions)
juce_add_plugin(adi_dynamic_eq
    COMPANY_NAME ADI PLUGIN_MANUFACTURER_CODE Adiz PLUGIN_CODE Adqe
    PRODUCT_NAME "ADI Dynamic EQ" VERSION 0.1.0
    FORMATS VST3 IS_SYNTH FALSE NEEDS_MIDI_INPUT FALSE NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE VST3_CATEGORIES Fx EQ)
# Keep ZL's controller, dynamics, sidechain and all filter structures intact.
file(GLOB_RECURSE zl_dsp CONFIGURE_DEPENDS "${ZL_SOURCE}/source/dsp/*.cpp" "${ZL_SOURCE}/source/zlp/*.cpp")
target_sources(adi_dynamic_eq PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp ${zl_dsp})
target_include_directories(adi_dynamic_eq PUBLIC Source)
target_include_directories(adi_dynamic_eq SYSTEM PUBLIC "${ZL_SOURCE}/source")
target_compile_definitions(adi_dynamic_eq PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 ZL_EQ_BAND_NUM=24)
target_link_libraries(adi_dynamic_eq PRIVATE juce::juce_audio_utils juce::juce_dsp nlopt hwy
    PUBLIC juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(adi_dynamic_eq PUBLIC /Zc:__cplusplus /fp:precise)
    set_source_files_properties(Source/PluginProcessor.cpp Source/PluginEditor.cpp
        PROPERTIES COMPILE_OPTIONS "/W4;/WX")
endif()
clap_juce_extensions_plugin(TARGET adi_dynamic_eq CLAP_ID "com.adi.dynamic-eq"
    CLAP_FEATURES audio-effect equalizer stereo)
enable_testing()
add_executable(dynamic_eq_gestures tests/test_gestures.cpp)
target_include_directories(dynamic_eq_gestures PRIVATE Source)
if(MSVC)
    target_compile_options(dynamic_eq_gestures PRIVATE /W4 /WX)
else()
    target_compile_options(dynamic_eq_gestures PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Werror)
endif()
add_test(NAME dynamic_eq_gestures COMMAND dynamic_eq_gestures)
# Independent upstream processor oracle: only rename its symbols and remove its
# editor construction. Its process/state/parameter code is not adapted.
file(READ "${ZL_SOURCE}/source/PluginProcessor.hpp" baseline_h)
string(REPLACE "PluginProcessor" "ZlBaselineProcessor" baseline_h "${baseline_h}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Baseline.hpp" "${baseline_h}")
file(READ "${ZL_SOURCE}/source/PluginProcessor.cpp" baseline_cpp)
string(REPLACE "PluginProcessor" "ZlBaselineProcessor" baseline_cpp "${baseline_cpp}")
string(REPLACE "\"ZlBaselineProcessor.hpp\"" "\"Baseline.hpp\"" baseline_cpp "${baseline_cpp}")
string(REPLACE "#include \"PluginEditor.hpp\"" "" baseline_cpp "${baseline_cpp}")
string(REPLACE "return new PluginEditor(*this);" "return nullptr;" baseline_cpp "${baseline_cpp}")
string(REPLACE "createPluginFilter()" "createBaselineFilter()" baseline_cpp "${baseline_cpp}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/Baseline.cpp" "${baseline_cpp}")
add_executable(dynamic_eq_processor tests/test_processor.cpp "${CMAKE_CURRENT_BINARY_DIR}/Baseline.cpp")
target_include_directories(dynamic_eq_processor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(dynamic_eq_processor PRIVATE adi_dynamic_eq juce::juce_audio_utils juce::juce_dsp nlopt hwy)
add_test(NAME dynamic_eq_processor COMMAND dynamic_eq_processor)
add_executable(dynamic_eq_clap tests/test_clap.cpp)
target_link_libraries(dynamic_eq_clap PRIVATE clap ${CMAKE_DL_LIBS})
add_dependencies(dynamic_eq_clap adi_dynamic_eq_CLAP)
add_test(NAME dynamic_eq_clap COMMAND dynamic_eq_clap $<TARGET_FILE:adi_dynamic_eq_CLAP>)
