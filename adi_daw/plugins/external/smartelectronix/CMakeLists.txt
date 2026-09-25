# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bram @ Smartelectronix's plug-ins as CLAP (ADR-0166). Upstream (GPL-3.0,
# github.com/bdejong/smartelectronix) builds VST3, AU and a standalone with
# JUCE 8 and no CLAP. This wrapper includes upstream's tree UNCHANGED and adds
# a CLAP target to each plug-in with clap-juce-extensions -- no patch to keep.
#
#   cmake -S plugins/external/smartelectronix -B <build> -DSMX_SOURCE=<checkout>

cmake_minimum_required(VERSION 3.22)
set(SMX_SOURCE "" CACHE PATH "The smartelectronix checkout (tools/fetch_plugins.sh)")
if (NOT EXISTS "${SMX_SOURCE}/cmake/CommonSettings.cmake")
    message(FATAL_ERROR "SMX_SOURCE does not point at a smartelectronix checkout: '${SMX_SOURCE}'")
endif()
include(${SMX_SOURCE}/cmake/CommonSettings.cmake)
project(adi_smartelectronix_clap VERSION 1.0.0)

add_subdirectory(${SMX_SOURCE} smartelectronix)   # upstream's own tree, its JUCE 8 included

include(FetchContent)
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        55525c9858d4b25687be7759a5e0f70eccef218e   # 2026-09-12, MIT
    GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(clap-juce-extensions)

# The id says whose BUILD this is: upstream ships no CLAP, and an id under its
# name would claim a release it never made.
function(adi_smx_clap target id)
    clap_juce_extensions_plugin(TARGET ${target} CLAP_ID "com.adi.smartelectronix.${id}" CLAP_FEATURES ${ARGN})
endfunction()

adi_smx_clap(Smexoscope            smexoscope        analyzer utility stereo)
adi_smx_clap(AnechoicRoomSimulator anechoicroom      audio-effect utility)
adi_smx_clap(Bitmurderer           bitmurderer       audio-effect distortion)
adi_smx_clap(Bouncy                bouncy            audio-effect delay)
adi_smx_clap(CrazyIvan             crazyivan         audio-effect distortion)
adi_smx_clap(Cyanide2              cyanide2          audio-effect distortion)
adi_smx_clap(H2O                   h2o               audio-effect compressor)
adi_smx_clap(Madshifta             madshifta         audio-effect pitch-shifter delay)
adi_smx_clap(OnePingOnly           onepingonly       instrument synthesizer)
adi_smx_clap(SupaPhaser            supaphaser        audio-effect phaser)
adi_smx_clap(SupaTrigga            supatrigga        audio-effect glitch)
