# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Erosion (DEV-22; lands after 3 December): the core, its Pd external and canonical
# canvas (pd/devices/Erosion.pd), dispatched natively by the built-in host. Its depth law and
# noise band are ADI's until a Live 12.4 reading (reading 28): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/erosion.cpp)
add_executable(adi_erosion_tests tests/test_erosion.cpp)
target_link_libraries(adi_erosion_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_erosion_tests COMMAND adi_erosion_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/erosion_tilde.cpp)
endif()
