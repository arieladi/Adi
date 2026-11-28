# SPDX-License-Identifier: GPL-3.0-or-later
# The DJ Filter (DEV-22; lands after 3 December): Auto Filter's DJ type as its own device, its Pd
# external and canonical canvas (pd/devices/DJ Filter.pd), dispatched natively by the built-in host.
# Its controls are ADI's until a Live 12.4 reading (reading 32): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/dj_filter.cpp)
add_executable(adi_dj_filter_tests tests/test_dj_filter.cpp)
target_link_libraries(adi_dj_filter_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_dj_filter_tests COMMAND adi_dj_filter_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/dj_filter_tilde.cpp)
endif()
