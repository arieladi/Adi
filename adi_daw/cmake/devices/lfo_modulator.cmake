# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's LFO modulator, the kernel (DEV-07's device side, ADR-0249 d8; lands after
# 3 December). Core only: the engine's DEV-07 evaluator routes it (ADR-0230). Built, not
# verified, until a Live 12.4 reading of the LFO.
target_sources(adi_core PRIVATE src/adi/dsp/lfo_modulator.cpp)
add_executable(adi_lfo_modulator_tests tests/test_lfo_modulator.cpp)
target_link_libraries(adi_lfo_modulator_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_lfo_modulator_tests COMMAND adi_lfo_modulator_tests)
