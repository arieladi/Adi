# DF-E21 is a forward acceptance gate. The cloud implementation opts in explicitly.
option(ADI_ENABLE_DF_E21_ACCEPTANCE "Run strict hosted transport acceptance (DF-E21)" OFF)
if(NOT WIN32 OR NOT ADI_WITH_JUCE)
    if(ADI_ENABLE_DF_E21_ACCEPTANCE)
        message(FATAL_ERROR "DF-E21 acceptance currently requires Windows and ADI_WITH_JUCE=ON")
    endif()
    return()
endif()
if(NOT TARGET adi_research_time_probe)
    if(ADI_ENABLE_DF_E21_ACCEPTANCE)
        message(FATAL_ERROR "DF-E21 acceptance requires adi_research_time_probe")
    endif()
    return()
endif()
get_filename_component(_adi_df_root "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
juce_add_console_app(adi_df_e21_acceptance PRODUCT_NAME "adi_df_e21_acceptance")
set(_adi_df_sources
    "${CMAKE_CURRENT_LIST_DIR}/host_acceptance.cpp"
    "${_adi_df_root}/src/juce/vst3_host.cpp"
    "${_adi_df_root}/src/juce/vst3_events.cpp"
    "${_adi_df_root}/src/juce/editor_host_component.cpp")
target_sources(adi_df_e21_acceptance PRIVATE ${_adi_df_sources})
target_compile_features(adi_df_e21_acceptance PRIVATE cxx_std_20)
target_include_directories(adi_df_e21_acceptance PRIVATE "${_adi_df_root}/tests")
target_include_directories(adi_df_e21_acceptance SYSTEM PRIVATE
    "${ADI_THIRD_PARTY}/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK")
target_compile_definitions(adi_df_e21_acceptance PRIVATE
    ADI_DF_E21_PROBE_PATH="$<TARGET_FILE:adi_research_time_probe>"
    ADI_DF_E21_SCANNER_PATH="$<TARGET_FILE:adi_plugin_scanner>"
    JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0 JUCE_PLUGINHOST_ARA=0)
target_link_libraries(adi_df_e21_acceptance PRIVATE adi_core juce::juce_audio_processors juce::juce_audio_utils)
get_target_property(_adi_df_juce_incs juce::juce_audio_processors INTERFACE_INCLUDE_DIRECTORIES)
if(_adi_df_juce_incs)
    target_include_directories(adi_df_e21_acceptance SYSTEM PRIVATE ${_adi_df_juce_incs})
endif()
set_source_files_properties(${_adi_df_sources} PROPERTIES
    COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
if(MSVC)
    set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/host_acceptance.cpp" APPEND PROPERTY COMPILE_OPTIONS /WX)
endif()
add_dependencies(adi_df_e21_acceptance adi_research_time_probe adi_plugin_scanner)
add_test(NAME adi_df_e21_oracle COMMAND adi_df_e21_acceptance --self-test)
if(ADI_ENABLE_DF_E21_ACCEPTANCE)
    add_test(NAME adi_df_e21_host_acceptance COMMAND adi_df_e21_acceptance --acceptance)
    set_tests_properties(adi_df_e21_host_acceptance PROPERTIES LABELS "df-e21;hosted" TIMEOUT 120)
else()
    message(STATUS "DF-E21 host acceptance is disabled; enable ADI_ENABLE_DF_E21_ACCEPTANCE on the implementing branch")
endif()
