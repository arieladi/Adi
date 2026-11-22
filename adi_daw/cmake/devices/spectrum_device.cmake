# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Spectrum as a track device (DEV-22; lands after 3 December): the lane's side, a
# pass-through core whose controls become the Mix View's shared analyser's settings, its Pd external
# and canonical canvas (pd/devices/Spectrum.pd; the contract is pd/devices/Spectrum.md). Reading 34.
target_sources(adi_core PRIVATE src/adi/dsp/spectrum_device.cpp)
add_executable(adi_spectrum_device_tests tests/test_spectrum_device.cpp)
target_link_libraries(adi_spectrum_device_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_spectrum_device_tests COMMAND adi_spectrum_device_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/spectrum_device_tilde.cpp)
endif()
