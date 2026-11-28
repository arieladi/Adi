// SPDX-License-Identifier: AGPL-3.0-or-later
#include "adi/sync/midi_clock_transport.hpp"

#include <limits>

namespace adi::sync {

std::optional<MidiTransportEdge> MidiClockTransport::receive(ClockEvent event) noexcept {
    switch (event.kind) {
    case ClockKind::SongPosition:
        if (rolling_ || pending_ || !encodeSongPosition(event.tick))
            return {};
        tick_ = event.tick;
        return MidiTransportEdge{MidiTransportKind::Locate, tick_};
    case ClockKind::Start:
        if (!rolling_)
            pending_ = MidiTransportKind::Start;
        return {};
    case ClockKind::Continue:
        if (!rolling_ && !pending_)
            pending_ = MidiTransportKind::Continue;
        return {};
    case ClockKind::Clock:
        if (pending_) {
            if (*pending_ == MidiTransportKind::Start)
                tick_ = 0;
            const auto edge = MidiTransportEdge{*pending_, tick_};
            pending_.reset();
            rolling_ = true;
            return edge;
        }
        if (!rolling_ || tick_ > std::numeric_limits<std::int64_t>::max() - ticksPerClock)
            return {};
        tick_ += ticksPerClock;
        return MidiTransportEdge{MidiTransportKind::Pulse, tick_};
    case ClockKind::Stop:
        pending_.reset();
        if (!rolling_)
            return {};
        rolling_ = false;
        tick_ -= tick_ % ticksPerSongPosition;
        return MidiTransportEdge{MidiTransportKind::Stop, tick_};
    }
    return {};
}

} // namespace adi::sync
