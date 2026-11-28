# SPDX-License-Identifier: AGPL-3.0-or-later
include_guard(GLOBAL)

# ADR-0226 uses qm-dsp 4d2a4a4e0c2dd0ddf07bf9d2a224ded912712714 and
# the TempoTrackV2 fork from Mixxx 902cfc2543d7ef65bda1678e6f3c3e98be4aa39d.
# MixxxRoot names Mixxx's lib/qm-dsp directory, not the Mixxx repository root.
function(adi_add_qmdsp qmRoot mixxxRoot)
    if(TARGET adi_qmdsp)
        return()
    endif()
    if(NOT EXISTS "${qmRoot}/ext/kissfft/tools/kiss_fftr.c" OR
       NOT EXISTS "${mixxxRoot}/dsp/tempotracking/TempoTrackV2.cpp")
        message(FATAL_ERROR "Supply the pinned qm-dsp and Mixxx lib/qm-dsp source roots")
    endif()

    # Mixxx's half-window padding forms df.begin() + i with i < 0 before adding
    # k back: undefined, and an MSVC Debug assertion that waits on a dialog.
    # Build a copy that adds the offsets first; the pinned tree stays untouched.
    set(_tracker "${mixxxRoot}/dsp/tempotracking/TempoTrackV2.cpp")
    file(READ "${_tracker}" _source)
    set(_padded "std::copy(df.begin() + i + k, df.begin() + i + l,")
    string(FIND "${_source}" "${_padded}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "${_tracker} no longer has the padded copy this build corrects")
    endif()
    string(REPLACE "${_padded}" "std::copy(df.begin() + (i + k), df.begin() + (i + l)," _source "${_source}")
    set(_copy "${CMAKE_CURRENT_BINARY_DIR}/generated/qmdsp/TempoTrackV2.cpp")
    file(WRITE "${_copy}.in" "${_source}")
    configure_file("${_copy}.in" "${_copy}" COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_tracker}")

    # Select the approved algorithms and their dependencies. In particular,
    # do not build the original TempoTrackV2 or qm-dsp's unrelated PCA code.
    add_library(adi_qmdsp STATIC
        "${_copy}"
        "${mixxxRoot}/maths/MathUtilities.cpp"
        "${qmRoot}/dsp/onsets/DetectionFunction.cpp"
        "${qmRoot}/dsp/phasevocoder/PhaseVocoder.cpp"
        "${qmRoot}/dsp/rateconversion/Decimator.cpp"
        "${qmRoot}/dsp/tempotracking/DownBeat.cpp"
        "${qmRoot}/dsp/transforms/FFT.cpp"
        "${qmRoot}/maths/KLDivergence.cpp"
        "${qmRoot}/ext/kissfft/kiss_fft.c"
        "${qmRoot}/ext/kissfft/tools/kiss_fftr.c")
    target_include_directories(adi_qmdsp SYSTEM PUBLIC "${mixxxRoot}" "${qmRoot}")
    target_include_directories(adi_qmdsp SYSTEM PRIVATE "${mixxxRoot}/dsp/tempotracking"
        "${qmRoot}/ext/kissfft" "${qmRoot}/ext/kissfft/tools")
    target_compile_definitions(adi_qmdsp PRIVATE kiss_fft_scalar=double)
    if(MSVC)
        target_compile_definitions(adi_qmdsp PRIVATE _USE_MATH_DEFINES NOMINMAX)
    endif()
endfunction()
