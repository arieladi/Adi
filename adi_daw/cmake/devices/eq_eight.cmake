# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/eq_eight.cpp)
# A band at 0 dB is bit-exact unity only when b*x and a*y round alike: no
# contraction into fused multiply-adds (Apple clang on arm64 contracts by
# default). MSVC's /fp:precise does not contract.
if(NOT MSVC)
    set_source_files_properties(src/adi/dsp/eq_eight.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
add_executable(adi_eq_eight_tests tests/test_eq_eight.cpp)
target_link_libraries(adi_eq_eight_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_eq_eight_tests COMMAND adi_eq_eight_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/eq_eight_tilde.cpp)
    add_executable(adi_eq_eight_pd_tests tests/test_eq_eight_pd.cpp)
    target_link_libraries(adi_eq_eight_pd_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_eq_eight_pd_tests PRIVATE ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd" ADI_DEVICE_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd/devices")
    add_test(NAME adi_eq_eight_pd_tests COMMAND adi_eq_eight_pd_tests)
endif()
