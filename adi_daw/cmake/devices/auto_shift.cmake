# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Auto Shift (DEV-22; lands after 3 December): the core on the Tuner's pitch detector and
# the Shifter's Pitch mode, its Pd external and canonical canvas (pd/devices/Auto Shift.pd),
# dispatched natively by the built-in host. Built, not verified: reading 33.
target_sources(adi_core PRIVATE src/adi/dsp/auto_shift.cpp)
add_executable(adi_auto_shift_tests tests/test_auto_shift.cpp)
target_link_libraries(adi_auto_shift_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_auto_shift_tests COMMAND adi_auto_shift_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/auto_shift_tilde.cpp)
endif()
