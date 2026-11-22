// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "adi/sync/midi_sync_wire.hpp"

namespace adi::sync {

enum class MidiTransportKind { Locate, Start, Continue, Pulse, Stop };

struct MidiTransportEdge {
    MidiTransportKind kind;
    std::int64_t tick;
    bool operator==(const MidiTransportEdge &) const = default;
};

/// Driver-owned input state. Idle clocks still go to the independent tempo
/// follower, while this state advances only after Start or Continue.
class MidiClockTransport {
public:
    [[nodiscard]] std::optional<MidiTransportEdge> receive(ClockEvent event) noexcept;
    void reset() noexcept { *this = MidiClockTransport{}; }

private:
    std::optional<MidiTransportKind> pending_;
    std::int64_t tick_ = 0;
    bool rolling_ = false;
};

} // namespace adi::sync
