# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12.4's Auto Pan-Tremolo (DEV-22; lands after 3 December): the core, its Pd
# external and canonical canvas (pd/devices/Auto Pan-Tremolo.pd), dispatched natively by
# the built-in host. Its laws are ADI's until a Live 12.4 reading (reading 24): built, not
# verified.
target_sources(adi_core PRIVATE src/adi/dsp/auto_pan.cpp)
add_executable(adi_auto_pan_tests tests/test_auto_pan.cpp)
target_link_libraries(adi_auto_pan_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_auto_pan_tests COMMAND adi_auto_pan_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/auto_pan_tilde.cpp)
endif()
