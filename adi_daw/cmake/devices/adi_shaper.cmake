# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/adi_shaper.cpp src/adi/dsp/adi_shaper_filter.cpp
                                src/adi/dsp/adi_shaper_liquid.cpp src/adi/dsp/adi_shaper_drive.cpp
                                src/adi/dsp/adi_shaper_crush.cpp src/adi/dsp/adi_shaper_time.cpp)
foreach(suite IN ITEMS adi_shaper adi_shaper_filter adi_shaper_liquid adi_shaper_drive adi_shaper_crush adi_shaper_time)
    add_executable(${suite}_tests tests/test_${suite}.cpp)
    target_link_libraries(${suite}_tests PRIVATE adi_core adi_warnings)
    add_test(NAME ${suite}_tests COMMAND ${suite}_tests)
endforeach()
if(ADI_WITH_PD)
    target_sources(adi_pd_builtins PRIVATE src/adi/pd_builtins/adi_shaper_tilde.cpp)
    add_executable(adi_shaper_pd_tests tests/test_adi_shaper_pd.cpp)
    target_link_libraries(adi_shaper_pd_tests PRIVATE adi_core adi_warnings)
    target_compile_definitions(adi_shaper_pd_tests PRIVATE
        ADI_PD_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd"
        ADI_SHAPER_PATCH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pd/devices")
    add_test(NAME adi_shaper_pd_tests COMMAND adi_shaper_pd_tests)
endif()
