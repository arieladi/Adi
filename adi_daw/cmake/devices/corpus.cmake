# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Corpus (DEV-22; lands after 3 December): the core, its Pd external and canonical canvas
# (pd/devices/Corpus.pd), dispatched natively by the built-in host. Its models and laws are ADI's
# until a Live 12.4 reading (reading 37): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/corpus.cpp)
add_executable(adi_corpus_tests tests/test_corpus.cpp)
target_link_libraries(adi_corpus_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_corpus_tests COMMAND adi_corpus_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/corpus_tilde.cpp)
endif()
