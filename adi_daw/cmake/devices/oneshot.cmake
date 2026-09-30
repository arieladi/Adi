# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/oneshot.cpp)
add_executable(adi_oneshot_tests tests/test_oneshot.cpp)
target_link_libraries(adi_oneshot_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_oneshot_tests COMMAND adi_oneshot_tests)
