# Step 7.2: persistence has no GUI dependency; the window tier is optional.
target_sources(adi_core PRIVATE src/adi/ui/window_state.cpp src/adi/ui/theme.cpp src/adi/ui/density.cpp src/adi/ui/project_document.cpp src/adi/ui/plugin_hold_off.cpp src/adi/ui/parameter_feed.cpp src/adi/ui/session_state.cpp src/adi/ui/stack_model.cpp src/adi/ui/shared_analysis.cpp)
target_sources(adi_core PRIVATE src/adi/ui/analysis_comparison.cpp)
add_executable(adi_ui_analysis_comparison_tests tests/test_analysis_comparison.cpp)
target_link_libraries(adi_ui_analysis_comparison_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_ui_analysis_comparison_tests COMMAND adi_ui_analysis_comparison_tests)
add_executable(adi_ui_spectrum_slope_tests tests/test_spectrum_slope.cpp)
target_include_directories(adi_ui_spectrum_slope_tests PRIVATE src)
target_link_libraries(adi_ui_spectrum_slope_tests PRIVATE adi_warnings)
add_test(NAME adi_ui_spectrum_slope_tests COMMAND adi_ui_spectrum_slope_tests)
add_library(adi_spectrum_average STATIC src/adi/dsp/spectrum_average.cpp)
target_include_directories(adi_spectrum_average PUBLIC src)
target_compile_features(adi_spectrum_average PUBLIC cxx_std_20)
target_link_libraries(adi_spectrum_average PRIVATE adi_warnings)
target_link_libraries(adi_core PRIVATE adi_spectrum_average)
set_target_properties(adi_spectrum_average PROPERTIES POSITION_INDEPENDENT_CODE ON INTERPROCEDURAL_OPTIMIZATION FALSE)
if(MSVC)
    target_compile_options(adi_spectrum_average PRIVATE /fp:precise)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(adi_spectrum_average PRIVATE /clang:-ffp-model=precise /clang:-ffp-contract=off /clang:-fno-lto)
    else()
        target_compile_options(adi_spectrum_average PRIVATE /GL-)
    endif()
    set_property(SOURCE tests/test_spectrum_average.cpp APPEND PROPERTY COMPILE_OPTIONS /fp:precise)
    set_property(SOURCE tests/test_spectrum_average_fast.cpp APPEND PROPERTY COMPILE_OPTIONS /fp:fast)
else()
    target_compile_options(adi_spectrum_average PRIVATE -fno-fast-math -ffp-contract=off -fno-lto)
    set_property(SOURCE tests/test_spectrum_average.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-fast-math -ffp-contract=off)
    set_property(SOURCE tests/test_spectrum_average_fast.cpp APPEND PROPERTY COMPILE_OPTIONS -ffast-math)
