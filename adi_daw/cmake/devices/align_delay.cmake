# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Align Delay (DEV-22; lands after 3 December): the core, its Pd external and
# canonical canvas (pd/devices/Align Delay.pd), dispatched natively by the built-in host.
# Its ranges and laws are ADI's until a Live 12.4 reading of Align Delay (reading 23):
# built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/align_delay.cpp)
add_executable(adi_align_delay_tests tests/test_align_delay.cpp)
target_link_libraries(adi_align_delay_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_align_delay_tests COMMAND adi_align_delay_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/align_delay_tilde.cpp)
endif()
