# SPDX-License-Identifier: GPL-3.0-or-later
#
# ChowCentaur as CLAP (ADR-0166). Upstream (BSD-3-Clause,
# github.com/jatinchowdhury18/KlonCentaur, last released 1.4.0 in 2021) ships
# VST3, AU, LV2 and a standalone on a JUCE 6.0.8 fork, and no CLAP. This wrapper
# includes upstream's tree UNCHANGED and adds a CLAP target with
# clap-juce-extensions, which supports JUCE 6.0.7 and later. The plug-in's own
# code is BSD; the binary links JUCE 6 under GPLv3, so what we build is GPLv3.
#
#   cmake -S plugins/external/chowcentaur -B <build> -DKLON_SOURCE=<checkout>

cmake_minimum_required(VERSION 3.21)
set(KLON_SOURCE "" CACHE PATH "The KlonCentaur checkout, submodules included (tools/fetch_plugins.sh)")
if (NOT EXISTS "${KLON_SOURCE}/modules/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "KLON_SOURCE is not a KlonCentaur checkout with submodules: '${KLON_SOURCE}'")
endif()
set(CMAKE_CXX_STANDARD 17)
project(adi_chowcentaur_clap LANGUAGES C CXX)

add_subdirectory(${KLON_SOURCE} kloncentaur)

# NOT the pin the other plug-ins use. From a40d845 (2023-04-18) the wrapper
# derives from juce::HostProvidedContextMenu whenever JUCE_VERSION >= 6.0.8,
# and upstream's fork calls itself 6.0.8 from a snapshot that predates that
# class. The commit before it is the newest that builds against this JUCE.
include(FetchContent)
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        25368807eee1b1635d8b505d35d0996c50a7139c   # 2023-04-13, MIT
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(clap-juce-extensions)

# Our id: upstream never released a CLAP of Centaur.
clap_juce_extensions_plugin(TARGET ChowCentaur
    CLAP_ID "com.adi.chowdsp.chowcentaur"
    CLAP_FEATURES audio-effect distortion)
