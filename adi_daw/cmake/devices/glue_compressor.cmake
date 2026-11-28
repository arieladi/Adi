# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Glue Compressor (DEV-22, ADR-0249 d7; lands after 3 December): the core, its
# Pd external and canonical canvas (pd/devices/Glue Compressor.pd), dispatched natively
# by the built-in host. Its defaults and laws are ADI's until the director's reading of
# Live's Glue Compressor (reading 19): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/glue_compressor.cpp)
add_executable(adi_glue_compressor_tests tests/test_glue_compressor.cpp)
target_link_libraries(adi_glue_compressor_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_glue_compressor_tests COMMAND adi_glue_compressor_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/glue_compressor_tilde.cpp)
endif()
