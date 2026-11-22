# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Tuner (DEV-22; lands after 3 December): the core and the pitch detector Auto Shift
# shares later, its Pd external and canonical canvas (pd/devices/Tuner.pd), dispatched natively
# by the built-in host. Its controls are ADI's until a Live 12.4 reading (reading 27): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/pitch_detector.cpp src/adi/dsp/tuner.cpp)
add_executable(adi_tuner_tests tests/test_tuner.cpp)
target_link_libraries(adi_tuner_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_tuner_tests COMMAND adi_tuner_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/tuner_tilde.cpp)
endif()
