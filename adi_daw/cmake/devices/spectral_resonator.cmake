# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Spectral Resonator (DEV-22; lands after 3 December): the core, its Pd external and
# canonical canvas (pd/devices/Spectral Resonator.pd), dispatched natively by the built-in host. Its
# laws are ADI's until a Live 12.4 reading (reading 44): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/spectral_resonator.cpp)
add_executable(adi_spectral_resonator_tests tests/test_spectral_resonator.cpp)
target_link_libraries(adi_spectral_resonator_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_spectral_resonator_tests COMMAND adi_spectral_resonator_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/spectral_resonator_tilde.cpp)
endif()
