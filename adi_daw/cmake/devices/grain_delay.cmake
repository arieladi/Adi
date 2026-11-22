# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Grain Delay (DEV-22; lands after 3 December): the core, its Pd external and canonical
# canvas (pd/devices/Grain Delay.pd), dispatched natively by the built-in host. Its laws are ADI's
# until a Live 12.4 reading (reading 40): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/grain_delay.cpp)
add_executable(adi_grain_delay_tests tests/test_grain_delay.cpp)
target_link_libraries(adi_grain_delay_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_grain_delay_tests COMMAND adi_grain_delay_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/grain_delay_tilde.cpp)
endif()
