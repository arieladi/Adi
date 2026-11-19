// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/ops.hpp"
#include <map>
#include <string>
#include <string_view>
namespace adi {
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
[[nodiscard]] std::span<const OpDescriptor> trackRecordingOps();
}
