# SPDX-License-Identifier: GPL-3.0-or-later
if(ADI_WITH_PD)
    add_executable(adi_pd_zero_delay_tests tests/test_pd_zero_delay.cpp)
    target_link_libraries(adi_pd_zero_delay_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_pd_zero_delay_tests PRIVATE
        ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd"
        ADI_DEVICE_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd/devices")
    add_test(NAME adi_pd_zero_delay_tests COMMAND adi_pd_zero_delay_tests)
endif()
