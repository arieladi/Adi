# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Vinyl Distortion (DEV-22; lands after 3 December): the core, its Pd external and
# canonical canvas (pd/devices/Vinyl Distortion.pd), dispatched natively by the built-in host. Its
# curves and crackle are ADI's until a Live 12.4 reading (reading 29): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/vinyl_distortion.cpp)
add_executable(adi_vinyl_distortion_tests tests/test_vinyl_distortion.cpp)
target_link_libraries(adi_vinyl_distortion_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_vinyl_distortion_tests COMMAND adi_vinyl_distortion_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/vinyl_distortion_tilde.cpp)
endif()
