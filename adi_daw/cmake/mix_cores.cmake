# SPDX-License-Identifier: AGPL-3.0-or-later
set(ADI_MIX_LIMITER_BUILD_OWNER_TESTS ON)
add_subdirectory(src/adi/mix/limiter)
target_link_libraries(adi_mix_limiter_owner_tests PRIVATE adi_warnings)
target_link_libraries(adi_core PRIVATE adi_mix_limiter)
set(ADI_MIX_EQ_HIGHWAY_SOURCE_DIR "${ADI_THIRD_PARTY}/highway" CACHE PATH
    "Pinned Highway 1.4.0 source for the authored EQ engine")
add_subdirectory(src/adi/mix/eq)
target_sources(adi_mix_eq PRIVATE src/adi/mix/eq/Backend.cpp)
target_link_libraries(adi_core PRIVATE adi_mix_eq)
target_sources(adi_core PRIVATE src/adi/mix_functions.cpp src/adi/mix_function_edit.cpp
    src/juce/mix_function_host.cpp src/juce/mix_eq_host.cpp)
foreach(_adi_mix_suite functions limiter document render_intent pdc_pin eq eq_host eq_pipeline)
    add_executable(adi_mix_${_adi_mix_suite}_tests tests/test_mix_${_adi_mix_suite}.cpp)
    target_link_libraries(adi_mix_${_adi_mix_suite}_tests PRIVATE adi_core adi_warnings)
    add_test(NAME adi_mix_${_adi_mix_suite}_tests COMMAND adi_mix_${_adi_mix_suite}_tests)
endforeach()
