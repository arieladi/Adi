// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <map>
#include <string>
#include <string_view>
namespace adi {
// SC-249: a new audio track requires an explicit choice to monitor its input.
inline constexpr std::int64_t kNewAudioTrackMonitorMode = 0; // Off
inline constexpr std::int64_t initialTrackMonitorMode(std::string_view kind) noexcept {
    if (kind == "audio") return kNewAudioTrackMonitorMode;
    return kind == "midi" || kind == "instrument" ? 2 : 0; // Auto for MIDI and instruments
}

struct TrackRecordingOptions {
    bool recordEnabled=true,playEnabled=false,keepMonitoringLatency=true;
    std::string recordGrid="1/16",playGrid="1/16";
    double playForgivenessMs=0;
    friend bool operator==(const TrackRecordingOptions&,const TrackRecordingOptions&)=default;
};
// Missing optional rows, including read-only older schemas, use fixed defaults.
[[nodiscard]] std::map<std::int64_t,TrackRecordingOptions> readTrackRecordingOptions(const Store&);
// Explicit registry values captured by the caller into the new-track batch.
// No ambient settings read takes place when an op is replayed.
[[nodiscard]] std::vector<OpRequest> trackRecordingDefaults(std::int64_t track,
    std::string_view recordChoice,bool keepMonitoringLatency);
// A stored Rec-Q or Play-Q subdivision, expressed in project ticks. Unknown choices return zero.
[[nodiscard]] std::int64_t trackRecordingGridTicks(std::string_view grid) noexcept;
[[nodiscard]] std::span<const OpDescriptor> trackRecordingOps();
}
