// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>

namespace adi::time_base {
enum class Rounding { Exact, Floor, Nearest };

struct Rate {
    std::uint32_t numerator = 0, denominator = 1;
};

// Compute value * numerator / denominator without floating point. The ratio
// must be positive. Exact refuses a remainder. Nearest rounds ties away from
// zero. All conversions refuse results outside the signed 64-bit domain.
[[nodiscard]] std::optional<std::int64_t> rescale(std::int64_t value, std::int64_t numerator,
                                                  std::int64_t denominator, Rounding) noexcept;
[[nodiscard]] std::optional<std::int64_t> framesToNanoseconds(std::int64_t frames, Rate,
                                                              Rounding) noexcept;
[[nodiscard]] std::optional<std::int64_t> nanosecondsToFrames(std::int64_t ns, Rate,
                                                              Rounding) noexcept;

struct Timecode {
    int hours = 0, minutes = 0, seconds = 0, frames = 0;
    bool operator==(const Timecode &) const = default;
};

// Linear frame indices and SMPTE labels within one 24-hour day. Drop-frame is
// defined here at 30000/1001 and 60000/1001. A skipped label is invalid.
// Callers with a longer timeline retain its day count separately.
[[nodiscard]] std::optional<std::int64_t> timecodeToFrame(Timecode, Rate, bool dropFrame) noexcept;
[[nodiscard]] std::optional<Timecode> frameToTimecode(std::int64_t frame, Rate, bool dropFrame) noexcept;
} // namespace adi::time_base
