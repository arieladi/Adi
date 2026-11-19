# SPDX-License-Identifier: GPL-3.0-or-later
# DEV-28: the one loudness meter (src/adi/dsp/loudness) and its tests.
target_sources(adi_core PRIVATE src/adi/dsp/loudness.cpp)
add_executable(adi_loudness_tests tests/test_loudness.cpp)
target_link_libraries(adi_loudness_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_loudness_tests COMMAND adi_loudness_tests)
