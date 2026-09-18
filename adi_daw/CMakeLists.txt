# SPDX-License-Identifier: GPL-3.0-or-later
#
# adi_daw — step 4: the headless reference implementation.
#
# JUCE is deliberately NOT a dependency of this tree. The .adi reader/writer is
# a pure, portable data layer with no audio, no GUI and no realtime code, and
# keeping JUCE out is what enforces that (ADR-0010: the format is a persistence
# layer, not the runtime model). JUCE arrives in step 5, in a separate target
# that links adi_core — not the other way round.

# 3.25, not 3.21: the SYSTEM argument to add_subdirectory below was added in
# 3.25, so a 3.21 CMake would fail to configure this file with
# "add_subdirectory called with incorrect number of arguments". The declared
# minimum has to be one that can actually build the tree.
cmake_minimum_required(VERSION 3.25)
project(adi_daw LANGUAGES C CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies, from third_party/ (see docs/EXTERNAL-CODE.md).
# Both are MIT and GPLv3-compatible; neither is vendored into git.
# ---------------------------------------------------------------------------
set(ADI_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

foreach(dep SQLiteCpp json)
    if(NOT EXISTS "${ADI_THIRD_PARTY}/${dep}/CMakeLists.txt")
        message(FATAL_ERROR
            "third_party/${dep} is missing. These are gitignored clones — run:\n"
            "    bash adi_daw/tools/fetch_external.sh")
    endif()
endforeach()

set(SQLITECPP_RUN_CPPLINT      OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INTERNAL_SQLITE  ON  CACHE BOOL "" FORCE)  # bundled sqlite3.c
set(JSON_BuildTests            OFF CACHE BOOL "" FORCE)
set(JSON_Install               OFF CACHE BOOL "" FORCE)

add_subdirectory("${ADI_THIRD_PARTY}/SQLiteCpp" EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory("${ADI_THIRD_PARTY}/json"      EXCLUDE_FROM_ALL SYSTEM)

# ---------------------------------------------------------------------------
# Warnings. The whole value of choosing C++ here is that layout and lifetime
# mistakes become compile errors, so the warnings that catch them matter.
#
# These live on an INTERFACE target rather than in add_compile_options(), which
# is directory-scoped and was therefore also applying them to third_party/. On
# clang that produced 1306 warnings from sqlite3.c and SQLiteCpp -- an amalgam of
# noise deep enough to bury a real warning in our own code, which is the only
# thing this flag set exists to surface. (SYSTEM on add_subdirectory does not
# help: it marks the dependency's INCLUDE DIRECTORIES as system, which silences
# diagnostics from its headers when WE include them, not diagnostics raised
# while compiling its own .c and .cpp files.)
#
# Every target we own links this PRIVATE; nothing in third_party/ links it.
# ---------------------------------------------------------------------------
add_library(adi_warnings INTERFACE)

if(MSVC)
    target_compile_options(adi_warnings INTERFACE
        /W4 /permissive- /utf-8 /Zc:preprocessor
        # Without this MSVC reports __cplusplus as 199711L regardless of the
        # standard actually in use, which makes any version diagnostic a lie.
        /Zc:__cplusplus
        /we4477 /we4020 /we4013)   # format, arg count, implicit decl
else()
    target_compile_options(adi_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wcast-align -Wconversion -Wsign-conversion
        -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# adi_core — the format library. No JUCE, no audio, no threads.
# ---------------------------------------------------------------------------
add_library(adi_core STATIC
    src/adi/blob.cpp
)
target_include_directories(adi_core PUBLIC src)
target_link_libraries(adi_core PUBLIC SQLiteCpp nlohmann_json::nlohmann_json)
target_link_libraries(adi_core PRIVATE adi_warnings)

# ---------------------------------------------------------------------------
# adi_tool — headless CLI. The only executable until step 5.
# ---------------------------------------------------------------------------
add_executable(adi_tool src/main.cpp)
target_link_libraries(adi_tool PRIVATE adi_core adi_warnings)

# ---------------------------------------------------------------------------
# Tests. No framework yet on purpose — a dependency we would have to justify,
# for asserts we can write in plain C++. Revisit if this grows past one file.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(adi_tests tests/test_main.cpp)
target_link_libraries(adi_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_tests COMMAND adi_tests)
