# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/redux.cpp)
add_executable(adi_redux_tests tests/test_redux.cpp)
target_link_libraries(adi_redux_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_redux_tests COMMAND adi_redux_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/redux_tilde.cpp)
    add_executable(adi_redux_pd_tests tests/test_redux_pd.cpp)
    target_link_libraries(adi_redux_pd_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_redux_pd_tests PRIVATE
            ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd"
            ADI_REDUX_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd/devices")
    add_test(NAME adi_redux_pd_tests COMMAND adi_redux_pd_tests)
endif()
