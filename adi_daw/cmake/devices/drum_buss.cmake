# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Drum Buss (DEV-22; lands after 3 December): the core, its Pd external and canonical
# canvas (pd/devices/Drum Buss.pd), dispatched natively by the built-in host. Its curves are ADI's
# until a Live 12.4 reading (reading 38): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/drum_buss.cpp)
add_executable(adi_drum_buss_tests tests/test_drum_buss.cpp)
target_link_libraries(adi_drum_buss_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_drum_buss_tests COMMAND adi_drum_buss_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/drum_buss_tilde.cpp)
endif()
