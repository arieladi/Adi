// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
namespace adi {
class Store;
/// Saved performance state in session_state; deliberately outside undo history.
struct TransportSettings {
    bool metronome=false,loopEnabled=false,hasLoop=false;
    std::int64_t loopStartTicks=0,loopEndTicks=92252160; // four 4/4 bars
    friend bool operator==(const TransportSettings&,const TransportSettings&)=default;
};
TransportSettings readTransportSettings(const Store&);
}
