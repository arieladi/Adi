# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Dynamic Tube (DEV-22; lands after 3 December): the core, its Pd external and canonical
# canvas (pd/devices/Dynamic Tube.pd), dispatched natively by the built-in host. Its curves are ADI's
# until a Live 12.4 reading (reading 39): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/dynamic_tube.cpp)
add_executable(adi_dynamic_tube_tests tests/test_dynamic_tube.cpp)
target_link_libraries(adi_dynamic_tube_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_dynamic_tube_tests COMMAND adi_dynamic_tube_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/dynamic_tube_tilde.cpp)
endif()
