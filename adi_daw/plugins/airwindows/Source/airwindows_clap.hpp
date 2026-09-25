// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADI Airwindows (ADR-0166): the kept Airwindows effects, each its own CLAP
// plug-in, from one binary. The entry points are here rather than in the
// module's clap_entry so the tests can drive the factory without loading a
// library.

#pragma once

#include <clap/clap.h>

namespace adi::airwindows {

/// clap_plugin_entry's three functions.
bool entryInit(const char* pluginPath);
void entryDeinit();
const void* entryGetFactory(const char* factoryId);

/// The parameter id auto gain has on every effect. Airwindows' own parameters
/// keep their indices as ids (0, 1, ...); this sits clear of them.
inline constexpr clap_id kAutoGainParamId = 1000;

}  // namespace adi::airwindows
