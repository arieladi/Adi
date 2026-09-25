// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI Airwindows (ADR-0166, ADR-0171): the kept Airwindows algorithms as eight
// suite CLAP plug-ins, "ADI Airwindows - <group>", each holding every algorithm
// of its group with the one that plays chosen inside. The entry points are
// here rather than in the module's clap_entry so the tests can drive the
// factory without loading a library.

#pragma once

#include <clap/clap.h>

namespace adi::airwindows {

/// clap_plugin_entry's three functions.
bool entryInit(const char* pluginPath);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

/// Parameter ids, fixed for the life of a suite (ADR-0171): an automation lane
/// or a stored value never changes meaning when another algorithm is chosen.
inline constexpr clap_id kAlgorithmParamId = 0;
inline constexpr clap_id kAutoGainParamId = 1;
/// Algorithm `a`'s parameter `k` is kAlgoParamBase + a * kAlgoParamStride + k.
inline constexpr clap_id kAlgoParamBase = 100;
inline constexpr clap_id kAlgoParamStride = 64;

/// The switch between algorithms is a crossfade of this long.
inline constexpr double kCrossfadeSeconds = 0.005;

}  // namespace adi::airwindows
