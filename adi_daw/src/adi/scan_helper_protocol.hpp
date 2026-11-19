// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string_view>

namespace adi::scan {
// The first byte of a successful metadata packet doubles as the probe-phase
// marker. The helper flushes it before opening the module. No extra bytes are
// added to the JSON packet or the bounded output and cache formats.
inline constexpr char kModuleOpening = '{';

// Dedicated helper command, independent from the plug-in metadata schema and
// registry version. A future owner must reject unknown versions/fields before
// using this structural deployment receipt. No plug-in compatibility is proved.
inline constexpr unsigned kHelperSelfTestProtocol = 1;
inline constexpr std::string_view kHelperSelfTestKind = "adi-plugin-scanner-self-test";
inline constexpr std::size_t kMaximumHelperSelfTestPacket = 1024;
} // namespace adi::scan
