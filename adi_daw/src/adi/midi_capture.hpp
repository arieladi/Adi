// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "adi/engine/midi_memory.hpp"
#include "adi/ops.hpp"
namespace adi::capture {
// The physical input parser sends a channel wheel as an unowned pitch event.
// Unlike per-note expression, it can create a controller-only MIDI clip.
bool isChannelPitchWheel(const engine::Event&) noexcept;
double channelPitchWheelValue(double semitones) noexcept;

struct MidiSpec {
    std::int64_t clip=0,positionTicks=0,firstSample=0,endSample=0;
    std::string name="Captured MIDI";
    bool closeHeldNotes=true; // close at explicit end with release velocity zero
    bool liveHistory=false; // allow a bounded live suffix and releases whose attacks predate it
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
    friend std::optional<PreparedMidi> prepareMidiBatch(Store&,std::span<const engine::MidiMemorySnapshot>,
                                                        std::span<const MidiSpec>,std::string&);
    friend CommitResult commitMidi(Store&,const PreparedMidi&,const CommitOptions&);
};
// Owner only, one coherent read, no writes/reservations. The explicit source
// range contains every retained event and ends no later than snapshot.endSample.
// Source timestamps are independent of project placement. The shared tempo map
// verifies every placed sample roundtrip. Capture validates source history and
// note ownership, and refuses velocities that cannot roundtrip through ANOT.
// Per-note expression uses native AEXP precision. CC values remain double.
// Native CC initial chase and cross-stream ordering follow existing playback.
// Raw input memory retains its exact serial order.
std::optional<PreparedMidi> prepareMidi(Store&,const engine::MidiMemorySnapshot&,const MidiSpec&,std::string& error);
std::optional<PreparedMidi> prepareMidiBatch(Store&,std::span<const engine::MidiMemorySnapshot>,
                                             std::span<const MidiSpec>,std::string& error);
// Same still-open Store/client and captured head (including NULL). Caller
// retains document generation across async work. Optional callback can veto
// the transaction; it cannot replace the captured head guard.
CommitResult commitMidi(Store&,const PreparedMidi&,const CommitOptions& options={});
std::span<const OpDescriptor> midiOps();
} // namespace adi::capture
