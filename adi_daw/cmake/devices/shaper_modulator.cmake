# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Shaper modulator, the kernel (DEV-07's device side, ADR-0249 d8; lands after
# 3 December), on the LFO's clock (modulator_clock.hpp). Core only: the engine's DEV-07
# evaluator routes it (ADR-0230). Built, not verified, until reading 11.
target_sources(adi_core PRIVATE src/adi/dsp/shaper_modulator.cpp)
add_executable(adi_shaper_modulator_tests tests/test_shaper_modulator.cpp)
target_link_libraries(adi_shaper_modulator_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_shaper_modulator_tests COMMAND adi_shaper_modulator_tests)
