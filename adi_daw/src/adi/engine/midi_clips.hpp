// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/midi_monitoring.hpp"
#include "adi/engine/realize.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"
#include <cstdint>
#include <memory>
#include <set>
#include <span>
namespace adi::engine {
// Message-thread registry; its per-track note state is used ONLY by the audio
// consumer. Shared by successive immutable source generations, never copied
// while the consumer is running. Graph publication decides which one executes.
// Contiguous generations retain held voices by persisted (clip,note,repeat)
// identity only while their authored properties still match; tempo alone does
// not release/retrigger them. Missing/ambiguous identities use release/chase.
struct MidiClipState;
struct NoteExpressionsRead;
struct MidiNoteActivity {
    std::uint32_t onsets = 0;
    bool held = false;
};
std::shared_ptr<MidiClipState> makeMidiClipState();
class MidiClips {
public:
    // Owner-only ordered union of actual event paths, ending each path at its
    // first consumer/stop. Audio effects after a consumer do not own its notes.
    MidiClips(const rows::Model&, Transport&, double rate, std::shared_ptr<MidiClipState>,
              const DeviceChainFn& eventPathFor = {});
    MidiClips(const rows::Model&,Transport&,double rate,std::shared_ptr<MidiClipState>,
              const DeviceChainFn& eventPathFor,const NoteExpressionsRead&,bool honorMonitoring=false);
    // A clip edit can retain sources from the same session, format and
    // monitoring policy. The previous generation supplies that policy. The
    // caller supplies the complete playback model and all affected tracks,
    // including the old track if the last clip moves away from it.
    MidiClips(const rows::Model&,Transport&,double rate,std::shared_ptr<MidiClipState>,
              const DeviceChainFn& eventPathFor,const NoteExpressionsRead&,
              const MidiClips* previous, const std::set<std::int64_t>& changedTracks);
    ~MidiClips();
    std::vector<Node*> sourcesFor(std::int64_t track);
    // Owned by this generation. Includes empty clips and uses the same sample
    // boundaries as the note scheduler. Overlapping or touching spans merge.
    [[nodiscard]] std::span<const MidiMonitorRange> monitorRanges(std::int64_t track) const noexcept;
    // Driver-only request, one bit per channel at the start of the current slice.
    [[nodiscard]] std::shared_ptr<std::uint16_t> pitchChaseFor(std::int64_t track) const noexcept;
    // Owner-thread read of the source's last rendered note activity. The onset counter
    // lets a meter show a note that began and ended between two frames.
    [[nodiscard]] MidiNoteActivity noteActivity(std::int64_t track) const noexcept;
    const std::vector<std::string>& problems() const noexcept;
    // Partial rendering diagnostics in problems() do not drop a clip.
    [[nodiscard]] bool clipUnavailable(std::int64_t clipId) const noexcept;
    std::uint32_t droppedNotes() const noexcept;
    std::uint64_t droppedControllers() const noexcept;

  private:
    MidiClips(const rows::Model&,Transport&,double rate,std::shared_ptr<MidiClipState>,
              const DeviceChainFn&,const NoteExpressionsRead&,bool honorMonitoring,
              const MidiClips* previous,const std::set<std::int64_t>& changedTracks);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
