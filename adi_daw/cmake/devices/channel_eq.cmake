# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Channel EQ (DEV-22; lands after 3 December): the core, its Pd external and
# canonical canvas (pd/devices/Channel EQ.pd), dispatched natively by the built-in host.
# Its corners and laws are ADI's until a Live 12.4 reading (reading 25): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/channel_eq.cpp)
add_executable(adi_channel_eq_tests tests/test_channel_eq.cpp)
target_link_libraries(adi_channel_eq_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_channel_eq_tests COMMAND adi_channel_eq_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/channel_eq_tilde.cpp)
endif()
