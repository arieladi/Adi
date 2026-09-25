# SPDX-License-Identifier: GPL-3.0-or-later
#
# ADI's own plug-ins (ADR-0166), built apart from the DAW: tools\build-plugins.bat.
#
# JUCE 9.0.2, our pinned copy, has no CLAP export (juce_add_plugin knows VST3,
# AU, AUv3, AAX, LV2, Standalone, Unity), so CLAP comes from clap-juce-extensions,
# as it does for Surge, ChowDSP and Airwindows Consolidated. Pinned by commit.

cmake_minimum_required(VERSION 3.22)
project(adi_plugins VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ADI_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

if (NOT EXISTS ${ADI_ROOT}/third_party/JUCE/CMakeLists.txt)
    message(FATAL_ERROR "JUCE is missing: bash tools/fetch_external.sh --with-juce")
endif()
add_subdirectory(${ADI_ROOT}/third_party/JUCE ${CMAKE_BINARY_DIR}/JUCE)

include(FetchContent)
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        55525c9858d4b25687be7759a5e0f70eccef218e   # 2026-09-12, MIT
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(clap-juce-extensions)

enable_testing()
add_subdirectory(rmsc)

# ADI Airwindows needs an airwin2rack checkout (tools/fetch_plugins.sh); without
# one it is skipped and the rest builds as before.
set(ADI_AIRWIN_SOURCE "" CACHE PATH "airwin2rack checkout for ADI Airwindows (tools/fetch_plugins.sh)")
if (EXISTS "${ADI_AIRWIN_SOURCE}/src/airwin_consolidated_base.h")
    add_subdirectory(airwindows)
else()
    message(STATUS "ADI Airwindows skipped: set ADI_AIRWIN_SOURCE to an airwin2rack checkout")
endif()
