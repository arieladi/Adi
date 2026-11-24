# Original diagnostics from the director's research; MIT notices remain beside sources.
# Include after adi_warnings and ADI_THIRD_PARTY are defined. No downloads or installs.
include_guard(GLOBAL)

if(NOT WIN32)
    message(STATUS "Research plug-in diagnostics unavailable here: the mapped-trace/loader platform layer currently requires Windows")
    set(ADI_RESEARCH_PROBES_AVAILABLE FALSE)
    return()
endif()
if(NOT TARGET adi_warnings)
    message(FATAL_ERROR "Research probe targets require adi_warnings")
endif()

set(_adi_research_plugins "${CMAKE_CURRENT_LIST_DIR}")
set(_adi_research_vst3_sdk "${ADI_THIRD_PARTY}/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK")
set(_adi_research_clap_headers "${ADI_THIRD_PARTY}/clap/include")
foreach(_adi_research_header IN ITEMS
        "${_adi_research_vst3_sdk}/pluginterfaces/vst/ivstprocesscontext.h"
        "${_adi_research_clap_headers}/clap/clap.h")
    if(NOT EXISTS "${_adi_research_header}")
        message(FATAL_ERROR "Research probe dependency absent: ${_adi_research_header}. Fetch the project's pinned hosting dependencies first.")
    endif()
endforeach()

function(_adi_research_strict target headers)
    target_link_libraries(${target} PRIVATE adi_warnings)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} SYSTEM PRIVATE "${headers}")
    # The diagnostics are acceptance fixtures: their own warnings are always errors,
    # including when the surrounding development build leaves ADI_WERROR off.
    if(MSVC)
        target_compile_options(${target} PRIVATE /WX)
    else()
        target_compile_options(${target} PRIVATE -Werror)
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "Tests/Research probes")
endfunction()

function(_adi_research_module target source name suffix headers)
    add_library(${target} MODULE "${_adi_research_plugins}/${source}")
    _adi_research_strict(${target} "${headers}")
    set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME "${name}" SUFFIX "${suffix}")
endfunction()

# These sources use public interface declarations/IDs only. They need no separate
# SDK implementation objects; all VST3 headers match the host's JUCE dependency.
# A distinct basename keeps parallel MSVC links from sharing import/debug files.
_adi_research_module(adi_research_clap_probe clap/adi_clap_probe.cpp ADIEngineClapProbe .clap "${_adi_research_clap_headers}")
_adi_research_module(adi_research_vst3_probe vst3/adi_vst3_probe.cpp ADIEngineProbe .vst3 "${_adi_research_vst3_sdk}")
_adi_research_module(adi_research_time_probe time/probe/adi_time_probe.cpp ADITimeProbe .vst3 "${_adi_research_vst3_sdk}")
_adi_research_module(adi_research_scan_probe scanner/scan_probe.cpp ScanProbe .vst3 "${_adi_research_vst3_sdk}")
add_custom_target(adi_research_probe_targets DEPENDS adi_research_clap_probe
    adi_research_vst3_probe adi_research_time_probe adi_research_scan_probe)

# On Windows the host accepts these flat PE modules. Never construct a relative
# path or assume the single-config layout in an acceptance harness.
set(ADI_RESEARCH_TIME_PROBE_PATH "$<TARGET_FILE:adi_research_time_probe>")
set(ADI_RESEARCH_PROBES_AVAILABLE TRUE)
option(ADI_RESEARCH_PROBE_SELFTESTS "Build standalone diagnostic fixture self-tests" ON)
if(ADI_RESEARCH_PROBE_SELFTESTS)
    foreach(_adi_research_kind IN ITEMS clap vst3 time)
        set(_adi_research_source "${_adi_research_kind}/selftest.cpp")
        set(_adi_research_headers "${_adi_research_vst3_sdk}")
        if(_adi_research_kind STREQUAL "clap")
            set(_adi_research_headers "${_adi_research_clap_headers}")
        elseif(_adi_research_kind STREQUAL "time")
            set(_adi_research_source "time/probe/selftest.cpp")
        endif()
        set(_adi_research_test "adi_research_${_adi_research_kind}_selftest")
        set(_adi_research_module "adi_research_${_adi_research_kind}_probe")
        add_executable(${_adi_research_test} "${_adi_research_plugins}/${_adi_research_source}")
        _adi_research_strict(${_adi_research_test} "${_adi_research_headers}")
        set_target_properties(${_adi_research_test} PROPERTIES OUTPUT_NAME "${_adi_research_kind}_selftest")
        add_dependencies(${_adi_research_test} ${_adi_research_module})
        add_dependencies(adi_research_probe_targets ${_adi_research_test})
        add_test(NAME ${_adi_research_test}
            COMMAND "$<TARGET_FILE:${_adi_research_test}>" "$<TARGET_FILE:${_adi_research_module}>")
        set_tests_properties(${_adi_research_test} PROPERTIES TIMEOUT 60
            LABELS "research;fixture" WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_adi_research_test}>")
    endforeach()
endif()
