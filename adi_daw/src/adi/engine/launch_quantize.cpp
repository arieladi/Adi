// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/engine/launch_quantize.hpp"

#include "adi/textproj.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace adi::engine::launch_quantize {
namespace {
constexpr auto ppq = textproj::kPPQ;
constexpr auto whole = textproj::kWhole;
constexpr auto high = std::numeric_limits<std::int64_t>::max();
constexpr std::array<std::int64_t, 4> bars{8, 4, 2, 1};
constexpr std::array<std::int64_t, 9> subdivision{
    2 * ppq, 4 * ppq / 3, ppq, 2 * ppq / 3, ppq / 2,
    ppq / 3, ppq / 4, ppq / 6, ppq / 8,
};

struct Grid {
    std::int64_t origin = 0;
    std::int64_t step = 0;
    std::optional<std::int64_t> nextMeter;
};

Grid gridAt(std::int64_t now, LaunchQuantization code,
            std::span<const rows::TimeSignature> meters) noexcept {
    const auto value = static_cast<std::int32_t>(code);
    if (now < 0 || value == 0 || !isLaunchQuantizationCode(value)) return {};
    if (value >= static_cast<std::int32_t>(LaunchQuantization::Half))
        return {0, subdivision[static_cast<std::size_t>(value - 5)], std::nullopt};
    const auto group = bars[static_cast<std::size_t>(value - 1)];
    const auto next = std::upper_bound(meters.begin(), meters.end(), now,
        [](std::int64_t tick, const rows::TimeSignature& meter) { return tick < meter.posTicks; });
    const auto* meter = next != meters.begin() ? &*(next - 1) : nullptr;
    Grid grid{meter ? std::max<std::int64_t>(0, meter->posTicks) : 0, group * whole, std::nullopt};
    if (next != meters.end()) grid.nextMeter = next->posTicks;
    if (meter && meter->denominator > 0 && whole % meter->denominator == 0 && meter->numerator > 0) {
        const auto beat = whole / meter->denominator;
        if (meter->numerator <= high / beat / group) grid.step = beat * meter->numerator * group;
    }
    return grid;
}
}  // namespace

std::optional<std::int64_t> nextGridTick(std::int64_t now, LaunchQuantization code,
                                         std::span<const rows::TimeSignature> meters) noexcept {
    if (now < 0) return std::nullopt;
    if (code == LaunchQuantization::None) return now;
    const auto grid = gridAt(now, code, meters);
    if (grid.step == 0) return std::nullopt;
    const auto remainder = (now - grid.origin) % grid.step;
    if (remainder == 0) return now;
    const auto until = grid.step - remainder;
    if (grid.nextMeter && *grid.nextMeter - now < until) return grid.nextMeter;
    if (until > high - now) return std::nullopt;
    return now + until;
}

std::optional<double> quantumBeats(std::int64_t now, LaunchQuantization code,
                                  std::span<const rows::TimeSignature> meters) noexcept {
    const auto grid = gridAt(now, code, meters);
    if (grid.step == 0) return std::nullopt;
    return static_cast<double>(grid.step) / static_cast<double>(ppq);
}

}  // namespace adi::engine::launch_quantize
