// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/midi_memory.hpp"
#include "adi/ops.hpp"
namespace adi::capture {
struct MidiSpec {
    std::int64_t clip=0,positionTicks=0,firstSample=0,endSample=0;
    std::string name="Captured MIDI";
    bool closeHeldNotes=true; // close at explicit end with release velocity zero
};
struct PreparedMidi {
    // One capture.createMidi request with explicit clip/stream identities and
    // validated native binary images; each stream is written once.
    std::vector<OpRequest> requests;
    std::size_t notes=0,controllerLanes=0,closedHeldNotes=0;
    std::optional<std::int64_t> head;
private:
    const Store* owner_=nullptr;
    std::string client_;
    friend std::optional<PreparedMidi> prepareMidi(Store&,const engine::MidiMemorySnapshot&,const MidiSpec&,std::string&);
    friend CommitResult commitMidi(Store&,const PreparedMidi&,const CommitOptions&);
};
// Owner only, one coherent read, no writes/reservations. The explicit source
// range contains every retained event and ends no later than snapshot.endSample.
// Source timestamps are independent of project placement. Uses step tempos and
// verifies every placed sample roundtrip. Refuses truncated/ambiguous ownership,
// expressions and velocities that cannot roundtrip through the existing ANOT
// bytes. CC values remain double. Native CC initial chase/cross-stream ordering
// follows existing playback; raw input memory retains its exact serial order.
std::optional<PreparedMidi> prepareMidi(Store&,const engine::MidiMemorySnapshot&,const MidiSpec&,std::string& error);
// Same still-open Store/client and captured head (including NULL). Caller
// retains document generation across async work. Optional callback can veto
// the transaction; it cannot replace the captured head guard.
CommitResult commitMidi(Store&,const PreparedMidi&,const CommitOptions& options={});
std::span<const OpDescriptor> midiOps();
} // namespace adi::capture
