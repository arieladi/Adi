// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native analog group summing: the flavours (ADR-0173, ADR-0174).
//
// A flavour is an Airwindows console SYSTEM: a channel half played on each
// child of a group, and a buss half played on the group's sum. Eight
// channel/buss pairs. EveryConsole's six systems are not among them: at the
// pinned commit it cannot select a system (summing_flavors.cpp).
//
// The KEY is what `group_summing.flavor` stores, and it is permanent: never
// renamed, never reused. The order of this table means nothing to a file
// (ADR-0173 d3). There is deliberately no CHECK over the keys in the schema:
// a minor adds only whole objects (ADR-0144), so a CHECK would freeze the list.
// The ops refuse an unknown key; a reader names one and plays none.
//
// No Airwindows code here: the ops validate against this table, and only the
// engine's summing.cpp builds the algorithms.

#pragma once

#include <span>
#include <string_view>

namespace adi {

struct SummingFlavor {
    std::string_view key;        ///< stored in the file; permanent
    std::string_view name;       ///< what the flavour menu shows
    std::string_view channel;    ///< the Airwindows algorithm of the channel half
    std::string_view buss;       ///< and of the buss half
};

[[nodiscard]] std::span<const SummingFlavor> summingFlavors() noexcept;
[[nodiscard]] const SummingFlavor* findSummingFlavor(std::string_view key) noexcept;

inline constexpr std::string_view kDefaultSummingFlavor = "console9";
inline constexpr double kSummingDriveMinDb = -12.0;
inline constexpr double kSummingDriveMaxDb = 24.0;

}  // namespace adi
