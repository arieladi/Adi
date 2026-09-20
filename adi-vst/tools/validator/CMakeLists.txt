cmake_minimum_required(VERSION 3.15)
project(VitalValidator VERSION 1.0.0 LANGUAGES CXX)

# Uses the stock JUCE download, NOT Vital's patched third_party/JUCE.
# The validator is a host: it loads the plugin as a black box over the VST3 ABI,
# so it neither needs nor wants Vital's DSP patches.
set(JUCE_PATH "C:/Users/Adi/Documents/JUCE/JUCE" CACHE PATH "Path to JUCE")
add_subdirectory(${JUCE_PATH} ${CMAKE_BINARY_DIR}/JUCE)

juce_add_console_app(VitalValidator PRODUCT_NAME "VitalValidator")

# JUCE's CMake API does not emit JuceHeader.h unless asked -- unlike Projucer,
# which always generates one. Without this, Main.cpp's #include fails.
juce_generate_juce_header(VitalValidator)

target_sources(VitalValidator PRIVATE Main.cpp)

target_compile_definitions(VitalValidator PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1)

target_link_libraries(VitalValidator PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
