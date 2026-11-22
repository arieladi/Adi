# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Spectral Time (DEV-22; lands after 3 December): the core, its Pd external and canonical
# canvas (pd/devices/Spectral Time.pd), dispatched natively by the built-in host. Its laws are ADI's
# until a Live 12.4 reading (reading 43): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/spectral_time.cpp)
add_executable(adi_spectral_time_tests tests/test_spectral_time.cpp)
target_link_libraries(adi_spectral_time_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_spectral_time_tests COMMAND adi_spectral_time_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/spectral_time_tilde.cpp)
endif()
