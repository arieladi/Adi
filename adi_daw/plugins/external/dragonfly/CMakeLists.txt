# SPDX-License-Identifier: GPL-3.0-or-later
#
# Dragonfly Reverb (Hall, Room, Plate, Early Reflections) as CLAP (ADR-0166).
# Upstream (GPL-3.0, github.com/michaelwillis/dragonfly-reverb) already ships
# CLAP, built with DPF's Makefiles and a GNU toolchain. This file builds the same
# sources through DPF's own CMake module (ISC), so MSVC can build them with the
# rest of our line and no MinGW. The source lists mirror upstream's per-plug-in
# Makefiles; the ids are upstream's own (DISTRHO_PLUGIN_CLAP_ID), because the
# plug-in is the one they release -- a session saved with theirs opens ours.
#
#   cmake -S plugins/external/dragonfly -B <build> -DDRAGONFLY_SOURCE=<checkout>

cmake_minimum_required(VERSION 3.15)
set(DRAGONFLY_SOURCE "" CACHE PATH "The dragonfly-reverb checkout, DPF submodule included (tools/fetch_plugins.sh)")
if (NOT EXISTS "${DRAGONFLY_SOURCE}/dpf/cmake/DPF-plugin.cmake")
    message(FATAL_ERROR "DRAGONFLY_SOURCE is not a dragonfly-reverb checkout with DPF: '${DRAGONFLY_SOURCE}'")
endif()
set(CMAKE_CXX_STANDARD 17)
project(adi_dragonfly_clap LANGUAGES C CXX)

add_subdirectory(${DRAGONFLY_SOURCE}/dpf dpf)

set(DF ${DRAGONFLY_SOURCE})
set(DF_COMMON
    ${DF}/common/kiss_fft/kiss_fft.c
    ${DF}/common/kiss_fft/kiss_fftr.c
    ${DF}/common/freeverb/allpass.cpp
    ${DF}/common/freeverb/biquad.cpp
    ${DF}/common/freeverb/comb.cpp
    ${DF}/common/freeverb/delay.cpp
    ${DF}/common/freeverb/delayline.cpp
    ${DF}/common/freeverb/earlyref.cpp
    ${DF}/common/freeverb/efilter.cpp
    ${DF}/common/freeverb/nrev.cpp
    ${DF}/common/freeverb/nrevb.cpp
    ${DF}/common/freeverb/progenitor.cpp
    ${DF}/common/freeverb/progenitor2.cpp
    ${DF}/common/freeverb/revbase.cpp
    ${DF}/common/freeverb/slot.cpp
    ${DF}/common/freeverb/strev.cpp
    ${DF}/common/freeverb/utils.cpp
    ${DF}/common/freeverb/zrev.cpp
    ${DF}/common/freeverb/zrev2.cpp)

# One plug-in: <name> <upstream dir> [SPECTROGRAM]. Early Reflections is the one
# without the spectrogram view.
#
# Each plug-in folder holds AbstractUI.cpp/.hpp as git SYMLINKS to common/. A
# Windows checkout without symlink rights writes them as one-line text files
# naming the target, and UI.hpp's #include "AbstractUI.hpp" finds that stub
# before any include path. So the folder is copied into the build tree without
# them and the real files are copied in beside it: the checkout stays as fetched
# and the build is the same on every system.
function(adi_dragonfly name dir)
    set(src ${CMAKE_BINARY_DIR}/src/${dir})
    file(COPY ${DF}/plugins/${dir}/ DESTINATION ${src}
         PATTERN "AbstractUI.*" EXCLUDE PATTERN "Makefile" EXCLUDE)
    file(COPY ${DF}/common/AbstractUI.cpp ${DF}/common/AbstractUI.hpp DESTINATION ${src})
    set(ui ${src}/Artwork.cpp ${src}/UI.cpp ${src}/AbstractUI.cpp
           ${DF}/common/LabelledKnob.cpp ${DF}/common/Selection.cpp
           ${DF}/common/Bitstream_Vera_Sans_Regular.cpp)
    if ("SPECTROGRAM" IN_LIST ARGN)
        list(APPEND ui ${DF}/common/Spectrogram.cpp)
    endif()
    dpf_add_plugin(${name}
        TARGETS clap
        UI_TYPE opengl
        FILES_COMMON ${src}/DSP.cpp ${DF_COMMON}
        FILES_DSP ${src}/Plugin.cpp
        FILES_UI ${ui})
    target_include_directories(${name} PUBLIC ${src} ${DF}/common)
    # M_E and friends are POSIX; MSVC's <cmath> declares them only on request.
    target_compile_definitions(${name} PUBLIC LIBFV3_FLOAT $<$<BOOL:${MSVC}>:_USE_MATH_DEFINES>)
endfunction()

adi_dragonfly(DragonflyHallReverb       dragonfly-hall-reverb       SPECTROGRAM)
adi_dragonfly(DragonflyRoomReverb       dragonfly-room-reverb       SPECTROGRAM)
adi_dragonfly(DragonflyPlateReverb      dragonfly-plate-reverb      SPECTROGRAM)
adi_dragonfly(DragonflyEarlyReflections dragonfly-early-reflections)
