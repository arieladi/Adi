# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/combchord.cpp)
add_executable(adi_combchord_tests tests/test_combchord.cpp)
target_link_libraries(adi_combchord_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_combchord_tests COMMAND adi_combchord_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/combchord_tilde.cpp)

endif()
