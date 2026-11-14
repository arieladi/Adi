# SPDX-License-Identifier: AGPL-3.0-or-later
# Caller supplies the workspace's pinned JUCE (currently 9.0.5). The public
# facade stays standard C++; DSP linkage is private and hosts no plug-in.
if(TARGET adi_mix_clip)
    return()
endif()
add_library(adi_mix_clip STATIC Core.cpp Dsp.cpp)
set_target_properties(adi_mix_clip PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(adi_mix_clip PUBLIC cxx_std_20)
target_include_directories(adi_mix_clip PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
target_link_libraries(adi_mix_clip PRIVATE juce::juce_dsp)
target_compile_definitions(adi_mix_clip PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
if(TARGET adi_warnings)
    # JUCE contributes module translation units to this target. Keep the
    # project's strict warning policy on our code, not upstream module sources.
    set_source_files_properties(Core.cpp Dsp.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
endif()
if(MSVC)
    target_compile_options(adi_mix_clip PRIVATE /fp:precise /utf-8)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(adi_mix_clip PRIVATE /clang:-ffp-model=precise /clang:-ffp-contract=off)
    endif()
else()
    target_compile_options(adi_mix_clip PRIVATE -ffp-contract=off)
endif()
