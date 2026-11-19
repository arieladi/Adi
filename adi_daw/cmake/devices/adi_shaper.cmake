# SPDX-License-Identifier: GPL-3.0-or-later
target_sources(adi_core PRIVATE src/adi/dsp/adi_shaper.cpp src/adi/dsp/adi_shaper_filter.cpp
                                src/adi/dsp/adi_shaper_liquid.cpp src/adi/dsp/adi_shaper_drive.cpp
                                src/adi/dsp/adi_shaper_crush.cpp src/adi/dsp/adi_shaper_time.cpp
                                src/adi/dsp/adi_shaper_pitch.cpp src/adi/dsp/adi_shaper_noise.cpp
                                src/adi/dsp/adi_shaper_reverb.cpp)
foreach(suite IN ITEMS adi_shaper adi_shaper_filter adi_shaper_liquid adi_shaper_drive adi_shaper_crush
                       adi_shaper_time adi_shaper_pitch adi_shaper_noise adi_shaper_reverb)
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

if(ADI_WITH_JUCE)
    # The owning window can embed this device-specific view using the gesture port.
    add_library(adi_shaper_curve INTERFACE)
    target_sources(adi_shaper_curve INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/juce/adi_shaper_curve.cpp")
    target_link_libraries(adi_shaper_curve INTERFACE adi_core juce::juce_gui_basics)
    juce_add_console_app(adi_shaper_curve_tests PRODUCT_NAME "adi_shaper_curve_tests")
    target_sources(adi_shaper_curve_tests PRIVATE tests/test_adi_shaper_curve.cpp)
    target_link_libraries(adi_shaper_curve_tests PRIVATE adi_shaper_curve)
    target_compile_definitions(adi_shaper_curve_tests PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    set_source_files_properties(src/juce/adi_shaper_curve.cpp tests/test_adi_shaper_curve.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
    add_test(NAME adi_shaper_curve_tests COMMAND adi_shaper_curve_tests)
endif()
