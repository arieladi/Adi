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
# CI's strict job turns this on. It lives on adi_warnings rather than in
# CMAKE_CXX_FLAGS deliberately: a global -Werror would also apply to
# third_party/, and sqlite3.c is a 250k-line amalgamation that has never
# promised to be warning-free under our flag set. Our code is what we gate.
option(ADI_WERROR "Treat warnings in our own code as errors" OFF)

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

if(ADI_WERROR)
    target_compile_options(adi_warnings INTERFACE
        $<IF:$<BOOL:${MSVC}>,/WX,-Werror>)
endif()

# ---------------------------------------------------------------------------
# adi_core — the format library. No JUCE, no audio, no threads.
# ---------------------------------------------------------------------------
# The DDL is generated from docs/format/schema.sql rather than pasted, so the
# binary and tools/validate_schema.py exercise the same bytes. A hand-copied
# schema drifts in the worst direction: the validator keeps passing while the
# shipped binary creates a different database.
set(ADI_SCHEMA_SQL "${CMAKE_CURRENT_SOURCE_DIR}/docs/format/schema.sql")
set(ADI_SCHEMA_HPP "${CMAKE_CURRENT_BINARY_DIR}/generated/adi/schema_sql.hpp")

add_custom_command(
    OUTPUT  "${ADI_SCHEMA_HPP}"
    COMMAND "${CMAKE_COMMAND}"
            -DIN=${ADI_SCHEMA_SQL} -DOUT=${ADI_SCHEMA_HPP}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
    DEPENDS "${ADI_SCHEMA_SQL}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
    COMMENT "Embedding schema.sql"
    VERBATIM)

