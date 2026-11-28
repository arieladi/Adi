// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/launch_quantization.hpp"
#include "adi/textproj.hpp"
#include <cstdint>
#include <optional>
namespace adi {
class Store;
enum class MetronomeSound : std::uint8_t { Classic, Wood, Bell };
inline constexpr auto kMetronomeEighthNoteTicks = textproj::kWhole / 8;
[[nodiscard]] constexpr bool metronomeEighthNotesFit(std::int64_t barTicks) noexcept {
    return barTicks > 0 && barTicks % kMetronomeEighthNoteTicks == 0;
}
struct MetronomeOptions {
    bool recordingOnly = false;
    bool eighthNotes = false;
    MetronomeSound sound = MetronomeSound::Classic;
    friend bool operator==(const MetronomeOptions&, const MetronomeOptions&) = default;
};
/// Saved performance state in session_state; deliberately outside undo history.
struct TransportSettings {
    bool metronome=false,loopEnabled=false,hasLoop=false;
    std::int64_t loopStartTicks=0,loopEndTicks=92252160; // four 4/4 bars
    std::int32_t countInBars = 0;               // none, 1, 2 or 4; applies only to a record start
    std::optional<std::int64_t> songStartTicks; // absent: insert marker/time selection
    bool punchIn=false,punchOut=false;         // the loop brace's start and end, independent of Loop
    MetronomeOptions metronomeOptions;
    std::optional<LaunchQuantization> launchQuantization = std::nullopt;
    bool followActionsEnabled = true;
    friend bool operator==(const TransportSettings&,const TransportSettings&)=default;
};
TransportSettings readTransportSettings(const Store&);
}
