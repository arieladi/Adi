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
foreach(source BLAKE3/c/blake3.c miniz/miniz.c pffft/pffft.c)
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

# PFFFT, the FFT behind dsp::Spectrum (ADR-0024, ADR-0195 d5). Pinned in its own
# right at the commit bungee carries as a submodule, because when the warp
# engine lands ADI links this FFT anyway and a second one would be two FFTs in
# one binary.
#
# SIMD is left as PFFFT finds it -- it detects SSE, NEON and Altivec itself and
# falls back to scalar, and the scalar path gives the same numbers, just slower.
# The one thing it needs from us is 16-byte alignment on its buffers, which
# dsp::Spectrum gets from pffft_aligned_malloc rather than from a vector.
#
# Its warnings are upstream's, not this tree's, so it is added without
# adi_warnings -- the same treatment airwin2rack and libpd get.
add_library(adi_pffft STATIC "${ADI_THIRD_PARTY}/pffft/pffft.c")
target_include_directories(adi_pffft SYSTEM PUBLIC "${ADI_THIRD_PARTY}/pffft")
if(MSVC)
    target_compile_options(adi_pffft PRIVATE /wd4244 /wd4305 /wd4996)
else()
    target_compile_options(adi_pffft PRIVATE -Wno-unused-function -Wno-unused-parameter)
endif()

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
    # The table of compiled-in Pd externals (ADR-0188 d8). No libpd in the
    # registry itself, so it builds on every ABI; only the setup functions it
    # calls touch Pd.
    src/juce/pd_builtins.cpp
    # The sample slot (ADR-0192 d4). No libpd in it either -- the handoff is
    # SnapshotPublisher and the decode is the audio layer, so it builds on
    # every ABI.
    src/juce/pd_samples.cpp
    # The static declaration parse (ADR-0177 fix 3, ADR-0183): no libpd in it
    # at all, deliberately -- the declared set must never depend on Pd running,
    # which is also why it is built on every ABI including the ones where the
    # runtime below is not.
    src/juce/pd_declarations.cpp
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
    src/adi/dsp/masking.cpp
    src/adi/dsp/spectrum.cpp
    src/adi/dsp/auto_gain.cpp
    src/adi/dsp/true_peak.cpp
    src/adi/engine/latency.cpp
    src/adi/engine/host.cpp
    src/adi/engine/realize.cpp
    src/adi/engine/session.cpp
    src/adi/engine/mixer.cpp
    src/adi/engine/summing.cpp
    src/adi/engine/scope.cpp
    src/adi/summing_flavors.cpp
    src/adi/engine/param_automation.cpp
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
# PUBLIC: dsp/spectrum.cpp includes pffft.h, and nothing else in the tree does.
target_link_libraries(adi_core PRIVATE adi_pffft)