add_library(adi_core STATIC
    src/adi/blob.cpp
    src/adi/store.cpp
    src/adi/ops.cpp
    src/adi/ops_catalog.cpp
    src/adi/history.cpp
    src/adi/digest.cpp
    src/adi/check.cpp
    src/adi/textproj.cpp
    "${ADI_SCHEMA_HPP}"
)
target_include_directories(adi_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(adi_core PUBLIC
    ADI_VERSION_STRING="${PROJECT_VERSION}")
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

# A separate binary from adi_tests so a store failure is distinguishable from a
# blob failure at a glance in CI, without reading the log.
add_executable(adi_store_tests tests/test_store.cpp)
target_link_libraries(adi_store_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_store_tests COMMAND adi_store_tests)

# Separate again, for the same reason: an ops failure and a store failure should
# be distinguishable from the CI summary without opening the log.
add_executable(adi_ops_tests tests/test_ops.cpp)
target_link_libraries(adi_ops_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_ops_tests COMMAND adi_ops_tests)

add_executable(adi_history_tests tests/test_history.cpp)
target_link_libraries(adi_history_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_history_tests COMMAND adi_history_tests)

# The round-trip corpus (ADR-0021 §7.4). Its own binary because it is an
# INTEGRATION test -- store, ops and history together -- and a failure here
# means something different from a failure in any of the three unit suites.
add_executable(adi_replay_tests tests/test_replay.cpp)
target_link_libraries(adi_replay_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_replay_tests COMMAND adi_replay_tests)

add_executable(adi_catalog_tests tests/test_catalog.cpp)
target_link_libraries(adi_catalog_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_catalog_tests COMMAND adi_catalog_tests)

add_executable(adi_check_tests tests/test_check.cpp)
target_link_libraries(adi_check_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_check_tests COMMAND adi_check_tests)

# A second binary rather than a second file in adi_tests: the projection has no
# dependency on the blob layer and testing it in isolation keeps that true. If
# textproj ever needs blob.hpp to build its tests, that is worth noticing.
add_executable(adi_tests_textproj tests/test_textproj.cpp)
target_link_libraries(adi_tests_textproj PRIVATE adi_core adi_warnings)
add_test(NAME adi_tests_textproj COMMAND adi_tests_textproj)

# ---------------------------------------------------------------------------
# Fuzzing — optional, clang-only, OFF by default.
#
# StreamReader is the only thing in the tree that parses bytes it did not write,
# so it is the only thing that can be handed a hostile file. Four defects were
# found in it by hand (ADR-0023); this target exists to find the fifth.
#
# Off by default and gated on clang so that the MSVC build, and any default
# build anywhere, is completely unaffected by its existence.
#
#   cmake -S adi_daw -B build -DADI_BUILD_FUZZERS=ON
#   python3 adi_daw/tests/fuzz_seeds.py build/fuzz_corpus
#   ./build/adi_fuzz_blob build/fuzz_corpus -max_total_time=300
#
# Targets are discovered: every tests/fuzz_<x>.cpp becomes adi_fuzz_<x>, built
# against src/adi/<x>.cpp. `cmake --build ... --target adi_fuzzers` builds them
# all.
#
# blob.cpp is compiled INTO this target rather than linked from adi_core: a
# sanitised binary linking unsanitised objects still runs, but the uninstrumented
# half is invisible to the fuzzer, which is the half being tested. It also keeps
# SQLite out of a target that has no use for it.
# ---------------------------------------------------------------------------
option(ADI_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)

if(ADI_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "ADI_BUILD_FUZZERS needs clang; this compiler is "
            "${CMAKE_CXX_COMPILER_ID}. Configure a separate build directory "
            "with CXX=clang++, or leave the option off.")
    endif()

    # Apple's clang is a Clang, but Apple ship it without the libFuzzer runtime:
    # the failure is a link error naming libclang_rt.fuzzer_osx.a, an hour after
    # you would rather have been told. Say so at configure time instead.
    if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(FATAL_ERROR
            "Apple clang does not ship a libFuzzer runtime "
            "(libclang_rt.fuzzer_osx.a is absent from the Xcode toolchain).\n"
            "Use Homebrew LLVM, which does:\n"
            "    brew install llvm lld\n"
            "    cmake -S adi_daw -B build-fuzz -DADI_BUILD_FUZZERS=ON \\\n"
            "      -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \\\n"
            "      -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang\n"
            "Homebrew LLVM emits objects Apple's ld rejects "
            "('invalid r_symbolnum'), so lld is not optional there.")
    endif()

    set(ADI_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)

    # Discovered, not listed. A hardcoded target list is a list that stops
    # covering the tree the moment someone adds to it -- which is exactly what
    # happened to the -Werror gate when textproj.cpp arrived.
    #
    # Each tests/fuzz_<x>.cpp is built against src/adi/<x>.cpp. Compiling the
    # subject INTO the target rather than linking adi_core keeps every
    # instrumented byte instrumented: a sanitised binary linking unsanitised
    # objects still runs, but the uninstrumented half is invisible to the
    # fuzzer, and that half is the part being tested.
    file(GLOB ADI_FUZZ_HARNESSES CONFIGURE_DEPENDS "tests/fuzz_*.cpp")
    if(NOT ADI_FUZZ_HARNESSES)
        message(FATAL_ERROR "ADI_BUILD_FUZZERS is ON but tests/fuzz_*.cpp found nothing")
    endif()

    add_custom_target(adi_fuzzers)

    foreach(harness ${ADI_FUZZ_HARNESSES})
        get_filename_component(stem "${harness}" NAME_WE)          # fuzz_blob
        string(REGEX REPLACE "^fuzz_" "" subject "${stem}")         # blob
        set(subject_src "${CMAKE_CURRENT_SOURCE_DIR}/src/adi/${subject}.cpp")
        if(NOT EXISTS "${subject_src}")
            message(FATAL_ERROR
                "${harness} has no subject: expected src/adi/${subject}.cpp. "
                "A fuzz harness is named after what it fuzzes.")
        endif()

        add_executable(adi_${stem} "${harness}" "${subject_src}")
        target_include_directories(adi_${stem} PRIVATE src)
        target_link_libraries(adi_${stem} PRIVATE adi_warnings)
        target_compile_options(adi_${stem} PRIVATE ${ADI_FUZZ_FLAGS})
        target_link_options(adi_${stem} PRIVATE ${ADI_FUZZ_FLAGS})
        add_dependencies(adi_fuzzers adi_${stem})
        message(STATUS "fuzz target: adi_${stem} over src/adi/${subject}.cpp")
    endforeach()

    # Regenerated on every build: the seeds are the shapes of defects we have
    # already seen, and a stale corpus is a fuzzer told to stop looking there.
    add_custom_command(TARGET adi_fuzz_blob POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "generating fuzz seed corpus"
        COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz_seeds.py"
                "$<TARGET_FILE_DIR:adi_fuzz_blob>/fuzz_corpus/blob"
        VERBATIM)
endif()
