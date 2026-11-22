# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Pedal (DEV-22; lands after 3 December): the core, its Pd external and canonical canvas
# (pd/devices/Pedal.pd), dispatched natively by the built-in host. Its voices are ADI's until a
# Live 12.4 reading (reading 41): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/live_pedal.cpp)
add_executable(adi_live_pedal_tests tests/test_live_pedal.cpp)
target_link_libraries(adi_live_pedal_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_live_pedal_tests COMMAND adi_live_pedal_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/live_pedal_tilde.cpp)
endif()
