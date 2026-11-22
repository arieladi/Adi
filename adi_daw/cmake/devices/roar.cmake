# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Roar (DEV-22; lands after 3 December): the core, its Pd external and canonical canvas
# (pd/devices/Roar.pd), dispatched natively by the built-in host. Its shapes and routings are ADI's
# until a Live 12.4 reading (reading 42): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/roar.cpp)
add_executable(adi_roar_tests tests/test_roar.cpp)
target_link_libraries(adi_roar_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_roar_tests COMMAND adi_roar_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/roar_tilde.cpp)
endif()
