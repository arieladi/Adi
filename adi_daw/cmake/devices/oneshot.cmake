# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/oneshot.cpp)
add_executable(adi_oneshot_tests tests/test_oneshot.cpp)
target_link_libraries(adi_oneshot_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_oneshot_tests COMMAND adi_oneshot_tests)
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/oneshot_tilde.cpp)
    add_executable(adi_oneshot_pd_tests tests/test_oneshot_pd.cpp)
    target_link_libraries(adi_oneshot_pd_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_oneshot_pd_tests PRIVATE
        ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd"
        ADI_DEVICE_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd/devices")
    add_test(NAME adi_oneshot_pd_tests COMMAND adi_oneshot_pd_tests)
endif()
