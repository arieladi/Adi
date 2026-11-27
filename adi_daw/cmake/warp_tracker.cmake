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

    # Select the approved algorithms and their dependencies. In particular,
    # do not build the original TempoTrackV2 or qm-dsp's unrelated PCA code.
    add_library(adi_qmdsp STATIC
        "${mixxxRoot}/dsp/tempotracking/TempoTrackV2.cpp"
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
    target_include_directories(adi_qmdsp SYSTEM PRIVATE
        "${qmRoot}/ext/kissfft" "${qmRoot}/ext/kissfft/tools")
    target_compile_definitions(adi_qmdsp PRIVATE kiss_fft_scalar=double)
    if(MSVC)
        target_compile_definitions(adi_qmdsp PRIVATE _USE_MATH_DEFINES NOMINMAX)
    endif()
endfunction()
