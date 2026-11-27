# SPDX-License-Identifier: AGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(adi_warp_bench LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(QM_DSP_ROOT "" CACHE PATH "qm-dsp at the qm-vamp-plugins-v1.8.0 pin")
set(MIXXX_QM_ROOT "" CACHE PATH "lib/qm-dsp inside the pinned Mixxx checkout")
get_filename_component(ADI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${ADI_ROOT}/cmake/warp_tracker.cmake")
adi_add_qmdsp("${QM_DSP_ROOT}" "${MIXXX_QM_ROOT}")

add_library(adi_warp_candidate STATIC
    "${ADI_ROOT}/src/adi/audio/warp_beat_candidate.cpp"
    "${ADI_ROOT}/src/adi/audio/warp_analysis_pcm.cpp"
    "${ADI_ROOT}/src/adi/audio/warp_timing_map.cpp")
target_include_directories(adi_warp_candidate PUBLIC "${ADI_ROOT}/src")
target_link_libraries(adi_warp_candidate PRIVATE adi_qmdsp)

set(ADI_THIRD_PARTY "${ADI_ROOT}/third_party" CACHE PATH "ADI's pinned third-party checkouts")
set(BUILD_TESTING OFF CACHE BOOL "Keep dependency tests out of the standalone benchmark" FORCE)
add_subdirectory("${ADI_THIRD_PARTY}/libsamplerate" "${CMAKE_BINARY_DIR}/libsamplerate" EXCLUDE_FROM_ALL SYSTEM)
target_link_libraries(adi_warp_candidate PRIVATE samplerate)
add_library(adi_warp_pffft STATIC "${ADI_THIRD_PARTY}/pffft/pffft.c")
target_include_directories(adi_warp_pffft SYSTEM PUBLIC "${ADI_THIRD_PARTY}/pffft")

add_executable(adi_warp_bench probe.cpp
    "${ADI_ROOT}/src/adi/audio/wav_file.cpp"
    "${ADI_ROOT}/src/adi/audio/transients.cpp"
    "${ADI_ROOT}/src/adi/audio/auto_warp.cpp"
    "${ADI_ROOT}/src/adi/audio/spectral_onsets.cpp"
    "${ADI_ROOT}/src/adi/dsp/spectrum.cpp")
target_link_libraries(adi_warp_bench PRIVATE adi_warp_candidate adi_warp_pffft)

add_executable(adi_warp_candidate_tests candidate_test.cpp "${ADI_ROOT}/src/adi/audio/warp_map.cpp")
target_link_libraries(adi_warp_candidate_tests PRIVATE adi_warp_candidate)
foreach(target IN ITEMS adi_warp_candidate adi_warp_bench adi_warp_candidate_tests)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()

enable_testing()
add_test(NAME adi_warp_candidate_tests COMMAND adi_warp_candidate_tests)
