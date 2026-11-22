# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Compressor, the core only (DEV-22): no canvas, no catalogue entry
# until the director's reading of Live's Compressor (reading 17).
target_sources(adi_core PRIVATE src/adi/dsp/live_compressor.cpp)
add_executable(adi_live_compressor_tests tests/test_live_compressor.cpp)
target_link_libraries(adi_live_compressor_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_live_compressor_tests COMMAND adi_live_compressor_tests)
