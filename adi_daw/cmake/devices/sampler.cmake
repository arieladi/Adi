# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/sampler.cpp)
add_executable(adi_sampler_tests tests/test_sampler.cpp)
target_link_libraries(adi_sampler_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_sampler_tests COMMAND adi_sampler_tests)
