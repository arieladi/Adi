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

# ADR-0127: pinned C dependencies. BLAKE3's SIMD paths: see below.
foreach(source BLAKE3/c/blake3.c miniz/miniz.c)
    if(NOT EXISTS "${ADI_THIRD_PARTY}/${source}")
        message(FATAL_ERROR "Missing ${source}; run tools/fetch_external.sh --build-only")
    endif()
endforeach()
add_library(adi_blake3 STATIC
    "${ADI_THIRD_PARTY}/BLAKE3/c/blake3.c"
    "${ADI_THIRD_PARTY}/BLAKE3/c/blake3_dispatch.c"
    "${ADI_THIRD_PARTY}/BLAKE3/c/blake3_portable.c")
# ADR-0147, ADR-0150, ADR-0153: every SIMD path the CPU has, chosen at run time
# by BLAKE3's own dispatcher (cpuid and xgetbv), so one binary runs everywhere
# and takes the fastest path the machine offers. Measured on a Ryzen 7 5700X3D,
# 1 GiB in memory: portable 650, SSE4.1 1,726, AVX2 3,381 MiB/s. Each ISA's flag
# is set on its own source file only, so no instruction leaks into code that
# runs before the dispatcher has checked. Universal Apple builds (one compile,
# two architectures) stay portable: a per-file ISA flag cannot apply to both.
set(_adi_b3 "${ADI_THIRD_PARTY}/BLAKE3/c")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86)$"
   AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
    target_sources(adi_blake3 PRIVATE "${_adi_b3}/blake3_sse2.c" "${_adi_b3}/blake3_sse41.c"
        "${_adi_b3}/blake3_avx2.c" "${_adi_b3}/blake3_avx512.c")
    target_compile_definitions(adi_blake3 PRIVATE BLAKE3_USE_NEON=0)
    if(MSVC)
        # SSE2 and SSE4.1 intrinsics need no flag on MSVC; AVX2 and AVX-512 do.
        set_source_files_properties("${_adi_b3}/blake3_avx2.c" PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        set_source_files_properties("${_adi_b3}/blake3_avx512.c" PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
    else()
        set_source_files_properties("${_adi_b3}/blake3_sse2.c" PROPERTIES COMPILE_OPTIONS "-msse2")
        set_source_files_properties("${_adi_b3}/blake3_sse41.c" PROPERTIES COMPILE_OPTIONS "-msse4.1")
        set_source_files_properties("${_adi_b3}/blake3_avx2.c" PROPERTIES COMPILE_OPTIONS "-mavx2")
        set_source_files_properties("${_adi_b3}/blake3_avx512.c" PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$"
       AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    # NEON is part of every 64-bit ARM, Apple Silicon included: no flag.
    target_sources(adi_blake3 PRIVATE "${_adi_b3}/blake3_neon.c")
    target_compile_definitions(adi_blake3 PRIVATE BLAKE3_USE_NEON=1
        BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
else()
    target_compile_definitions(adi_blake3 PRIVATE BLAKE3_USE_NEON=0
        BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif()
target_include_directories(adi_blake3 SYSTEM PUBLIC "${ADI_THIRD_PARTY}/BLAKE3/c")
# Select the upstream C sources without introducing its global build options.
add_library(adi_miniz STATIC
    "${ADI_THIRD_PARTY}/miniz/miniz.c" "${ADI_THIRD_PARTY}/miniz/miniz_zip.c"
    "${ADI_THIRD_PARTY}/miniz/miniz_tdef.c" "${ADI_THIRD_PARTY}/miniz/miniz_tinfl.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/miniz")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/miniz/miniz_export.h"
    "#pragma once\n#define MINIZ_EXPORT\n")
target_include_directories(adi_miniz SYSTEM PUBLIC "${ADI_THIRD_PARTY}/miniz"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/miniz")
# All disk I/O uses C++ path-aware streams and 64-bit offsets, including i386.
# Filesystem timestamps are not part of this media container contract. Avoid
# miniz's unused local-time conversion and its process-global CRT timezone state.
target_compile_definitions(adi_miniz PUBLIC MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

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
    src/adi/appdata.cpp
    src/adi/panel.cpp
    src/adi/plugin_registry.cpp
    src/adi/media/blake3.cpp
    src/adi/media/zip_writer.cpp
    src/adi/audio/wav_file.cpp
    src/adi/blob.cpp
    # Deliberately in adi_core rather than behind ADI_WITH_JUCE: device_core
    # includes no JUCE, so its tests run on all seven ABIs instead of only in
    # the one JUCE job. See src/juce/device_core.hpp.
    src/juce/device_core.cpp
    # Same rule, same payoff: the device CONTRACT has no JUCE in it, so the
    # missing-plugin placeholder and the node wrapper are tested on every ABI
    # rather than only where a plugin SDK exists. See src/juce/device_model.hpp.
    src/juce/device_model.cpp
    # CLAP hosting, and it needs NO JUCE -- clap is a header-only MIT C API
    # with no dependencies, so the host side lives in adi_core and its tests
    # run on all seven ABIs. VST3 hosting can only ever be exercised in the
    # one job that has a plugin SDK. See src/juce/clap_host.hpp (ADR-0075).
    src/juce/clap_host.cpp
    src/juce/device_host.cpp
    src/juce/pd_device.cpp
    src/adi/store.cpp
    src/adi/ops.cpp
    src/adi/ops_catalog.cpp
    src/adi/history.cpp
    src/adi/digest.cpp
    src/adi/check.cpp
    src/adi/engine/snapshot.cpp
    src/adi/engine/graph.cpp
    src/adi/engine/plan.cpp
    src/adi/dsp/biquad.cpp
    src/adi/dsp/limiter.cpp
    src/adi/dsp/rmsc.cpp
    src/adi/engine/latency.cpp
    src/adi/engine/host.cpp
    src/adi/engine/realize.cpp
    src/adi/engine/session.cpp
    src/adi/engine/mixer.cpp
    src/adi/engine/play_quantize.cpp
    src/adi/engine/param_ops.cpp
    src/adi/engine/param_edits.cpp
    src/adi/engine/mpe_input.cpp
    src/adi/engine/mpe_output.cpp
    src/adi/textproj.cpp
    src/adi/store_rows.cpp
    src/adi/textproj_store.cpp
    "${ADI_SCHEMA_HPP}"
)
target_include_directories(adi_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(adi_core PUBLIC
    ADI_VERSION_STRING="${PROJECT_VERSION}")
target_include_directories(adi_core PUBLIC src)

# Header-only, MIT, pinned by tag and commit (ADR-0024). SYSTEM so a C API's
# headers do not have to survive this tree's warning wall.
if(NOT EXISTS "${ADI_THIRD_PARTY}/clap/include/clap/clap.h")
    message(FATAL_ERROR
        "third_party/clap is absent. It is pinned and fetched by:\n"
        "    bash adi_daw/tools/fetch_external.sh --build-only")
endif()
target_include_directories(adi_core SYSTEM PUBLIC "${ADI_THIRD_PARTY}/clap/include")
target_link_libraries(adi_core PUBLIC SQLiteCpp nlohmann_json::nlohmann_json)
target_link_libraries(adi_core PRIVATE adi_warnings adi_blake3 adi_miniz)

# ---------------------------------------------------------------------------
# adi_tool — headless CLI. The only executable until step 5.
# ---------------------------------------------------------------------------
# The Propose-tier changeset (ADR-0145 d9, ADR-0148): its own library so the
# agent's staging layer is a separate unit from the core it drives.
add_library(adi_changeset STATIC src/adi/changeset.cpp)
target_link_libraries(adi_changeset PUBLIC adi_core PRIVATE adi_warnings)

add_executable(adi_tool src/main.cpp)
target_link_libraries(adi_tool PRIVATE adi_core adi_changeset adi_warnings)

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

# Remarks (ADR-0131): the table, its four ops, and what they must not read.
add_executable(adi_remarks_tests tests/test_remarks.cpp)
target_link_libraries(adi_remarks_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_remarks_tests COMMAND adi_remarks_tests)

# History snapshots (ADR-0128): revert never discards, across branches too.
add_executable(adi_snapshots_tests tests/test_snapshots.cpp)
target_link_libraries(adi_snapshots_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_snapshots_tests COMMAND adi_snapshots_tests)

# Upgrading an older 1.x file (ADR-0144): built from the frozen schema.sql of
# each minor, which is why the test is told where the history lives.
# The settings store (ADR-0152): the registry, the per-application file,
# presets, bundles and the agent's pipeline. No UI.
add_library(adi_settings STATIC
    src/adi/settings/registry.cpp
    src/adi/settings/store.cpp
    src/adi/settings/bundle.cpp
    src/adi/settings/catalogue.cpp)
target_link_libraries(adi_settings PUBLIC adi_core PRIVATE adi_miniz adi_warnings)

add_executable(adi_settings_tests tests/test_settings.cpp)
target_link_libraries(adi_settings_tests PRIVATE adi_settings adi_core adi_miniz adi_warnings)
target_compile_definitions(adi_settings_tests PRIVATE
    ADI_DOCS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs")
add_test(NAME adi_settings_tests COMMAND adi_settings_tests)

add_executable(adi_changeset_tests tests/test_changeset.cpp)
target_link_libraries(adi_changeset_tests PRIVATE adi_changeset adi_core adi_warnings)
add_test(NAME adi_changeset_tests COMMAND adi_changeset_tests)

add_executable(adi_migrate_tests tests/test_migrate.cpp)
target_link_libraries(adi_migrate_tests PRIVATE adi_core adi_warnings)
target_compile_definitions(adi_migrate_tests PRIVATE
    ADI_SCHEMA_HISTORY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs/format/history")
add_test(NAME adi_migrate_tests COMMAND adi_migrate_tests)

# The engine skeleton. Its own binary because its concurrency test is the only
# one in the tree that runs a second thread, and a failure there means something
# categorically different from a failure anywhere else.
# The audio-device core. No JUCE, so this runs everywhere; the JUCE half is
# thin enough that the JUCE job only has to prove a callback arrives.
#
# Its own binary because it replaces global operator new to count allocations --
# ADR-0010 says process() must not allocate, and that turns the claim into an
# observation. Linking that into another suite would count their allocations too.
add_executable(adi_device_tests tests/test_device.cpp)
target_link_libraries(adi_device_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_device_tests COMMAND adi_device_tests)

# The nine device ops. Its own binary because its findings are per-row: the
# round-trip corpus cannot see them, since undoing device.insert cascades
# plugin_params away before the comparison. See the file header.
# The device CONTRACT -- no JUCE, no VST3, so it runs on all seven ABIs. That
# it needs no plugin SDK to compile is part of what it asserts.
# ADR-0054's input parser. No JUCE, so it runs on all seven ABIs -- and the
# mandate's failure mode is that nothing fails, so the checks are about
# resolution surviving rather than values being roughly right.
# The CLAP host. No JUCE and no plugin SDK, so it runs on all seven ABIs --
# which is the headline difference from VST3 hosting (ADR-0075).
# A real .clap, loaded with no JUCE and no plugin SDK -- dlopen and one
# exported symbol. Not a test: it needs a plugin installed, and CI has none.
add_executable(adi_clap_probe src/juce/clap_probe.cpp)
target_link_libraries(adi_clap_probe PRIVATE adi_core adi_warnings)

add_executable(adi_dsp_tests tests/test_dsp.cpp)
target_link_libraries(adi_dsp_tests PRIVATE adi_core adi_warnings)
# The Pd patches are read from the source tree, not copied next to the binary.
target_compile_definitions(adi_dsp_tests PRIVATE ADI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME adi_dsp_tests COMMAND adi_dsp_tests)

add_executable(adi_pd_tests tests/test_pd.cpp)
target_link_libraries(adi_pd_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_pd_tests COMMAND adi_pd_tests)

add_executable(adi_device_host_tests tests/test_device_host.cpp)
target_link_libraries(adi_device_host_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_device_host_tests COMMAND adi_device_host_tests)

add_executable(adi_clap_tests tests/test_clap.cpp)
target_link_libraries(adi_clap_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_clap_tests COMMAND adi_clap_tests)

add_executable(adi_mpe_tests tests/test_mpe.cpp)
target_link_libraries(adi_mpe_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_mpe_tests COMMAND adi_mpe_tests)

add_executable(adi_mpe_output_tests tests/test_mpe_output.cpp)
target_link_libraries(adi_mpe_output_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_mpe_output_tests COMMAND adi_mpe_output_tests)

add_executable(adi_device_model_tests tests/test_device_model.cpp)
target_link_libraries(adi_device_model_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_device_model_tests COMMAND adi_device_model_tests)

add_executable(adi_device_ops_tests tests/test_device_ops.cpp)
target_link_libraries(adi_device_ops_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_device_ops_tests COMMAND adi_device_ops_tests)

add_executable(adi_plan_tests tests/test_plan.cpp)
target_link_libraries(adi_plan_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_plan_tests COMMAND adi_plan_tests)

add_executable(adi_host_tests tests/test_host.cpp)
target_link_libraries(adi_host_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_host_tests COMMAND adi_host_tests)

add_executable(adi_retap_tests tests/test_retap.cpp)
target_link_libraries(adi_retap_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_retap_tests COMMAND adi_retap_tests)

add_executable(adi_realize_tests tests/test_realize.cpp)
target_link_libraries(adi_realize_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_realize_tests COMMAND adi_realize_tests)

# ADR-0122: a real .adi, loaded, its devices resolved, played through the
# device contract at every block size. No JUCE, so it runs on all seven
# ABIs; the JUCE half (adi_play) only has to prove a device callback
# reaches it.
add_executable(adi_session_tests tests/test_session.cpp)
target_link_libraries(adi_session_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_session_tests COMMAND adi_session_tests)

# ADR-0163: the mixer strip in the graph.
add_executable(adi_mixer_tests tests/test_mixer.cpp)
target_link_libraries(adi_mixer_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_mixer_tests COMMAND adi_mixer_tests)

add_executable(adi_graph_tests tests/test_graph.cpp)
target_link_libraries(adi_graph_tests PRIVATE adi_core adi_warnings)
find_package(Threads REQUIRED)   # ADR-0123: one test renders on a second thread
target_link_libraries(adi_graph_tests PRIVATE Threads::Threads)
add_test(NAME adi_graph_tests COMMAND adi_graph_tests)

# ADR-0124: broadcasts in, ops out; the CLAP path renders on a second thread.
add_executable(adi_param_ops_tests tests/test_param_ops.cpp)
target_link_libraries(adi_param_ops_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_param_ops_tests COMMAND adi_param_ops_tests)

add_executable(adi_engine_tests tests/test_engine.cpp)
target_link_libraries(adi_engine_tests PRIVATE adi_core adi_warnings)
find_package(Threads REQUIRED)

# ADR-0154: the plug-in panel's rule and the Parameter List's search.
add_executable(adi_panel_tests tests/test_panel.cpp)
target_link_libraries(adi_panel_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_panel_tests COMMAND adi_panel_tests)

# ADR-0149: where application data lives, and the plugin capabilities registry.
add_executable(adi_plugin_registry_tests tests/test_plugin_registry.cpp)
target_link_libraries(adi_plugin_registry_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_plugin_registry_tests COMMAND adi_plugin_registry_tests)

add_executable(adi_param_edits_tests tests/test_param_edits.cpp)
target_link_libraries(adi_param_edits_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_param_edits_tests COMMAND adi_param_edits_tests)

target_link_libraries(adi_engine_tests PRIVATE Threads::Threads)
add_test(NAME adi_engine_tests COMMAND adi_engine_tests)

# A second binary rather than a second file in adi_tests: the projection has no
# dependency on the blob layer and testing it in isolation keeps that true. If
# textproj ever needs blob.hpp to build its tests, that is worth noticing.
add_executable(adi_tests_textproj tests/test_textproj.cpp)
target_link_libraries(adi_tests_textproj PRIVATE adi_core adi_warnings)
add_test(NAME adi_tests_textproj COMMAND adi_tests_textproj)

# The store adapter. A separate suite from adi_tests_textproj on purpose: that
# one proves the pure layer needs no database, and folding a suite that opens
# .adi files into it would quietly destroy the property it exists to show.
add_executable(adi_textproj_store_tests tests/test_textproj_store.cpp)
target_link_libraries(adi_textproj_store_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_textproj_store_tests COMMAND adi_textproj_store_tests)

# ADR-0102: headless synthetic callback timings; opt in for Release measurements.
# The proposal and measurement limits are recorded in collab/linux.md.
option(ADI_BUILD_BENCHMARKS "Build the headless block benchmark" OFF)
if(ADI_BUILD_BENCHMARKS)
    add_executable(adi_block_benchmark tools/benchmark_blocks.cpp)
    target_link_libraries(adi_block_benchmark PRIVATE adi_core adi_warnings)
endif()

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

# ---------------------------------------------------------------------------
# JUCE — optional, OFF by default. Step 6 infrastructure (ADR-0048).
#
# ADR-0036 built the engine headless on purpose, and those tests are how the
# snapshot handoff is proved. So JUCE must never become a hard dependency of
# adi_core: with ADI_WITH_JUCE=OFF the whole tree, all ten suites and every
# validator build and pass with no JUCE present at all. That is the property
# this option exists to protect, and CI keeps JUCE-off required on every ABI.
#
#   bash adi_daw/tools/fetch_external.sh --with-juce
#   cmake -S adi_daw -B build-juce -DADI_WITH_JUCE=ON
#   ./build-juce/adi_audio_probe --block 8192
# ---------------------------------------------------------------------------
option(ADI_WITH_JUCE "Build the JUCE-dependent targets (step 6)" OFF)

if(ADI_WITH_JUCE)
    set(ADI_JUCE_DIR "${ADI_THIRD_PARTY}/JUCE")
    if(NOT EXISTS "${ADI_JUCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "ADI_WITH_JUCE=ON but third_party/JUCE is absent. It is pinned and\n"
            "fetched separately because it is ~800MB unshallowed and no default\n"
            "build needs it:\n"
            "    bash adi_daw/tools/fetch_external.sh --with-juce")
    endif()

    add_subdirectory("${ADI_JUCE_DIR}" EXCLUDE_FROM_ALL SYSTEM)

    # --- adi_audio_probe ---------------------------------------------------
    # Opens an audio device at a requested block size, reports what it actually
    # got, and closes. No graph, no processing. It exists to prove the
    # dependency configures, links and RUNS on clang/arm64 and MSVC before step
    # 6 depends on it, and to answer ADR-0042's question empirically: what block
    # sizes will a real driver give us?
    juce_add_console_app(adi_audio_probe PRODUCT_NAME "adi_audio_probe")
    target_sources(adi_audio_probe PRIVATE src/juce/audio_probe.cpp)
    target_compile_features(adi_audio_probe PRIVATE cxx_std_20)

    target_compile_definitions(adi_audio_probe PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_DISPLAY_SPLASH_SCREEN=0   # we take the AGPL grant, not the free tier

        # ADR-0041: VST3 is the only host format, and every other one is set to
        # 0 EXPLICITLY rather than left to a default. A format enabled by a
        # default is one somebody has to keep compiling, and the ADR says not
        # behind a flag and not for testing.
        #
        # JUCE 9.0.2 also ships LV2 and LADSPA hosts, which ADR-0041 does not
        # name. They are off here for the same reason and the ADR should list
        # them.
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_ARA=0)

    # --- adi_vst3_probe ----------------------------------------------------
    # VST3 hosting, in the one job that has a plugin SDK. It asserts what can
    # be asserted with NO PLUGIN INSTALLED -- that the format manager carries
    # VST3 and nothing else (ADR-0041), that a description that resolves to
    # nothing yields a MissingDevice rather than a null (ADR-0011) -- and
    # scans and instantiates only when a plugin is actually there.
    juce_add_console_app(adi_vst3_probe PRODUCT_NAME "adi_vst3_probe")
    target_sources(adi_vst3_probe PRIVATE
        src/juce/vst3_probe.cpp
        src/juce/vst3_host.cpp
        src/juce/vst3_events.cpp)
    target_compile_features(adi_vst3_probe PRIVATE cxx_std_20)
    target_compile_definitions(adi_vst3_probe PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        # The probe pumps the message loop to observe a plugin reporting a
        # latency change, which JUCE delivers on that thread. Console app,
        # no UI, so a modal loop is the whole program rather than a hazard.
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_ARA=0)
    target_link_libraries(adi_vst3_probe PRIVATE
        adi_core
        juce::juce_audio_processors
        juce::juce_audio_utils
        adi_warnings)
    get_target_property(_adi_proc_incs juce::juce_audio_processors
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(_adi_proc_incs)
        target_include_directories(adi_vst3_probe SYSTEM PRIVATE ${_adi_proc_incs})
    endif()

    # --- adi_test_vst3: a VST3 whose edit controller IS reachable (ADR-0100) --
    # Every MPE synth on the development machine hides its controller, so the
    # host's capability probe and MpeMidi's IMidiMapping parameter path had
    # never run. This fixture is one class that is both component and
    # controller. Built only against Steinberg's MIT VST3 SDK -- the copy JUCE
    # carries -- and only on Windows for now: a macOS bundle needs an
    # Info.plist and bundle entry points nobody here can test.
    if(WIN32)
        set(_adi_vst3sdk
            "${ADI_THIRD_PARTY}/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK")
        # The SDK's own interface-id definitions, compiled WITHOUT our warning
        # wall: they are Steinberg's, and a warning in them is not ours to fix.
        add_library(adi_test_vst3_sdk OBJECT
            "${_adi_vst3sdk}/pluginterfaces/base/coreiids.cpp"
            "${_adi_vst3sdk}/pluginterfaces/base/funknown.cpp"
            "${_adi_vst3sdk}/public.sdk/source/vst/vstinitiids.cpp")
        target_include_directories(adi_test_vst3_sdk SYSTEM PRIVATE "${_adi_vst3sdk}")
        target_compile_features(adi_test_vst3_sdk PRIVATE cxx_std_20)

        add_library(adi_test_vst3 MODULE
            tests/fixtures/vst3_expression_synth.cpp
            $<TARGET_OBJECTS:adi_test_vst3_sdk>)
        target_include_directories(adi_test_vst3 SYSTEM PRIVATE "${_adi_vst3sdk}")
        target_compile_features(adi_test_vst3 PRIVATE cxx_std_20)
        target_link_libraries(adi_test_vst3 PRIVATE adi_warnings ole32)   # ole32: FUID's CoCreateGuid
        # The DLL is built apart from its bundle: a file and a directory cannot
        # both be called "ADI Expression Test.vst3" in one folder.
        set_target_properties(adi_test_vst3 PROPERTIES
            PREFIX "" SUFFIX ".vst3" OUTPUT_NAME "ADI Expression Test"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_dll")
        set(_adi_fixture "${CMAKE_CURRENT_BINARY_DIR}/fixtures/ADI Expression Test.vst3")
        add_custom_command(TARGET adi_test_vst3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_adi_fixture}/Contents/x86_64-win"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:adi_test_vst3>" "${_adi_fixture}/Contents/x86_64-win/")
        add_dependencies(adi_vst3_probe adi_test_vst3)
        target_compile_definitions(adi_vst3_probe PRIVATE ADI_TEST_VST3="${_adi_fixture}")
    endif()

    # adi_core carries device_core.cpp and, through its PUBLIC include dir, the
    # engine seam. Linking it rather than recompiling the source means the probe
    # exercises exactly the DeviceCore that ships and that the ABI suites test.
    # ADR-0122: a project through a real device -- the session's proof by ear.
    # Links both hosts: VST3 through JUCE, CLAP through ours. The fixture VST3
    # is reachable with --fixture on Windows, where it is built.
    juce_add_console_app(adi_play PRODUCT_NAME "adi_play")
    target_sources(adi_play PRIVATE
        src/juce/play.cpp
        src/juce/juce_device_loader.cpp
        src/juce/vst3_host.cpp
        src/juce/vst3_events.cpp)
    target_compile_features(adi_play PRIVATE cxx_std_20)
    target_compile_definitions(adi_play PRIVATE
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_AU=0
        JUCE_PLUGINHOST_LV2=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_ARA=0)
    target_link_libraries(adi_play PRIVATE
        adi_core
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_audio_devices
        adi_warnings)
    if(_adi_proc_incs)
        target_include_directories(adi_play SYSTEM PRIVATE ${_adi_proc_incs})
    endif()
    if(WIN32)
        add_dependencies(adi_play adi_test_vst3)
        target_compile_definitions(adi_play PRIVATE ADI_TEST_VST3="${_adi_fixture}")
    endif()

    target_link_libraries(adi_audio_probe PRIVATE
        adi_core
        juce::juce_audio_devices
        juce::juce_audio_utils
        adi_warnings)

    # JUCE's own headers do not survive this tree's warning wall, and they are
    # not ours to fix. SYSTEM on add_subdirectory covers its sources; this
    # covers its headers where we include them.
    get_target_property(_adi_juce_incs juce::juce_audio_devices
                        INTERFACE_INCLUDE_DIRECTORIES)
    if(_adi_juce_incs)
        target_include_directories(adi_audio_probe SYSTEM PRIVATE ${_adi_juce_incs})
    endif()

    # ADR-0137: ASIO on Windows. JUCE 9 bundles the three ASIO SDK headers
    # (modules/juce_audio_devices/native/asio), dual-licensed by Steinberg
    # since October 2025 as its ASIO licence OR GPL-3.0; we take GPL-3.0,
    # which OPEN_SOURCE_POLICY.md authorises. Nothing is fetched. The targets
    # that open devices get it; the audio probe fails on Windows without it.
    if(WIN32)
        target_compile_definitions(adi_audio_probe PRIVATE JUCE_ASIO=1)
        target_compile_definitions(adi_play PRIVATE JUCE_ASIO=1)
    endif()

    message(STATUS "adi: JUCE enabled (${ADI_JUCE_DIR})")
endif()

add_executable(adi_wav_file_tests tests/test_wav_file.cpp)
target_link_libraries(adi_wav_file_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_wav_file_tests COMMAND adi_wav_file_tests)

add_executable(adi_play_quantize_tests tests/test_play_quantize.cpp)
target_link_libraries(adi_play_quantize_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_play_quantize_tests COMMAND adi_play_quantize_tests)

add_executable(adi_media_hash_tests tests/test_media_hash.cpp)
target_link_libraries(adi_media_hash_tests PRIVATE adi_core adi_warnings Threads::Threads)
target_compile_definitions(adi_media_hash_tests PRIVATE
    ADI_BLAKE3_VECTORS="${ADI_THIRD_PARTY}/BLAKE3/test_vectors/test_vectors.json")
add_test(NAME adi_media_hash_tests COMMAND adi_media_hash_tests)

add_executable(adi_zip_writer_tests tests/test_zip_writer.cpp)
target_link_libraries(adi_zip_writer_tests PRIVATE adi_core adi_warnings adi_miniz Threads::Threads)
add_test(NAME adi_zip_writer_tests COMMAND adi_zip_writer_tests)

# linux/collect-export: only this assignment's sources and test targets.
target_sources(adi_core PRIVATE src/adi/media/media_ops.cpp src/adi/media/collect_export.cpp)
add_executable(adi_media_ops_tests tests/test_media_ops.cpp)
target_link_libraries(adi_media_ops_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_media_ops_tests COMMAND adi_media_ops_tests)
add_executable(adi_collect_export_tests tests/test_collect_export.cpp)
target_link_libraries(adi_collect_export_tests PRIVATE adi_core adi_warnings adi_miniz)
add_dependencies(adi_collect_export_tests adi_tool)
target_compile_definitions(adi_collect_export_tests PRIVATE ADI_TOOL_PATH="$<TARGET_FILE:adi_tool>")
add_test(NAME adi_collect_export_tests COMMAND adi_collect_export_tests)

# No-clobber publication platform boundary.
target_sources(adi_core PRIVATE src/adi/media/publish_file.cpp)

# ADR-0147: app-scoped library index, independent from the .adi schema.
if(NOT EXISTS "${ADI_THIRD_PARTY}/utf8proc/utf8proc.c")
    message(FATAL_ERROR "Missing utf8proc; run tools/fetch_external.sh --build-only")
endif()
add_library(adi_utf8proc STATIC "${ADI_THIRD_PARTY}/utf8proc/utf8proc.c")
target_include_directories(adi_utf8proc SYSTEM PUBLIC "${ADI_THIRD_PARTY}/utf8proc")
target_compile_definitions(adi_utf8proc PUBLIC UTF8PROC_STATIC)
target_sources(adi_core PRIVATE src/adi/library/index.cpp src/adi/library/volume.cpp src/adi/library/platform.cpp)
target_link_libraries(adi_core PRIVATE adi_utf8proc Threads::Threads)
add_executable(adi_library_tests tests/test_library.cpp)
target_link_libraries(adi_library_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_library_tests COMMAND adi_library_tests)

set_tests_properties(adi_library_tests PROPERTIES TIMEOUT 120)

# ADR-0151: worker-only clip conversion. Keep upstream tests/examples out of
# our suite count and leave the parent BUILD_TESTING value untouched.
function(adi_add_samplerate)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    # Its old cmake_minimum_required leaves CMP0077 unset. A fresh cache
    # otherwise lets include(CTest)'s option() erase our normal OFF variable.
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(BUILD_TESTING OFF)
    set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${ADI_THIRD_PARTY}/libsamplerate" "${CMAKE_BINARY_DIR}/libsamplerate" EXCLUDE_FROM_ALL)
endfunction()
adi_add_samplerate()
target_sources(adi_core PRIVATE src/adi/engine/transport.cpp src/adi/engine/clip_playback.cpp)
target_link_libraries(adi_core PRIVATE samplerate)
add_executable(adi_clip_playback_tests tests/test_clip_playback.cpp)
target_link_libraries(adi_clip_playback_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_clip_playback_tests COMMAND adi_clip_playback_tests)
target_compile_definitions(adi_clip_playback_tests PRIVATE
    ADI_DECODE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/decode")
set_tests_properties(adi_clip_playback_tests PROPERTIES TIMEOUT 600)

# ADR-0156: one decoder for imported audio, into the decoding cache. The
# libraries' implementations compile once, as C, outside the warning wall;
# adi_core's clip worker asks the decoder for a playable file.
foreach(source dr_libs/dr_flac.h dr_libs/dr_mp3.h dr_libs/dr_wav.h miniaudio/extras/stb_vorbis.c)
    if(NOT EXISTS "${ADI_THIRD_PARTY}/${source}")
        message(FATAL_ERROR "Missing ${source}; run tools/fetch_external.sh --build-only")
    endif()
endforeach()
add_library(adi_decoders STATIC src/adi/audio/decode_libs.c)
target_include_directories(adi_decoders SYSTEM PUBLIC
    "${ADI_THIRD_PARTY}/dr_libs" "${ADI_THIRD_PARTY}/miniaudio/extras")
target_sources(adi_core PRIVATE src/adi/audio/decode.cpp)
target_link_libraries(adi_core PRIVATE adi_decoders)
add_executable(adi_decode_tests tests/test_decode.cpp)
target_link_libraries(adi_decode_tests PRIVATE adi_core adi_warnings)
target_compile_definitions(adi_decode_tests PRIVATE
    ADI_DECODE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/decode")
add_test(NAME adi_decode_tests COMMAND adi_decode_tests)
set_tests_properties(adi_decode_tests PROPERTIES TIMEOUT 120)

# ADR-0155: immutable MIDI clip schedules, emitted before graph forwarding.
target_sources(adi_core PRIVATE src/adi/engine/midi_clips.cpp)
add_executable(adi_midi_clips_tests tests/test_midi_clips.cpp)
target_link_libraries(adi_midi_clips_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_midi_clips_tests COMMAND adi_midi_clips_tests)

# ADR-0159: the curve formulas of SPEC §6.3.2, one evaluator for every reader.
target_sources(adi_core PRIVATE src/adi/engine/curves.cpp)
add_executable(adi_curves_tests tests/test_curves.cpp)
target_link_libraries(adi_curves_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_curves_tests COMMAND adi_curves_tests)
# ADR-0159 part two: automation read into the engine, not yet played.
target_sources(adi_core PRIVATE src/adi/engine/automation.cpp)
add_executable(adi_automation_tests tests/test_automation.cpp)
target_link_libraries(adi_automation_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_automation_tests COMMAND adi_automation_tests)
