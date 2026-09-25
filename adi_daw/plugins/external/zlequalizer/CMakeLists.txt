# SPDX-License-Identifier: AGPL-3.0-or-later
#
# ZL Equalizer 2 as CLAP (ADR-0166): the baseline for our own equalizer.
# Upstream (AGPL-3.0, github.com/ZL-Audio/ZLEqualizer) ships VST3, AU, LV2 and
# a standalone, and no CLAP. This wrapper includes upstream's tree UNCHANGED and
# adds a CLAP target with clap-juce-extensions -- no patch to keep. Anything
# built from it is AGPLv3 (OPEN_SOURCE_POLICY.md §4).
#
#   cmake -S plugins/external/zlequalizer -B <build> -DZL_SOURCE=<checkout>
#
# The checkout needs its submodules (JUCE fork, nlopt, highway, zldsp_fft):
# tools/fetch_plugins.sh does that.

cmake_minimum_required(VERSION 3.24.1)
set(ZL_SOURCE "" CACHE PATH "The ZLEqualizer checkout, submodules included (tools/fetch_plugins.sh)")
if (NOT EXISTS "${ZL_SOURCE}/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "ZL_SOURCE is not a ZLEqualizer checkout with submodules: '${ZL_SOURCE}'")
endif()
project(adi_zlequalizer_clap LANGUAGES C CXX)

# Upstream reads these when they are defined. VST3 alone keeps the build short:
# CLAP is what the DAW loads, and one JUCE format has to exist for the wrapper to
# hang off. Nothing is copied into the system plug-in folders.
set(ZL_JUCE_FORMATS "VST3" CACHE STRING "JUCE formats beside the CLAP")
set(ZL_JUCE_COPY_PLUGIN FALSE CACHE BOOL "Copy into the system plug-in folders after building")
# SSE2 is x86-64's floor and what upstream ships for Windows; AVX2 is its second
# build. Upstream builds Windows with clang-cl. With MSVC the -march flags are
# ignored (D9002) and highway picks its target from MSVC's own /arch.
set(ZL_HWY_STATIC_TARGET "SSE2" CACHE STRING "highway static target: SSE2/SSE4/AVX2/NEON")

add_subdirectory(${ZL_SOURCE} zlequalizer)

include(FetchContent)
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        55525c9858d4b25687be7759a5e0f70eccef218e   # 2026-09-12, MIT
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(clap-juce-extensions)

# Our id, not ZL's: upstream ships no CLAP, and an id under its name would claim
# a release it never made.
clap_juce_extensions_plugin(TARGET ZLEqualizer
    CLAP_ID "com.adi.zlaudio.zlequalizer2"
    CLAP_FEATURES audio-effect equalizer stereo)