# ---------------------------------------------------------------------------
# libpd -- the Pure Data runtime (ADR-0035, ADR-0183).
#
# PD_MULTI IS NOT OPTIONAL HERE, and it is the one setting worth explaining.
# Plain libpd is a SINGLE Pd instance per process: every open patch shares one
# DSP graph, one sample rate, one block size, and one `libpd_process_float`
# call that runs all of them together. A DAW needs the opposite -- a device in
# track 1's chain processed when the graph reaches track 1, at that node's block
# size, with its own latency (ADR-0095). PD_MULTI compiles pure-data with
# PDINSTANCE, giving one `t_pdinstance` per device and `libpd_set_instance` to
# choose it, which is what makes a Pd patch a device rather than a session-wide
# effect.
#
# It lives in adi_core, not behind ADI_WITH_JUCE, for the reason stated above
# device_core and clap_host: libpd is a C library with no GUI and no plugin SDK,
# so it compiles on every ABI, and the Pd tier's tests run on all seven rather
# than in the one JUCE job.
#
# Shared is off: we link one static archive and ship no libpd.dylib for anyone
# to mistake for a plug-in. The C examples follow the shared build and go too.
#
# PD_LOCALE is left ON, as upstream has it. Pd parses "1.5" out of patch files
# with the C library's float parser, so a host process running under a
# comma-decimal locale would read every float in every patch wrong. It sets
# LC_NUMERIC process-wide, which is intrusive for a host -- but a DAW that
# silently misreads its own device patches on a French machine is worse than one
# that formats a number differently, and every DAW that embeds Pd takes this.
#
# ADI_WITH_PD IS ON EVERYWHERE THE DEPENDENCY CAN BE BUILT, and OFF on MSVC
# unless someone provides pthreads -- which is upstream's requirement, not a
# choice made here. libpd's own CMakeLists fails hard under MSVC without
# PTHREADS_LIB and PTHREADS_INCLUDE_DIR ("Please provide a path to the pthreads
# library and its headers"), because Pd threads with pthreads and MSVC ships no
# implementation. GCC and clang on Windows would use winpthreads; MSVC has
# nothing.
#
# Closing that means ADDING A SHIPPED DEPENDENCY -- pthread-win32, or vcpkg's
# copy of it -- and EXTERNAL-CODE.md's rows say who decides that: several are
# marked "director-granted". So the Pd tier is gated rather than that decision
# being taken here, and ADR-0183 d24 states the two options and their costs.
#
# WHAT IS NOT GATED: the declaration parse. ADR-0177 fix 3 puts the declared set
# behind a static parse with Pd not running, so it has no libpd in it, and it
# and its tests build on every ABI including this one. The half of ADR-0188 d8
# that refuses [declare -lib] from the patch text is therefore still tested on
# Windows; the half that scans directories is not.
option(ADI_WITH_PD "Build the Pure Data device tier (needs libpd)" ON)
if(ADI_WITH_PD AND MSVC AND (NOT PTHREADS_LIB OR NOT PTHREADS_INCLUDE_DIR))
    # pthreads4w, granted by the director for exactly this (ADR-0192 d5).
    #
    # THE WHOLE LIBRARY IS ONE TRANSLATION UNIT. `pthread.c` #includes 145 of
    # the other 146 sources -- everything except `signal.c`, which upstream
    # leaves out on purpose. So this target is one source file, and adding a
    # second would define every symbol twice.
    #
    # `dll.c` IS among them and must stay, because a STATIC pthreads4w has no
    # DllMain to initialise it. Instead `dll.c` puts `on_process_init` in
    # `.CRT$XCU` so the C runtime calls it before main, and `implement.h`
    # references `__ptw32_autostatic_anchor()` so the linker cannot drop the
    # module that holds it. Compiling `pthread.c` alone carries both.
    #
    # `__PTW32_STATIC_LIB` is PUBLIC because it changes `__PTW32_DLLPORT` in
    # `_ptw32.h`, which every consumer's `pthread.h` reads: a library built
    # static and a consumer expecting a DLL disagree about `dllimport` and the
    # link fails. `HAVE_CONFIG_H` is PRIVATE -- `config.h` defines
    # `__PTW32_BUILD`, which is true of the library and false of its users.
    # The cleanup style is left alone: `pthread.h` defaults to
    # `__PTW32_CLEANUP_C`, which is the flavour we want, and a default both
    # sides read beats a definition one side might miss.
    if(EXISTS "${ADI_THIRD_PARTY}/pthreads4w/pthread.c")
        add_library(adi_pthreads4w STATIC "${ADI_THIRD_PARTY}/pthreads4w/pthread.c")
        target_compile_definitions(adi_pthreads4w
            PUBLIC  __PTW32_STATIC_LIB
            PRIVATE HAVE_CONFIG_H _CRT_SECURE_NO_WARNINGS)
        target_include_directories(adi_pthreads4w SYSTEM PUBLIC
            "${ADI_THIRD_PARTY}/pthreads4w")
        # Upstream's own warnings, not this tree's: 2017 C against MSVC 19.4x.
        target_compile_options(adi_pthreads4w PRIVATE
            /wd4100 /wd4127 /wd4189 /wd4244 /wd4267 /wd4996)
        # libpd tests these two and calls FATAL_ERROR if either is empty, then
        # uses PTHREADS_LIB in target_link_libraries -- where a TARGET name is
        # better than a path, because it carries the include directory and the
        # PUBLIC definition above with it.
        set(PTHREADS_LIB adi_pthreads4w CACHE STRING "" FORCE)
        set(PTHREADS_INCLUDE_DIR "${ADI_THIRD_PARTY}/pthreads4w" CACHE PATH "" FORCE)
        message(STATUS "ADI_WITH_PD on for MSVC: pthreads4w from third_party (ADR-0192 d5)")
    else()
        message(STATUS
            "ADI_WITH_PD off: libpd needs pthreads under MSVC, and "
            "third_party/pthreads4w is absent. It is pinned and fetched by:\n"
            "    bash adi_daw/tools/fetch_external.sh --build-only")
        set(ADI_WITH_PD OFF)
    endif()
