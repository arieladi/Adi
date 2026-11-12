# SPDX-License-Identifier: AGPL-3.0-or-later
add_subdirectory(src/adi/mix/limiter)
target_link_libraries(adi_core PRIVATE adi_mix_limiter)
target_sources(adi_core PRIVATE src/adi/mix_functions.cpp src/adi/mix_function_edit.cpp
    src/juce/mix_function_host.cpp)
foreach(_adi_mix_suite functions limiter document render_intent pdc_pin)
    add_executable(adi_mix_${_adi_mix_suite}_tests tests/test_mix_${_adi_mix_suite}.cpp)
    target_link_libraries(adi_mix_${_adi_mix_suite}_tests PRIVATE adi_core adi_warnings)
    add_test(NAME adi_mix_${_adi_mix_suite}_tests COMMAND adi_mix_${_adi_mix_suite}_tests)
endforeach()
