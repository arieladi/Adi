# SPDX-License-Identifier: GPL-3.0-or-later
# Live 12's Limiter (DEV-22, ADR-0249 d7; lands after 3 December), on ADI's lookahead
# brickwall: the core, its Pd external and canonical canvas (pd/devices/Limiter.pd),
# dispatched natively by the built-in host; it reports its lookahead (plus the true-peak
# detector's delay in True Peak mode). Its defaults and laws are ADI's until the
# director's reading of Live's Limiter (reading 21): built, not verified.
target_sources(adi_core PRIVATE src/adi/dsp/live_limiter.cpp)
add_executable(adi_live_limiter_tests tests/test_live_limiter.cpp)
target_link_libraries(adi_live_limiter_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_live_limiter_tests COMMAND adi_live_limiter_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/live_limiter_tilde.cpp)
endif()