endif()
add_executable(adi_ui_spectrum_average_tests tests/test_spectrum_average.cpp tests/test_spectrum_average_fast.cpp)
target_link_libraries(adi_ui_spectrum_average_tests PRIVATE adi_spectrum_average adi_warnings)
set_target_properties(adi_ui_spectrum_average_tests PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
add_test(NAME adi_ui_spectrum_average_tests COMMAND adi_ui_spectrum_average_tests)
add_executable(adi_ui_shared_analysis_tests tests/test_shared_analysis.cpp)
target_link_libraries(adi_ui_shared_analysis_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_ui_shared_analysis_tests COMMAND adi_ui_shared_analysis_tests)
add_executable(adi_ui_document_tests tests/test_ui_document.cpp)
target_link_libraries(adi_ui_document_tests PRIVATE adi_settings adi_warnings)
add_test(NAME adi_ui_document_tests COMMAND adi_ui_document_tests)
add_executable(adi_ui_window_state_tests tests/test_ui_window_state.cpp)
target_link_libraries(adi_ui_window_state_tests PRIVATE adi_core adi_settings adi_warnings)
add_test(NAME adi_ui_window_state_tests COMMAND adi_ui_window_state_tests)
add_executable(adi_ui_stack_model_tests tests/test_stack_model.cpp)
target_link_libraries(adi_ui_stack_model_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_ui_stack_model_tests COMMAND adi_ui_stack_model_tests)
# The MIDI Note Editor's geometry (STEP-7-PLAN 7.7 step 2), without a window.
add_executable(adi_ui_note_geometry_tests tests/test_ui_note_geometry.cpp)
target_link_libraries(adi_ui_note_geometry_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_ui_note_geometry_tests COMMAND adi_ui_note_geometry_tests)
add_executable(adi_ui_note_edits_tests tests/test_ui_note_edits.cpp)
target_link_libraries(adi_ui_note_edits_tests PRIVATE adi_core adi_warnings)
add_test(NAME adi_ui_note_edits_tests COMMAND adi_ui_note_edits_tests)
if(ADI_WITH_JUCE)
    add_subdirectory(src/adi/mix/clip)
    add_subdirectory(src/adi/mix/limit_chain)
    add_executable(adi_mix_clip_tests tests/test_mix_clip.cpp)
    target_link_libraries(adi_mix_clip_tests PRIVATE adi_mix_clip adi_warnings)
    add_test(NAME adi_mix_clip_tests COMMAND adi_mix_clip_tests)
    add_executable(adi_mix_limit_chain_tests tests/test_mix_limit_chain.cpp)
    target_link_libraries(adi_mix_limit_chain_tests PRIVATE adi_mix_limit_chain adi_warnings)
    add_test(NAME adi_mix_limit_chain_tests COMMAND adi_mix_limit_chain_tests)
    add_library(adi_mix_pipeline_host STATIC src/juce/mix_pipeline_host.cpp)
    target_link_libraries(adi_mix_pipeline_host PUBLIC adi_core PRIVATE adi_mix_limit_chain adi_warnings)
    target_link_libraries(adi_play PRIVATE adi_mix_pipeline_host)
    target_link_libraries(adi_play_render_tests PRIVATE adi_mix_pipeline_host)
    add_executable(adi_mix_pipeline_document_tests tests/test_mix_pipeline_document.cpp)
    target_link_libraries(adi_mix_pipeline_document_tests PRIVATE adi_mix_pipeline_host adi_warnings)
    add_test(NAME adi_mix_pipeline_document_tests COMMAND adi_mix_pipeline_document_tests)
    # JUCE's generated resource/artefact paths exceed MAX_PATH in deep worktrees.
    if(WIN32)
        string(LENGTH "${CMAKE_BINARY_DIR}" adi_ui_path_length)
        if(adi_ui_path_length GREATER 100)
            message(FATAL_ERROR "Step 7 JUCE build needs a short Windows build path (under 100 characters).")
        endif()
    endif()
    add_library(adi_ui_shell STATIC src/juce/ui_shell.cpp src/juce/ui_arrangement.cpp src/juce/ui_browser.cpp src/juce/ui_controlbar.cpp src/juce/ui_devices.cpp src/juce/ui_floating.cpp src/juce/ui_look.cpp src/juce/ui_controls.cpp src/juce/ui_mixer.cpp src/juce/ui_mixview.cpp src/juce/ui_mix_channels.cpp src/juce/ui_mix_meter.cpp src/juce/ui_mix_analysers.cpp src/juce/ui_mix_focus.cpp src/juce/ui_settings.cpp src/juce/ui_stack.cpp src/juce/ui_analysis.cpp src/juce/ui_channel_config.cpp src/juce/ui_history.cpp src/juce/ui_clipview.cpp src/juce/ui_clip_controller.cpp src/juce/ui_export.cpp)
    # Some libraries offer only integer charconv, or expose floating symbols
    # that the selected macOS deployment target cannot call.
    set(adi_ui_charconv_flags)
    if(APPLE)
        list(APPEND adi_ui_charconv_flags -Werror=unguarded-availability)
    endif()
    try_compile(adi_ui_float_charconv SOURCE_FROM_CONTENT charconv.cpp [=[
        #include <charconv>
        int main() {
            char buffer[64]{};
            double value = 1.25;
            const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
            return static_cast<int>(std::from_chars(buffer, written.ptr, value, std::chars_format::general).ec);
        }
    ]=] NO_CACHE COMPILE_DEFINITIONS ${adi_ui_charconv_flags})
    if(adi_ui_float_charconv)
        set_property(SOURCE src/juce/ui_arrangement.cpp src/juce/ui_clipview.cpp APPEND
            PROPERTY COMPILE_DEFINITIONS ADI_UI_FLOAT_CHARCONV)
    endif()
    set(classic_skin "${CMAKE_CURRENT_LIST_DIR}/smexoscope/images")
    juce_add_binary_data(adi_scope_skin NAMESPACE ScopeClassicImages HEADER_NAME ScopeClassicImages.h SOURCES
        ${classic_skin}/body.png ${classic_skin}/blue_knob1_4.png ${classic_skin}/free_etc.png
        ${classic_skin}/lefr_right.png ${classic_skin}/off_on.png ${classic_skin}/slider_new.png
        ${classic_skin}/heads.png ${classic_skin}/readout.png)
    set_target_properties(adi_scope_skin PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(classic_widgets src/juce/smexoscope/widgets)
    target_sources(adi_ui_shell PRIVATE src/juce/ui_scope_classic.cpp
        ${classic_widgets}/WaveDisplay.cpp ${classic_widgets}/TextElement.cpp
        ${classic_widgets}/MultiStateButton.cpp ${classic_widgets}/CustomKnob.cpp ${classic_widgets}/CustomSlider.cpp)
    set_source_files_properties(src/juce/ui_scope_classic.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
    target_link_libraries(adi_ui_shell PRIVATE adi_scope_skin)
    target_link_libraries(adi_ui_shell PUBLIC adi_settings juce::juce_gui_basics juce::juce_gui_extra)
    # SRF-05: the MIDI ports stay on the Win32 backend; Windows MIDI Services and WinRT MIDI are pinned off so
    # every target that compiles juce_audio_devices through the shell agrees on the backend.
    target_compile_definitions(adi_ui_shell PUBLIC JUCE_STANDALONE_APPLICATION=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
                                                  JUCE_USE_WINDOWS_MIDI_SERVICES=0 JUCE_USE_WINRT_MIDI=0)
    juce_add_console_app(adi_ui_shell_tests PRODUCT_NAME "adi_ui_shell_tests")
    # The Settings window's Audio page drives a device manager; the tests build it over an unopened one.
    target_sources(adi_ui_shell_tests PRIVATE tests/test_ui_shell.cpp src/juce/app_audio_page.cpp src/juce/app_midi_ports.cpp)
    target_link_libraries(adi_ui_shell_tests PRIVATE adi_ui_shell juce::juce_audio_devices)
    target_compile_definitions(adi_ui_shell_tests PRIVATE ADI_UI_REFERENCES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/ui")
    # The suite's fixtures keep whole root windows on the stack. Windows gives a program 1 MB of stack
    # where Linux and macOS give 8 MB, and a Debug build's frames are large: on 27 November the root grew
    # (the Info box's own analyser, the recording fixtures) and the suite overflowed on Windows only. The
    # program itself keeps its root on the heap; this is the test executable's stack, made as large as
    # the other platforms'.
    if (MSVC)
        target_link_options(adi_ui_shell_tests PRIVATE /STACK:8388608)
    endif()
    # Apply our warning policy to our code, not JUCE's module translation units.
    set_source_files_properties(src/juce/ui_shell.cpp src/juce/ui_arrangement.cpp src/juce/ui_browser.cpp src/juce/ui_controlbar.cpp src/juce/ui_devices.cpp src/juce/ui_floating.cpp src/juce/ui_look.cpp src/juce/ui_controls.cpp src/juce/ui_mixer.cpp src/juce/ui_mixview.cpp src/juce/ui_mix_channels.cpp src/juce/ui_mix_meter.cpp src/juce/ui_mix_analysers.cpp src/juce/ui_mix_focus.cpp src/juce/ui_settings.cpp src/juce/ui_stack.cpp src/juce/ui_analysis.cpp src/juce/ui_channel_config.cpp src/juce/ui_history.cpp src/juce/ui_clipview.cpp src/juce/ui_clip_controller.cpp src/juce/ui_export.cpp src/juce/app_audio_page.cpp src/juce/app_midi_ports.cpp tests/test_ui_shell.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
    add_test(NAME adi_ui_shell_tests COMMAND adi_ui_shell_tests)
    add_test(NAME adi_ui_appearance_settings_tests COMMAND adi_ui_shell_tests --appearance-settings-only)
    juce_add_console_app(adi_ui_shared_spectrum_tests PRODUCT_NAME "adi_ui_shared_spectrum_tests")
    target_sources(adi_ui_shared_spectrum_tests PRIVATE tests/test_ui_shared_analysis.cpp)
    target_link_libraries(adi_ui_shared_spectrum_tests PRIVATE adi_ui_shell)
    set_source_files_properties(tests/test_ui_shared_analysis.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
    add_test(NAME adi_ui_shared_spectrum_tests COMMAND adi_ui_shared_spectrum_tests)
    if(APPLE)
        set_tests_properties(adi_ui_shared_spectrum_tests PROPERTIES
            FAIL_REGULAR_EXPRESSION "Leaked objects detected:.*ImagePixelData")
    endif()
    juce_add_console_app(adi_ui_mix_functions_tests PRODUCT_NAME "adi_ui_mix_functions_tests")
    target_sources(adi_ui_mix_functions_tests PRIVATE tests/test_ui_mix_functions.cpp)
    target_link_libraries(adi_ui_mix_functions_tests PRIVATE adi_ui_shell juce::juce_audio_devices)
    set_source_files_properties(tests/test_ui_mix_functions.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
    add_test(NAME adi_ui_mix_functions_tests COMMAND adi_ui_mix_functions_tests)
    juce_add_console_app(adi_ui_eq_pages_tests PRODUCT_NAME "adi_ui_eq_pages_tests")
    target_sources(adi_ui_eq_pages_tests PRIVATE tests/test_ui_eq_pages.cpp)
    target_link_libraries(adi_ui_eq_pages_tests PRIVATE adi_ui_shell)
    set_source_files_properties(tests/test_ui_eq_pages.cpp PROPERTIES
        COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
    add_test(NAME adi_ui_eq_pages_tests COMMAND adi_ui_eq_pages_tests)
    set(adi_program_icon "${CMAKE_CURRENT_SOURCE_DIR}/src/juce/assets/program.png")
    set(adi_notice_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party_notices")
    juce_add_binary_data(adi_app_resources NAMESPACE AdiAppResources HEADER_NAME adi_app_resources.hpp SOURCES
        "${adi_program_icon}"
        "${adi_notice_dir}/analysis_palettes/README.md"
        "${adi_notice_dir}/analysis_palettes/CMasher-LICENSE.txt"
        "${adi_notice_dir}/analysis_palettes/matplotlib-LICENSE.txt"
        "${adi_notice_dir}/analysis_palettes/seaborn-LICENSE.txt"
        "${adi_notice_dir}/smexoscope/NOTICE.md"
        "${adi_notice_dir}/smexoscope/LICENSE.txt")
    juce_add_gui_app(adi_app PRODUCT_NAME "ADI" COMPANY_NAME "ADI" BUNDLE_ID "org.adi.daw"
        ICON_BIG "${adi_program_icon}"
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "ADI uses audio input to record audio tracks.")
    set(_adi_starter_drums "${CMAKE_CURRENT_SOURCE_DIR}/content/starter-drums")
    if(APPLE)
        juce_add_bundle_resources_directory(adi_app "${_adi_starter_drums}")
        target_compile_definitions(adi_app PRIVATE ADI_STARTER_DRUMS_RELATIVE="../Resources/starter-drums")
        add_test(NAME adi_ui_microphone_usage_tests
            COMMAND /usr/libexec/PlistBuddy -c "Print :NSMicrophoneUsageDescription"
                "$<TARGET_BUNDLE_CONTENT_DIR:adi_app>/Info.plist")
    endif()
    if(WIN32)
        # SC-388: a sample at a path over 260 characters imports (where Windows allows long paths).
        target_sources(adi_app PRIVATE src/juce/long_paths.manifest)
    endif()
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The build tree works without an install. Only the installer decides where to stage the kit.
        add_custom_target(adi_starter_drums
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_adi_starter_drums}"
                    "$<TARGET_FILE_DIR:adi_app>/starter-drums" VERBATIM)
        add_dependencies(adi_app adi_starter_drums)
    endif()
    target_sources(adi_app PRIVATE src/juce/ui_main.cpp src/juce/ui_backup_trash.cpp src/juce/app_audio_page.cpp src/juce/app_midi_ports.cpp src/juce/juce_device_loader.cpp src/juce/vst3_host.cpp src/juce/vst3_events.cpp src/juce/editor_host_component.cpp)
    # Reuse the player's host-format/ASIO switches, not its console entry or fixture.
    get_target_property(adi_ui_host_defs adi_play COMPILE_DEFINITIONS)
    list(FILTER adi_ui_host_defs EXCLUDE REGEX "^(_CONSOLE|ADI_TEST_VST3)")
    target_compile_definitions(adi_app PRIVATE ${adi_ui_host_defs})
    target_link_libraries(adi_app PRIVATE adi_app_resources adi_ui_shell adi_mix_pipeline_host juce::juce_audio_processors juce::juce_audio_utils juce::juce_audio_devices)
    if(WIN32)
        target_link_libraries(adi_app PRIVATE ole32 shell32)
    endif()
    add_test(NAME adi_ui_quit_export_tests
        COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:adi_app>" "-DFIXTURE=$<TARGET_FILE:adi_ui_document_tests>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_quit_export_probe.cmake")
    if(_adi_proc_incs)
        target_include_directories(adi_app SYSTEM PRIVATE ${_adi_proc_incs})
    endif()
    set_source_files_properties(src/juce/ui_main.cpp src/juce/ui_backup_trash.cpp src/juce/app_midi_ports.cpp PROPERTIES COMPILE_OPTIONS "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        include(cmake/install_adi.cmake)
    endif()
endif()
