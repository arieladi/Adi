# SPDX-License-Identifier: AGPL-3.0-or-later
if(ADI_WITH_JUCE)
    set(adi_shaper_editor_sources
        src/juce/adi_shaper_panel.cpp
        src/juce/adi_shaper_panel_session.cpp
        src/juce/adi_shaper_panel_view.cpp)
    # The shell target is declared after device modules are discovered.
    function(adi_shaper_attach_panel)
        target_sources(adi_ui_shell PRIVATE ${adi_shaper_editor_sources})
    endfunction()
    cmake_language(DEFER CALL adi_shaper_attach_panel)

    juce_add_console_app(adi_shaper_panel_tests PRODUCT_NAME "adi_shaper_panel_tests")
    target_sources(adi_shaper_panel_tests PRIVATE tests/adi_shaper_panel_tests.cpp src/juce/adi_shaper_panel.cpp)
    target_link_libraries(adi_shaper_panel_tests PRIVATE adi_core juce::juce_gui_basics)
    target_compile_definitions(adi_shaper_panel_tests PRIVATE JUCE_STANDALONE_APPLICATION=1)
    add_test(NAME adi_shaper_panel_tests COMMAND adi_shaper_panel_tests)

    if(ADI_WITH_PD)
        juce_add_console_app(adi_shaper_panel_session_tests PRODUCT_NAME "adi_shaper_panel_session_tests")
        target_sources(adi_shaper_panel_session_tests PRIVATE tests/adi_shaper_panel_session_tests.cpp
            src/juce/adi_shaper_panel_session.cpp)
        target_link_libraries(adi_shaper_panel_session_tests PRIVATE adi_core juce::juce_gui_basics)
        target_compile_definitions(adi_shaper_panel_session_tests PRIVATE JUCE_STANDALONE_APPLICATION=1)
        add_test(NAME adi_shaper_panel_session_tests COMMAND adi_shaper_panel_session_tests)
    endif()

    set_property(SOURCE ${adi_shaper_editor_sources} tests/adi_shaper_panel_tests.cpp
        tests/adi_shaper_panel_session_tests.cpp APPEND PROPERTY COMPILE_OPTIONS
        "$<TARGET_PROPERTY:adi_warnings,INTERFACE_COMPILE_OPTIONS>")
endif()