endif()

if(ADI_WITH_PD)
if(NOT EXISTS "${ADI_THIRD_PARTY}/libpd/libpd_wrapper/z_libpd.h")
    message(FATAL_ERROR
        "third_party/libpd is absent. It is pinned and fetched by:\n"
        "    bash adi_daw/tools/fetch_external.sh --build-only")
endif()
if(NOT EXISTS "${ADI_THIRD_PARTY}/libpd/pure-data/src/m_pd.h")
    message(FATAL_ERROR
        "third_party/libpd is present but its pure-data submodule is not.\n"
        "pure-data IS the DSP; libpd without it is a wrapper around nothing.\n"
        "    bash adi_daw/tools/fetch_external.sh --build-only")
endif()
# LIBPD'S HEADERS DECLARE EVERY SYMBOL `__declspec(dllimport)` ON WINDOWS
# UNLESS TOLD OTHERWISE, and we link the static library. `m_pd.h` defines
# `EXTERN` as `dllexport` for libpd's own sources (`PD_INTERNAL`) and
# `dllimport` for everyone else, so `adi_core` compiled references to
# `__imp_libpd_init` and the rest, and the link failed with two dozen LNK2019s
# naming symbols that were sitting in the archive all along. Measured, in CI:
# the compile was clean and only the link failed, which is what a dllimport
# mismatch looks like.
#
# `PD_DEFINE_EXTERN` is libpd's own knob for exactly this ("a custom string for
# special linking purposes"). Setting it to `extern` makes `EXTERN` plain on
# both sides -- `m_pd.h` guards it with `#ifndef EXTERN`, so this wins -- and
# libpd's own sources stop emitting `dllexport` too, which is right for an
# archive nobody imports from.
#
# It is set unconditionally rather than under `if(MSVC)`: on ELF and Mach-O
# `EXTERN` is already plain `extern`, so this changes nothing there, and one
# code path is worth more than a conditional that only one platform exercises.
# A NORMAL variable, not a cache entry: libpd declares it with `option()`, and
# under CMP0077 a normal variable of the same name makes `option()` a no-op.
set(PD_DEFINE_EXTERN      extern)
set(PD_MULTI              ON  CACHE BOOL "" FORCE)
set(PD_UTILS              ON  CACHE BOOL "" FORCE)
set(PD_EXTRA              ON  CACHE BOOL "" FORCE)
set(LIBPD_STATIC          ON  CACHE BOOL "" FORCE)
set(LIBPD_SHARED          OFF CACHE BOOL "" FORCE)
set(PD_BUILD_C_EXAMPLES   OFF CACHE BOOL "" FORCE)
add_subdirectory("${ADI_THIRD_PARTY}/libpd" EXCLUDE_FROM_ALL SYSTEM)
target_include_directories(adi_core SYSTEM PUBLIC
    "${ADI_THIRD_PARTY}/libpd/libpd_wrapper"
    "${ADI_THIRD_PARTY}/libpd/libpd_wrapper/util"
    "${ADI_THIRD_PARTY}/libpd/pure-data/src")
