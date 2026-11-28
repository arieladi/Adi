# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Gate (DEV-22, ADR-0249 d7; lands after 3 December): the core, its Pd
# external and canonical canvas (pd/devices/Gate.pd), dispatched natively by the
# built-in host; it reports the Lookahead latency. Its defaults and laws are ADI's
# until the director's reading of Live's Gate (reading 20): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/live_gate.cpp)
add_executable(adi_live_gate_tests tests/test_live_gate.cpp)
target_link_libraries(adi_live_gate_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_live_gate_tests COMMAND adi_live_gate_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/live_gate_tilde.cpp)
endif()
