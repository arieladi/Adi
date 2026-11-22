# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Glue Compressor, the core (DEV-22, ADR-0249 d7): no canvas, no catalogue entry
# yet; it lands after 3 December, and claims no parity before a Live reading.
target_sources(adi_core PRIVATE src/adi/dsp/glue_compressor.cpp)
add_executable(adi_glue_compressor_tests tests/test_glue_compressor.cpp)
target_link_libraries(adi_glue_compressor_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_glue_compressor_tests COMMAND adi_glue_compressor_tests)