target_link_libraries(adi_core PUBLIC libpd_static)
# LIBPD ONLY LINKS ITS SYSTEM LIBRARIES INTO THE SHARED TARGET, and we build the
# static one. Read, not discovered in a red build: libpd's CMakeLists gives
# `Ws2_32` and `${PTHREADS_LIB}` to `libpd` (SHARED) and gives `libpd_static`
# nothing but its two INTERFACE settings targets, which carry compile
# definitions and include directories only. So on Windows the pthreads and
# winsock symbols Pd uses -- `s_inter.c` opens sockets -- are this target's to
# supply, or `adi_core` fails to link with nothing on any path saying why.
if(WIN32)
    target_link_libraries(adi_core PUBLIC Ws2_32)
    if(MSVC AND TARGET adi_pthreads4w)
        target_link_libraries(adi_core PUBLIC adi_pthreads4w)
    endif()
endif()
# PDINSTANCE changes struct layouts in m_pd.h, so every translation unit that
# includes it must agree. It is PUBLIC on the libpd target, but stated here too
# because a mismatch is not a link error -- it is a silent layout disagreement.
target_compile_definitions(adi_core PUBLIC PDINSTANCE=1 PDTHREADS=1)
# The libpd side of PdPatchEngine (ADR-0183), added to adi_core here rather than
# in its source list, because it is the one Pd source that includes z_libpd.h.
target_sources(adi_core PRIVATE src/juce/pd_engine.cpp)
target_compile_definitions(adi_core PUBLIC ADI_WITH_PD=1)

# ADI'S COMPILED-IN Pd EXTERNALS (ADR-0188 d8, ADR-0192 d1). One source each,
# listed here; each declares itself with ADI_PD_BUILTIN beside its own setup
# function, so `src/juce/pd_builtins.cpp` is never edited to admit one -- the
# externals are win_codex's and `src/juce/**` is not (ADR-0192 d6).
#
# AN OBJECT LIBRARY, not sources added to adi_core, and that is the whole point.
# A self-registering translation unit inside a STATIC library is dropped by the
# linker when nothing references it, which would remove the external and print
# nothing. An OBJECT library's objects are always linked.
#
# Empty until the first external lands; the guard keeps CMake happy until then,
# because an OBJECT library with no sources is an error.
set(ADI_PD_BUILTIN_SOURCES
    # src/adi/dsp/adi_combchord_tilde.cpp
    # src/adi/dsp/adi_colorcab_tilde.cpp
)
if(ADI_PD_BUILTIN_SOURCES)
    add_library(adi_pd_builtins OBJECT ${ADI_PD_BUILTIN_SOURCES})
    target_link_libraries(adi_pd_builtins PRIVATE libpd_static adi_warnings)
    target_include_directories(adi_pd_builtins PRIVATE src)
    target_link_libraries(adi_core PUBLIC adi_pd_builtins)
endif()
endif()

# ADR-0173, ADR-0174: the 16 Airwindows console algorithms behind native group
# summing, from airwin2rack (MIT), pinned by commit. Built as upstream builds
# them: their warnings are not this tree's, so they get none of its flags.
set(ADI_AIRWIN "${ADI_THIRD_PARTY}/airwin2rack/src")
if(NOT EXISTS "${ADI_AIRWIN}/airwin_consolidated_base.h")
    message(FATAL_ERROR
        "third_party/airwin2rack is absent. It is pinned and fetched by:\n"
        "    bash adi_daw/tools/fetch_external.sh --build-only")
endif()
set(ADI_CONSOLE_ALGORITHMS
    Console9Channel Console9Buss ConsoleLAChannel ConsoleLABuss ConsoleMCChannel ConsoleMCBuss
    ConsoleMDChannel ConsoleMDBuss PurestConsole3Channel PurestConsole3Buss PDChannel PDBuss
    C5RawChannel C5RawBuss AtmosphereChannel AtmosphereBuss)
set(adi_console_sources "${ADI_AIRWIN}/airwin_consolidated_base.cpp")
foreach(algo IN LISTS ADI_CONSOLE_ALGORITHMS)
    list(APPEND adi_console_sources "${ADI_AIRWIN}/autogen_airwin/${algo}.cpp"
                                    "${ADI_AIRWIN}/autogen_airwin/${algo}Proc.cpp")
