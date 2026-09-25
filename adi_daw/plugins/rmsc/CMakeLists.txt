# SPDX-License-Identifier: GPL-3.0-or-later
#
# ADI RMSC (ADR-0166): CLAP, VST3 and a standalone, from one JUCE target.

juce_add_plugin(adi_rmsc
    COMPANY_NAME "ADI"
    COMPANY_WEBSITE "https://github.com/arieladi/Adi"
    PLUGIN_MANUFACTURER_CODE Adiz
    PLUGIN_CODE Rmsc
    PRODUCT_NAME "ADI RMSC"
    DESCRIPTION "Ring-modulation sidechain ducking"
    VERSION 0.1.0
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Dynamics)

target_sources(adi_rmsc PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    # The DSP is the engine's own (src/adi/dsp), so the plug-in and the DAW
    # cannot drift apart. It depends on nothing but <cmath>.
    ${ADI_ROOT}/src/adi/dsp/rmsc.cpp)

target_include_directories(adi_rmsc PUBLIC ${ADI_ROOT}/src)

target_compile_definitions(adi_rmsc PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(adi_rmsc
    PRIVATE juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)

clap_juce_extensions_plugin(TARGET adi_rmsc
    CLAP_ID "com.adi.rmsc"
    CLAP_FEATURES audio-effect mixing stereo)

# The plug-in as a host sees it: buses, processBlock, parameters, state.
add_executable(adi_rmsc_tests tests/test_rmsc_plugin.cpp)
target_link_libraries(adi_rmsc_tests PRIVATE adi_rmsc juce::juce_audio_utils)
add_test(NAME adi_rmsc_tests COMMAND adi_rmsc_tests)
