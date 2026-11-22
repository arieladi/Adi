# WTS-01 (ADR-0248 d2, d12): the one-session acceptance suite, headless. `ctest -L acceptance`.
# The session's seven steps through the app's own document calls, no JUCE; OneShot needs Pd.
add_executable(adi_acceptance_wts01 tests/acceptance/test_session_wts01.cpp)
target_link_libraries(adi_acceptance_wts01 PRIVATE adi_core adi_settings adi_warnings)
add_test(NAME adi_acceptance_wts01 COMMAND adi_acceptance_wts01)
set_tests_properties(adi_acceptance_wts01 PROPERTIES LABELS acceptance TIMEOUT 900)
# The VST3 parts of steps 3 and 7: the repo's fixture instrument through the app's loader, no window.
if(ADI_WITH_JUCE AND TARGET adi_test_vst3 AND TARGET adi_play AND TARGET adi_mix_pipeline_host)
    juce_add_console_app(adi_acceptance_wts01_vst3 PRODUCT_NAME "adi_acceptance_wts01_vst3")
    target_sources(adi_acceptance_wts01_vst3 PRIVATE tests/acceptance/test_session_wts01_vst3.cpp
        src/juce/juce_device_loader.cpp src/juce/vst3_host.cpp src/juce/editor_host_component.cpp
        src/juce/vst3_events.cpp)
    target_compile_features(adi_acceptance_wts01_vst3 PRIVATE cxx_std_20)
    get_target_property(_adi_wts01_defs adi_play COMPILE_DEFINITIONS)
    target_compile_definitions(adi_acceptance_wts01_vst3 PRIVATE ${_adi_wts01_defs}
        ADI_SCAN_HELPER="$<TARGET_FILE:adi_plugin_scanner>" ADI_TEST_VST3="${_adi_fixture}")
    target_link_libraries(adi_acceptance_wts01_vst3 PRIVATE adi_core adi_settings adi_mix_pipeline_host
        juce::juce_audio_processors juce::juce_audio_utils juce::juce_audio_devices)
    if(_adi_proc_incs)
        target_include_directories(adi_acceptance_wts01_vst3 SYSTEM PRIVATE ${_adi_proc_incs})
    endif()
    add_dependencies(adi_acceptance_wts01_vst3 adi_plugin_scanner adi_test_vst3)
    add_test(NAME adi_acceptance_wts01_vst3 COMMAND adi_acceptance_wts01_vst3)
    set_tests_properties(adi_acceptance_wts01_vst3 PROPERTIES LABELS acceptance TIMEOUT 900)
endif()