endforeach()
add_library(adi_airwin_console STATIC ${adi_console_sources})
target_include_directories(adi_airwin_console SYSTEM PUBLIC "${ADI_AIRWIN}")
target_compile_definitions(adi_airwin_console PUBLIC _USE_MATH_DEFINES)
if(MSVC)
    target_compile_options(adi_airwin_console PRIVATE /w /bigobj)
else()
    target_compile_options(adi_airwin_console PRIVATE -w)
endif()
target_link_libraries(adi_core PRIVATE adi_airwin_console)

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

# The declaration parse. Links adi_core but touches no libpd: ADR-0177 fix 3
# puts the declared set behind a static parse of the patch text, and a test that
# needed Pd running to read a declaration would not be testing that.

# [adi.sample] and its handoff. Links adi_core and touches no libpd, for the
# reason the declaration parse does not: the slot is SnapshotPublisher and the
# decode is the audio layer, so both are tested on every ABI -- including the
# ones where the Pd runtime is not built.
add_executable(adi_pd_sample_tests tests/test_pd_samples.cpp)
target_link_libraries(adi_pd_sample_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_pd_sample_tests COMMAND adi_pd_sample_tests)

# dsp::Spectrum (ADR-0183 d7, ADR-0195 d5). The wrapper is what is tested, not
# PFFFT: its real packing puts DC and Nyquist in the first two slots, and the
# reference is a direct DFT written from the definition.
add_executable(adi_spectrum_tests tests/test_spectrum.cpp)
target_link_libraries(adi_spectrum_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_spectrum_tests COMMAND adi_spectrum_tests)

# The masking measure (ADR-0195 d5). Pure: no engine, no view, no agent -- the
# analyser overlay and analyze.masking are both callers, and this is where they
# are made to agree.
add_executable(adi_masking_tests tests/test_masking.cpp)
target_link_libraries(adi_masking_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_masking_tests COMMAND adi_masking_tests)

add_executable(adi_pd_declaration_tests tests/test_pd_declarations.cpp)
target_link_libraries(adi_pd_declaration_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_pd_declaration_tests COMMAND adi_pd_declaration_tests)

