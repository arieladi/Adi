// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
namespace adi {
class Store;
/// Saved performance state in session_state; deliberately outside undo history.
struct TransportSettings {
    bool metronome=false,loopEnabled=false,hasLoop=false;
    std::int64_t loopStartTicks=0,loopEndTicks=92252160; // four 4/4 bars
    std::int32_t countInBars = 0;               // none, 1, 2 or 4; applies only to a record start
    std::optional<std::int64_t> songStartTicks; // absent: insert marker/time selection
    friend bool operator==(const TransportSettings&,const TransportSettings&)=default;
};
TransportSettings readTransportSettings(const Store&);
}
