# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/colorcab.cpp)
add_executable(adi_colorcab_tests tests/test_colorcab.cpp)
target_link_libraries(adi_colorcab_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_colorcab_tests COMMAND adi_colorcab_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/colorcab_tilde.cpp)
# This end-to-end suite covers both Chord Comb and Color Cab.
    add_executable(adi_colorbass_pd_tests tests/test_colorbass_pd.cpp)
    target_link_libraries(adi_colorbass_pd_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_colorbass_pd_tests PRIVATE
            ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd"
            ADI_COLORBASS_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd/devices")
    add_test(NAME adi_colorbass_pd_tests COMMAND adi_colorbass_pd_tests)
endif()
