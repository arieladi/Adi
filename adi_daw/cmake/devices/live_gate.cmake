# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Gate, the core (DEV-22, ADR-0249 d7): no canvas, no catalogue entry
# yet; it lands after 3 December, and claims no parity before a Live reading.
target_sources(adi_core PRIVATE src/adi/dsp/live_gate.cpp)
add_executable(adi_live_gate_tests tests/test_live_gate.cpp)
target_link_libraries(adi_live_gate_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_live_gate_tests COMMAND adi_live_gate_tests)
