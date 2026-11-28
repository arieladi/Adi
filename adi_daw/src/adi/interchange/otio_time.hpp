// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/time_base.hpp"

#include <nlohmann/json.hpp>

namespace adi::interchange::otio {

struct RationalTime {
    std::int64_t valueNumerator = 0;
    std::int64_t valueDenominator = 1;
    time_base::Rate rate{24, 1};
};

struct TimeRange {
    RationalTime start;
    RationalTime duration;
};

/// Read the supported OTIO .1 objects. Integer values retain their entire
/// signed 64-bit range. Floating values use the JSON number's decimal form.
/// The common NTSC rate spellings map to the exact denominator 1001.
[[nodiscard]] std::optional<RationalTime> readTime(const nlohmann::json &);
[[nodiscard]] std::optional<TimeRange> readRange(const nlohmann::json &);

/// A write succeeds only if the emitted OTIO value reads back with the same
/// rational value and rate. The top-level converter owns any loss reporting.
[[nodiscard]] std::optional<nlohmann::json> writeTime(const RationalTime &);
[[nodiscard]] std::optional<nlohmann::json> writeRange(const TimeRange &);

/// Convert once at the placement boundary. Exact rejects a fractional
/// result. All modes reject an unrepresentable ratio or output tick.
[[nodiscard]] std::optional<std::int64_t>
toNanoseconds(const RationalTime &, time_base::Rounding) noexcept;

} // namespace adi::interchange::otio
