# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Cabinet (DEV-22; lands after 3 December): the core, its Pd external and canonical canvas
# (pd/devices/Cabinet.pd), dispatched natively by the built-in host. Its responses are ADI's until a
# Live 12.4 reading (reading 36): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/live_cabinet.cpp)
add_executable(adi_live_cabinet_tests tests/test_live_cabinet.cpp)
target_link_libraries(adi_live_cabinet_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_live_cabinet_tests COMMAND adi_live_cabinet_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/live_cabinet_tilde.cpp)
endif()
