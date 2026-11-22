// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
namespace adi {
class Store;
enum class LaunchQuantization : std::int32_t {
    None = 0, EightBars, FourBars, TwoBars, Bar, Half, HalfTriplet,
    Quarter, QuarterTriplet, Eighth, EighthTriplet, Sixteenth, SixteenthTriplet, ThirtySecond,
};
[[nodiscard]] constexpr bool isLaunchQuantizationCode(std::int64_t code) noexcept {
    return code >= 0 && code <= static_cast<std::int64_t>(LaunchQuantization::ThirtySecond);
}
/// Saved performance state in session_state; deliberately outside undo history.
struct TransportSettings {
    bool metronome=false,loopEnabled=false,hasLoop=false;
    std::int64_t loopStartTicks=0,loopEndTicks=92252160; // four 4/4 bars
    std::int32_t countInBars = 0;               // none, 1, 2 or 4; applies only to a record start
    std::optional<std::int64_t> songStartTicks; // absent: insert marker/time selection
    LaunchQuantization launchQuantization = LaunchQuantization::Bar;
    friend bool operator==(const TransportSettings&,const TransportSettings&)=default;
};
TransportSettings readTransportSettings(const Store&);
}
