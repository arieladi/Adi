// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "adi/engine/realize.hpp"
#include "adi/engine/transport.hpp"
#include "adi/store_rows.hpp"
#include <memory>
namespace adi::engine {
// Message-thread registry; its per-track note state is used ONLY by the audio
// consumer. Shared by successive immutable source generations, never copied
// while the consumer is running. Graph publication decides which one executes.
// Contiguous generations retain held voices by persisted (clip,note,repeat)
// identity only while their authored properties still match; tempo alone does
// not release/retrigger them. Missing/ambiguous identities use release/chase.
struct MidiClipState;
std::shared_ptr<MidiClipState> makeMidiClipState();
class MidiClips {
public:
    // Owner-only ordered union of actual event paths, ending each path at its
    // first consumer/stop. Audio effects after a consumer do not own its notes.
    MidiClips(const rows::Model&, Transport&, double rate, std::shared_ptr<MidiClipState>,
              const DeviceChainFn& eventPathFor = {});
    ~MidiClips();
    std::vector<Node*> sourcesFor(std::int64_t track);
    const std::vector<std::string>& problems() const noexcept;
    std::uint32_t droppedNotes() const noexcept;
    std::uint64_t droppedControllers() const noexcept;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};
}