# libpd actually running, as against test_pd.cpp's contract-with-a-fake. The
# patch directory is compiled in rather than found from the working directory,
# which differs between a local run and CI.
if(ADI_WITH_PD)
    add_executable(adi_pd_engine_tests tests/test_pd_engine.cpp)
    target_link_libraries(adi_pd_engine_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_pd_engine_tests PRIVATE
        ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd"
        ADI_PD_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/pd")
    add_test(NAME adi_pd_engine_tests COMMAND adi_pd_engine_tests)
endif()

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

add_executable(adi_transport_info_tests tests/test_transport_info.cpp)
target_link_libraries(adi_transport_info_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_transport_info_tests COMMAND adi_transport_info_tests)

add_executable(adi_analyser_taps_tests tests/test_analyser_taps.cpp)
target_link_libraries(adi_analyser_taps_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_analyser_taps_tests COMMAND adi_analyser_taps_tests)

# ADR-0163: the mixer strip in the graph.
add_executable(adi_mixer_tests tests/test_mixer.cpp)
target_link_libraries(adi_mixer_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_mixer_tests COMMAND adi_mixer_tests)

# ADR-0165: plug-in parameter automation, the engine side.
add_executable(adi_param_automation_tests tests/test_param_automation.cpp)
target_link_libraries(adi_param_automation_tests PRIVATE adi_core adi_warnings Threads::Threads)
add_test(NAME adi_param_automation_tests COMMAND adi_param_automation_tests)

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
    # controller. Built against Steinberg's MIT VST3 SDK -- the copy JUCE
    # carries -- and nothing else.
    #
    # WINDOWS AND macOS. This said "only on Windows for now: a macOS bundle
    # needs an Info.plist and bundle entry points nobody here can test", which
    # was true when it was written and stopped being true when mac came back
    # to a Mac. The Info.plist is tests/fixtures/Info.plist.in, the entry
    # points are `bundleEntry`/`bundleExit` at the bottom of the fixture, and
    # the test is `adi_play --list` finding all three variants in the built
    # bundle. Without this, the macOS half of the render CI job could only
    # ever have covered CLAP.
    if(WIN32 OR APPLE)
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
        # ole32 is for FUID's CoCreateGuid on Windows; the same code path uses
        # CFUUIDCreate on macOS, which is CoreFoundation.
        if(WIN32)
            target_link_libraries(adi_test_vst3 PRIVATE adi_warnings ole32)
        else()
            target_link_libraries(adi_test_vst3 PRIVATE adi_warnings
                                  "-framework CoreFoundation")
        endif()
        set(ADI_FIXTURE_NAME "ADI Expression Test")
        set(_adi_fixture
            "${CMAKE_CURRENT_BINARY_DIR}/fixtures/${ADI_FIXTURE_NAME}.vst3")

        # The binary is built apart from its bundle: a file and a directory
        # cannot both be called "ADI Expression Test.vst3" in one folder.
        #
        # The two platforms put the binary in different places inside the
        # bundle and only one of them wants the .vst3 suffix ON THE BINARY --
        # on macOS the suffix belongs to the bundle directory and the
        # executable inside it is named by CFBundleExecutable, with no
        # extension at all.
        if(WIN32)
            set_target_properties(adi_test_vst3 PROPERTIES
                PREFIX "" SUFFIX ".vst3" OUTPUT_NAME "${ADI_FIXTURE_NAME}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_dll")
            add_custom_command(TARGET adi_test_vst3 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${_adi_fixture}/Contents/x86_64-win"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:adi_test_vst3>"
                        "${_adi_fixture}/Contents/x86_64-win/")
        else()
            configure_file(tests/fixtures/Info.plist.in
                           "${CMAKE_CURRENT_BINARY_DIR}/fixtures/Info.plist" @ONLY)
            set_target_properties(adi_test_vst3 PROPERTIES
                PREFIX "" SUFFIX "" OUTPUT_NAME "${ADI_FIXTURE_NAME}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixture_dll")
            # STRIP THE EXTENDED ATTRIBUTES, THEN AD-HOC SIGN. Both are
            # required on macOS and neither is optional folklore.
            #
            # A bundle built inside an iCloud-synced folder (this repository
            # lives under ~/Documents) inherits `com.apple.provenance` and
            # `com.apple.fileprovider.*`, and Gatekeeper then refuses to load
            # it with "is damaged and can't be opened. You should move it to
            # the Trash" -- one modal dialog PER SCAN ATTEMPT. The dylib is
            # fine; macOS simply will not open an unsigned bundle carrying
            # provenance it cannot account for.
            #
            # `codesign --sign -` is an ad-hoc signature: no certificate, no
            # identity, nothing to configure, and enough for a locally built
            # test fixture to load. It is NOT a substitute for real signing
            # when something ships.
            #
            # `|| true` on both: a machine without `xattr` or `codesign`
            # still gets a working build, and the failure mode there is the
            # one we already understand rather than a build that stops.
            add_custom_command(TARGET adi_test_vst3 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "${_adi_fixture}/Contents/MacOS"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_BINARY_DIR}/fixtures/Info.plist"
                        "${_adi_fixture}/Contents/Info.plist"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:adi_test_vst3>"
                        "${_adi_fixture}/Contents/MacOS/"
                COMMAND xattr -cr "${_adi_fixture}" || true
                COMMAND codesign --force --deep --sign - "${_adi_fixture}" || true)
        endif()
        add_dependencies(adi_vst3_probe adi_test_vst3)
        target_compile_definitions(adi_vst3_probe PRIVATE ADI_TEST_VST3="${_adi_fixture}")
    endif()

    # adi_core carries device_core.cpp and, through its PUBLIC include dir, the
    # engine seam. Linking it rather than recompiling the source means the probe
    # exercises exactly the DeviceCore that ships and that the ABI suites test.
    # ADR-0122: a project through a real device -- the session's proof by ear.
    # Links both hosts: VST3 through JUCE, CLAP through ours. The fixture VST3
    # is reachable with --fixture on Windows and macOS, the two platforms it
    # is built on.
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
    if(TARGET adi_test_vst3)
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
