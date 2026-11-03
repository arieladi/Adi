# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/multiband_dynamics.cpp)
# Live's float pipeline is matched rounding for rounding (the resampler bit-exactly):
# no contraction into fused multiply-adds. MSVC's /fp:precise does not contract.
if(NOT MSVC)
    set_source_files_properties(src/adi/dsp/multiband_dynamics.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
add_executable(adi_multiband_dynamics_tests tests/test_multiband_dynamics.cpp)
target_link_libraries(adi_multiband_dynamics_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_multiband_dynamics_tests COMMAND adi_multiband_dynamics_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/multiband_dynamics_tilde.cpp)
    add_executable(adi_multiband_dynamics_pd_tests tests/test_multiband_dynamics_pd.cpp)
    target_link_libraries(adi_multiband_dynamics_pd_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_multiband_dynamics_pd_tests PRIVATE ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd" ADI_DEVICE_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd/devices")
    add_test(NAME adi_multiband_dynamics_pd_tests COMMAND adi_multiband_dynamics_pd_tests)
endif()
