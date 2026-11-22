# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Amp (DEV-22; lands after 3 December): the core, its Pd external and canonical canvas
# (pd/devices/Amp.pd), dispatched natively by the built-in host. Its voices are ADI's until a
# Live 12.4 reading (reading 35): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/live_amp.cpp)
add_executable(adi_live_amp_tests tests/test_live_amp.cpp)
target_link_libraries(adi_live_amp_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_live_amp_tests COMMAND adi_live_amp_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/live_amp_tilde.cpp)
endif()
